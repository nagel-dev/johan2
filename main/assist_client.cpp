#include "assist_client.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "assist";
static constexpr bool kDirectReplies =
#ifdef CONFIG_JOHAN2_TTS_DIRECT
    true;
#else
    false;
#endif
static constexpr bool kListeningCue =
#ifdef CONFIG_JOHAN2_LISTENING_CUE
    true;
#else
    false;
#endif
static constexpr bool kStopListener =
#ifdef CONFIG_JOHAN2_STOP_LISTENER
    true;
#else
    false;
#endif

void AssistClient::start(AudioOutput* audio) {
    if (!strlen(CONFIG_JOHAN2_HA_TOKEN)) {
        ESP_LOGI(TAG, "Assist disabled: set HA token in local sdkconfig");
        return;
    }
#if CONFIG_JOHAN2_TTS_DIRECT || CONFIG_JOHAN2_LISTENING_CUE
    if (audio) tts_.init(*audio);
#endif
    if (!kDirectReplies && !strlen(CONFIG_JOHAN2_HA_REPLY_PLAYER)) {
        ESP_LOGW(TAG, "Spoken replies disabled: enable direct replies or set reply media_player in menuconfig");
    }
    // Never emit WebSocket payloads or authentication data at debug level.
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    audio_queue_ = xQueueCreate(32, sizeof(Audio)); // At most 512 ms, including bounded send backpressure.
    messages_ = xQueueCreate(32, sizeof(Message));
    ESP_ERROR_CHECK(audio_queue_ && messages_ ? ESP_OK : ESP_ERR_NO_MEM);
    esp_websocket_client_config_t cfg{};
    cfg.uri = CONFIG_JOHAN2_HA_WS_URL;
    cfg.task_name = "ha_ws";
    cfg.task_stack = 6144;
    cfg.buffer_size = 4096;
    cfg.reconnect_timeout_ms = 10000;
    cfg.network_timeout_ms = 2000;
    cfg.enable_close_reconnect = true;
    socket_ = esp_websocket_client_init(&cfg);
    ESP_ERROR_CHECK(socket_ ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_websocket_register_events(socket_, WEBSOCKET_EVENT_ANY, event, this));
    ESP_ERROR_CHECK(xTaskCreate(task, "ha_assist", 8192, this, 3, nullptr) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}

void AssistClient::submit(const int16_t* samples, size_t count) {
    if (!accepting_.load() || !count || count > SAMPLES) return;
    Audio block{};
    block.count = count;
    memcpy(block.samples, samples, count * sizeof(int16_t));
    if (xQueueSend(audio_queue_, &block, 0) != pdTRUE) {
        // A gap can corrupt a command. Restart the session instead of queuing stale audio.
        accepting_.store(false);
        transport_failed_.store(true);
    }
}

void AssistClient::event(void* context, esp_event_base_t, int32_t id, void* raw) {
    auto& self = *static_cast<AssistClient*>(context);
    Message message{id, nullptr};
    if (id == WEBSOCKET_EVENT_DATA) {
        const auto& data = *static_cast<esp_websocket_event_data_t*>(raw);
        if (data.op_code != 1 && data.op_code != 0) return;
        if (data.op_code == 1 && data.payload_offset == 0) {
            self.received_.clear();
            self.receiving_ = true;
        }
        if (!self.receiving_) return;
        if (data.data_len < 0 || self.received_.size() + data.data_len > 16384) {
            ESP_EARLY_LOGW(TAG, "WS message too large: %d bytes buffered, %d more", (int)self.received_.size(), data.data_len);
            self.receiving_ = false;
            self.transport_failed_.store(true);
            return;
        }
        self.received_.append(data.data_ptr, data.data_len);
        if (!data.fin || data.payload_offset + data.data_len != data.payload_len) return;
        self.receiving_ = false;
        // LLM streaming sends many tiny progress events. Nothing uses them, and a burst can overflow the queue.
        if (self.received_.find("\"intent-progress\"") != std::string::npos) return;
        message.text = strdup(self.received_.c_str());
        if (!message.text) { self.transport_failed_.store(true); return; }
    } else if (id == WEBSOCKET_EVENT_CONNECTED || id == WEBSOCKET_EVENT_DISCONNECTED) {
        if (id == WEBSOCKET_EVENT_DISCONNECTED) ESP_EARLY_LOGW(TAG, "WS disconnected");
        self.accepting_.store(false);
        self.command_active_.store(false);
        self.received_.clear();
        self.receiving_ = false;
    } else {
        return;
    }
    if (xQueueSend(self.messages_, &message, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "WS message queue full, event %d dropped", (int)id);
        free(message.text);
        self.transport_failed_.store(true);
    }
}

bool AssistClient::send_text(const std::string& text) {
    return esp_websocket_client_send_text(socket_, text.data(), text.size(), pdMS_TO_TICKS(500))
           == static_cast<int>(text.size());
}

void AssistClient::publish_state(const char* state, bool force) {
    const bool changed = strcmp(assist_state_, state) != 0;
    assist_state_ = state;
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
    if ((changed || force) && protocol_.authenticated) {
        state_refresh_at_ = esp_timer_get_time() + 15000000;
        if (!send_text(protocol_.state_update(state))) ESP_LOGW(TAG, "State update delayed");
    }
#else
    (void)changed;
    (void)force;
#endif
}

// Spoken "stop"/"cancel" is never forwarded to the conversation agent.
static bool is_stop_phrase(const std::string& text) {
    std::string word;
    for (unsigned char c : text) if (isalnum(c)) word += static_cast<char>(tolower(c));
    static const char* const phrases[] = {"stop", "stopp", "cancel", "annuleer", "annuleren", "laatmaar", "stil"};
    for (const char* phrase : phrases) if (word == phrase) return true;
    return false;
}

// STT hallucination on noise/silence: a bare "Context:" label or the leaked STT prompt is not a command.
static bool is_stt_artifact(const std::string& text) {
    std::string word;
    for (unsigned char c : text) if (isalnum(c)) word += static_cast<char>(tolower(c));
    return word == "context" || word == "contexts" ||
           word.find("smarthomeusertalkingtohomeassistant") != std::string::npos;
}

void AssistClient::cancel_command(bool stop_playback) {
    followup_ = false;
    followup_deadline_ = 0;
    remote_start_pending_ = false;
    reset_audio();
    if (stop_playback) {
        cancel_local_announcement();
        if (direct_playing_) tts_.cancel();
    }
    if (protocol_.stop_running && !send_text(protocol_.cancel_stop())) transport_failed_.store(true);
    if (protocol_.running && !send_text(protocol_.cancel_run())) transport_failed_.store(true);
    retry_at_ = 0;
    publish_state("idle");
}

void AssistClient::reset_audio(bool preserve_end_cue) {
    cue_.cancel(preserve_end_cue);
    if (cue_.playing && (!preserve_end_cue || !cue_.playing_end)) tts_.cancel();
    if (!preserve_end_cue) deferred_reply_.clear();
#if CONFIG_JOHAN2_WAKE_VAD
    if (wake_gating_ && wake_gate_.seen_frames)
        ESP_LOGI(TAG, "Wake VAD: sent %" PRIu32 "/%" PRIu32 " frames", wake_gate_.sent_frames, wake_gate_.seen_frames);
    wake_gate_reset(&wake_gate_);
    wake_gating_ = false;
#endif
    accepting_.store(false);
    command_active_.store(false);
    xQueueReset(audio_queue_);
}

void AssistClient::request_listening_cue(bool ending) {
    if (!kListeningCue || !protocol_.listening_cue_enabled || !cue_.request(protocol_.run_id, esp_timer_get_time(), wake_tail_until_, ending)) return;
    // Mute music before scheduling playback, including the first wake event.
    if (!ending) command_active_.store(true);
    xQueueReset(audio_queue_);
}

void AssistClient::forward_to_player() {
    const auto request = protocol_.announcement(CONFIG_JOHAN2_HA_REPLY_PLAYER, CONFIG_JOHAN2_HA_WS_URL);
    if (request.empty()) {
        ESP_LOGW(TAG, "Cannot route spoken reply; check reply player and HA TTS output");
    } else {
        announcement_deadline_ = esp_timer_get_time() + 90000000;
        ESP_LOGI(TAG, "Sending spoken reply to Music Assistant player");
        if (!send_text(request)) transport_failed_.store(true);
    }
}

void AssistClient::report_local_announcement(const std::string& token, const char* state) {
    if (protocol_.authenticated && esp_websocket_client_is_connected(socket_) &&
        !send_text(protocol_.local_announcement_status(token, state)))
        ESP_LOGW(TAG, "Local announcement status could not be sent");
}

void AssistClient::finish_local_announcement(bool success) {
    report_local_announcement(local_announcement_token_, success ? "done" : "error");
    ESP_LOGI(TAG, "Local announcement %s", success ? "finished" : "failed or cancelled");
    local_announcement_url_.clear();
    local_announcement_token_.clear();
    local_announcement_playing_ = local_announcement_cancelled_ = false;
}

void AssistClient::cancel_local_announcement() {
    if (local_announcement_token_.empty()) return;
    if (local_announcement_playing_) {
        local_announcement_cancelled_ = true;
        tts_.cancel();
    } else {
        finish_local_announcement(false);
    }
}

// Shows what Assist heard or answered on HomeDash (via its proxy). Best effort, off the websocket task.
struct HomeDashPost { std::string path; std::string text; };

static void post_homedash_text(void* context) {
    std::unique_ptr<HomeDashPost> post(static_cast<HomeDashPost*>(context));
    std::string url = CONFIG_JOHAN2_HOMEDASH_TRANSCRIPT_URL;
    const auto slash = url.rfind('/');
    if (slash == std::string::npos) { vTaskDelete(nullptr); return; }
    url.resize(slash + 1);
    url += post->path;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 3000;
    if (auto client = esp_http_client_init(&config)) {
        esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
        esp_http_client_set_post_field(client, post->text.c_str(), post->text.size());
        if (esp_http_client_perform(client) != ESP_OK) ESP_LOGW(TAG, "Could not send %s to HomeDash", post->path.c_str());
        esp_http_client_cleanup(client);
    }
    vTaskDelete(nullptr);
}

static void send_to_homedash(const char* path, const std::string& text) {
    if (!CONFIG_JOHAN2_HOMEDASH_TRANSCRIPT_URL[0] || text.empty()) return;
    auto* post = new HomeDashPost{path, text};
    if (xTaskCreate(post_homedash_text, "hd_post", 4096, post, 3, nullptr) != pdPASS) delete post;
}

void AssistClient::task(void* context) { static_cast<AssistClient*>(context)->run(); }

bool AssistClient::send_audio(const int16_t* samples, size_t count) {
    uint8_t packet[1 + SAMPLES * 2];
    packet[0] = protocol_.active_handler();
    for (size_t i = 0; i < count; ++i) {
        const uint16_t sample = static_cast<uint16_t>(samples[i]);
        packet[1 + i * 2] = sample & 0xff;
        packet[2 + i * 2] = sample >> 8;
    }
    const int length = 1 + count * 2;
    const int sent = esp_websocket_client_send_bin(socket_, reinterpret_cast<char*>(packet), length,
                                                   pdMS_TO_TICKS(250));
    if (sent != length) ESP_LOGW(TAG, "Microphone transport write failed: %d/%d bytes", sent, length);
    return sent == length;
}

#if CONFIG_JOHAN2_WAKE_VAD
static bool wake_speech(void* context, int16_t* frame) {
    return vad_process(static_cast<vad_handle_t>(context), frame, 16000, 10) == VAD_SPEECH;
}
#endif

void AssistClient::run() {
#if CONFIG_JOHAN2_WAKE_VAD
    wake_vad_ = vad_create(VAD_MODE_0);
    if (!wake_vad_) ESP_LOGW(TAG, "Wake VAD unavailable; streaming continuously");
#endif
    ESP_ERROR_CHECK(esp_websocket_client_start(socket_));
    while (true) {
        const bool failed = transport_failed_.exchange(false);
        if (failed ||
            (protocol_.running && esp_timer_get_time() >= run_deadline_) ||
            (protocol_.announcement_pending && esp_timer_get_time() >= announcement_deadline_)) {
            ESP_LOGW(TAG, "Restarting Assist: %s (state %s)",
                     failed ? "transport failure" : "pipeline/playback timeout", assist_state_);
            publish_state("error");
            cancel_local_announcement();
            reset_audio();
            // Must be called outside the WebSocket callback/task.
            esp_websocket_client_stop(socket_);
            protocol_.reset();
            remote_start_pending_ = followup_ = false;
            followup_deadline_ = 0;
            Message stale;
            while (xQueueReceive(messages_, &stale, 0) == pdTRUE) free(stale.text);
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_ERROR_CHECK(esp_websocket_client_start(socket_));
        }
        Message message;
        if (xQueueReceive(messages_, &message, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (message.type == WEBSOCKET_EVENT_CONNECTED || message.type == WEBSOCKET_EVENT_DISCONNECTED) {
                cancel_local_announcement();
                reset_audio();
                protocol_.reset();
                remote_start_pending_ = followup_ = false;
                followup_deadline_ = 0;
                publish_state("offline");
                ESP_LOGI(TAG, "%s", message.type == WEBSOCKET_EVENT_CONNECTED ? "HA connected" : "HA disconnected");
            } else if (message.text) {
                const bool was_streaming = protocol_.any_streaming();
                const bool was_command_capture = protocol_.command_active && protocol_.streaming;
                const auto result = protocol_.receive(message.text);
                free(message.text);
                if (protocol_.last_event() == "run-start" && !followup_ &&
                    strcmp(assist_state_, "starting") != 0)
                    run_deadline_ = esp_timer_get_time() + 310000000;
                if (protocol_.running && result != AssistProtocol::Event::Authenticate && !protocol_.last_event().empty() &&
                    protocol_.last_event() != "intent-progress")
                    ESP_LOGI(TAG, "run event %s", protocol_.last_event().c_str());
                if (result == AssistProtocol::Event::Authenticate) {
                    JsonDocument auth;
                    auth["type"] = "auth";
                    auth["access_token"] = CONFIG_JOHAN2_HA_TOKEN;
                    std::string payload;
                    serializeJson(auth, payload);
                    if (!send_text(payload)) transport_failed_.store(true);
                    std::fill(payload.begin(), payload.end(), '\0');
                } else if (result == AssistProtocol::Event::Ready) {
                    ESP_LOGI(TAG, "HA authenticated");
                    retry_at_ = 0;
                    wifi_report_at_ = 0;
                    if (kDirectReplies && !send_text(protocol_.subscribe_announcements())) transport_failed_.store(true);
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
                    if (!send_text(protocol_.subscribe_control())) transport_failed_.store(true);
                    if (!send_text(protocol_.pipeline_update(CONFIG_JOHAN2_HA_PIPELINE))) ESP_LOGW(TAG, "Pipeline report delayed");
#endif
                    publish_state(remote_start_pending_ ? "starting" : "idle", true);
                } else if (result == AssistProtocol::Event::AuthInvalid) {
                    ESP_LOGE(TAG, "HA token rejected; update local sdkconfig and rebuild");
                    cancel_local_announcement();
                    reset_audio();
                    esp_websocket_client_stop(socket_);
                    vTaskDelete(nullptr);
                    return;
                } else if (result == AssistProtocol::Event::SyncError) {
                    ESP_LOGW(TAG, "HomeDash sync rejected; check HA helpers and device permissions");
                } else if (result == AssistProtocol::Event::WifiError) {
                    ESP_LOGW(TAG, "Wi-Fi sensor report rejected; check script.johan2_wifi_report and HA permissions");
                } else if (result == AssistProtocol::Event::LocalAnnouncementError) {
                    ESP_LOGW(TAG, "Local announcement rejected; check HA bridge, helpers, URL and permissions");
                } else if (result == AssistProtocol::Event::LocalAnnouncementStop) {
                    cancel_local_announcement();
                } else if (result == AssistProtocol::Event::LocalAnnouncement) {
                    const auto& token = protocol_.local_announcement_token();
                    if (!local_announcement_token_.empty()) {
                        ESP_LOGW(TAG, "Local announcement rejected: another notification is pending");
                        report_local_announcement(token, "error");
                    } else {
                        local_announcement_url_ = protocol_.local_announcement_url();
                        local_announcement_token_ = token;
                        local_announcement_deadline_ = esp_timer_get_time() + 85000000;
                        report_local_announcement(token, "queued");
                    }
                } else if (result == AssistProtocol::Event::OwnerChanged) {
                    ESP_LOGI(TAG, "Voice ownership %s", protocol_.owns_voice ? "granted" : "revoked");
                    if (!protocol_.owns_voice) cancel_command(true);
                } else if (result == AssistProtocol::Event::RemoteStart) {
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
                    if (!protocol_.owns_voice) continue;
#endif
                    // HA cancels an Assist run through its normal subscription API.
                    // Keep the authenticated socket, avoiding reconnect latency.
                    // Pressing start while processing/speaking interrupts that work (barge-in).
                    if (!remote_start_pending_ && strcmp(assist_state_, "starting") != 0 &&
                        strcmp(assist_state_, "listening") != 0 && !protocol_.announcement_pending) {
                        followup_ = false;
                        followup_deadline_ = 0;
                        followup_used_ = false;
                        followup_count_ = 0;
                        remote_start_pending_ = true;
                        reset_audio();
                        cancel_local_announcement();
                        if (direct_playing_) tts_.cancel();
                        if (protocol_.stop_running && !send_text(protocol_.cancel_stop())) transport_failed_.store(true);
                        if (protocol_.running && !send_text(protocol_.cancel_run())) transport_failed_.store(true);
                        retry_at_ = 0;
                        publish_state("starting");
                        ESP_LOGI(TAG, "Remote wake requested");
                    }
                } else if (result == AssistProtocol::Event::RemoteCancel) {
                    if (protocol_.command_active || remote_start_pending_ || strcmp(assist_state_, "starting") == 0 ||
                        direct_playing_ || !local_announcement_token_.empty()) {
                        const bool was_listening = protocol_.command_active && protocol_.streaming;
                        ESP_LOGI(TAG, "Cancelled by button");
                        cancel_command(true);
                        if (was_listening) request_listening_cue(true);
                    } else {
                        publish_state("idle", true);
                    }
                } else if (result == AssistProtocol::Event::Wake) {
                    wake_at_ = conversation_last_ = esp_timer_get_time();
                    followup_used_ = false;
                    followup_count_ = 0;
                    wake_tail_until_ = wake_at_ + kWakeTailUs;
                    request_listening_cue();
                    ESP_LOGI(TAG, "Wake word detected; listening for command");
                    publish_state("listening");
                } else if (result == AssistProtocol::Event::Listening) {
                    request_listening_cue();
                    if (!wake_at_ || strcmp(assist_state_, "starting") == 0)
                        wake_at_ = conversation_last_ = esp_timer_get_time();
                    if (followup_) followup_deadline_ = esp_timer_get_time() + kFollowupUs;
                    run_deadline_ = esp_timer_get_time() + 45000000;
                    publish_state("listening");
                } else if (result == AssistProtocol::Event::Processing) {
                    request_listening_cue(true);
                    run_deadline_ = esp_timer_get_time() + 120000000;
                    publish_state("processing");
                } else if (result == AssistProtocol::Event::Transcript) {
                    run_deadline_ = esp_timer_get_time() + 120000000;
                    publish_state("processing");
                    ESP_LOGI(TAG, "Heard: %s", protocol_.transcript().c_str());
                    if (is_stop_phrase(protocol_.transcript())) {
                        ESP_LOGI(TAG, "Cancelled by voice");
                        cancel_command(false);
                        request_listening_cue(true);
                        command_active_.store(false);
                        continue;
                    }
                    if (is_stt_artifact(protocol_.transcript())) {
                        ESP_LOGW(TAG, "Ignored STT artifact");
                        cancel_command(false);
                        request_listening_cue(true);
                        continue;
                    }
                    request_listening_cue(true);
                    send_to_homedash("transcript", protocol_.transcript());
                } else if (result == AssistProtocol::Event::StopTranscript) {
                    ESP_LOGI(TAG, "Heard while busy: %s", protocol_.stop_transcript().c_str());
                    if (is_stop_phrase(protocol_.stop_transcript())) {
                        ESP_LOGI(TAG, "Cancelled by voice while busy");
                        cancel_command(true);
                    }
                } else if (result == AssistProtocol::Event::SpeechReady) {
                    // Unduck before Sendspin receives the answer audio. HA/MA manages
                    // announcement playback and restoration of the music queue.
                    publish_state("speaking");
                    speech_at_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "Reply audio ready %lld ms after wake word", (long long)((speech_at_ - wake_at_) / 1000));
                    send_to_homedash("response", protocol_.reply_text());
                    reset_audio(true);
                    const std::string url = kDirectReplies ? protocol_.reply_url(CONFIG_JOHAN2_HA_WS_URL) : std::string{};
                    if (!url.empty() && (cue_.playing || cue_.pending)) {
                        deferred_reply_ = url; // Finish the low end cue before starting a reply.
                        announcement_deadline_ = esp_timer_get_time() + 90000000;
                    } else if (!url.empty() && tts_.start(url)) {
                        direct_playing_ = true;
                        direct_timed_out_ = false;
                        announcement_deadline_ = esp_timer_get_time() + 90000000;
                        ESP_LOGI(TAG, "Playing spoken reply on this device");
                    } else {
                        forward_to_player();
                    }
                } else if (result == AssistProtocol::Event::PlaybackAccepted ||
                           result == AssistProtocol::Event::PlaybackFailed) {
                    const bool success = result == AssistProtocol::Event::PlaybackAccepted;
                    ESP_LOGI(TAG, "Reply playback request %s %lld ms after audio ready", success ? "accepted" : "rejected",
                             (long long)((esp_timer_get_time() - speech_at_) / 1000));
                    retry_at_ = esp_timer_get_time() + (success ? 1500000 : 10000000);
                    publish_state(success ? "idle" : "error");
                } else if (result == AssistProtocol::Event::Error || result == AssistProtocol::Event::Finished ||
                           result == AssistProtocol::Event::Silent) {
                    const bool failed = result == AssistProtocol::Event::Error;
                    // The action already ran; skip the pipeline's remaining TTS of the empty reply.
                    if (result == AssistProtocol::Event::Silent && !send_text(protocol_.cancel_run())) transport_failed_.store(true);
                    const bool was_followup = followup_;
                    followup_ = false;
                    followup_deadline_ = 0;
                    if (followup_count_ < 3 && protocol_.wants_followup(was_followup, followup_used_)) {
                        ++followup_count_;
                        // Let the user repeat the command without the wake word.
                        followup_used_ = followup_ = remote_start_pending_ = true;
                        reset_audio(!failed);
                        retry_at_ = esp_timer_get_time() + 200000;
                        publish_state("starting");
                        ESP_LOGI(TAG, "%s; listening again without wake word",
                                 protocol_.continue_conversation ? "Agent expects an answer" : "Command unclear");
                    } else if (protocol_.unclear && was_followup) {
                        // No clear follow-up either: stay quiet and rearm the wake word.
                        if (!direct_playing_ && deferred_reply_.empty() && !protocol_.announcement_pending) publish_state("idle");
                        reset_audio(!failed);
                        retry_at_ = std::max(retry_at_, esp_timer_get_time() + 250000);
                    } else {
                        if (!direct_playing_ && deferred_reply_.empty() && !protocol_.announcement_pending)
                            publish_state(failed ? "error" : "idle");
                        reset_audio(!failed);
                        retry_at_ = std::max(retry_at_, esp_timer_get_time() + (failed ? 10000000 : 250000));
                        if (failed) ESP_LOGW(TAG, "Assist run failed: %s", protocol_.error_code().c_str());
                        if (!protocol_.unclear) followup_used_ = false;
                    }
                    if (was_command_capture) request_listening_cue(true);
                }
                const bool streaming = protocol_.any_streaming();
                if (!was_streaming && streaming) {
#if CONFIG_JOHAN2_WAKE_VAD
                    wake_gate_reset(&wake_gate_);
                    wake_gating_ = false;
#endif
                    xQueueReset(audio_queue_);
                    accepting_.store(true);
                } else if (was_streaming && !streaming) {
                    accepting_.store(false);
                    xQueueReset(audio_queue_);
                } else if (protocol_.stop_streaming && !transport_failed_.load()) {
                    accepting_.store(true); // reset_audio() above must not silence the stop listener
                }
                command_active_.store(protocol_.music_muted);
            }
        }
        const int64_t now = esp_timer_get_time();
#ifdef CONFIG_JOHAN2_WIFI_SENSOR
        if (protocol_.authenticated && now >= wifi_report_at_) {
            wifi_report_at_ = now + 10'000'000LL;
            wifi_ap_record_t ap{};
            const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi sensor read failed: %s", esp_err_to_name(err));
            } else if (!send_text(protocol_.wifi_report(ap.rssi))) {
                ESP_LOGW(TAG, "Wi-Fi sensor report delayed");
            }
        }
#endif
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
        // HA helpers may finish loading after socket authentication or be reloaded.
        // Reassert the device-owned state so a missed initial update self-heals.
        if (protocol_.authenticated && now >= state_refresh_at_) {
            publish_state(assist_state_, true);
            if (!send_text(protocol_.pipeline_update(CONFIG_JOHAN2_HA_PIPELINE))) ESP_LOGW(TAG, "Pipeline report delayed");
        }
        if (protocol_.authenticated && now >= heartbeat_at_) {
            heartbeat_at_ = now + 5000000;
            if (!send_text(protocol_.heartbeat())) ESP_LOGW(TAG, "Heartbeat delayed");
        }
#endif
        if (followup_deadline_ && protocol_.speech_started && !followup_speech_seen_) {
            followup_speech_seen_ = true;
            // VAD can fire on noise: bound recognition as well as initial silence.
            followup_deadline_ = now + 15000000;
        }
        if (followup_deadline_ && !protocol_.transcript().empty()) followup_deadline_ = 0;
        if (followup_deadline_ && now >= followup_deadline_) {
            ESP_LOGI(TAG, "No follow-up speech; back to wake word");
            cancel_command(false);
            request_listening_cue(true);
        }
        if (direct_playing_) {
            const auto result = tts_.poll();
            if (result != TtsPlayer::None) {
                direct_playing_ = false;
                const bool local = local_announcement_playing_;
                const bool success = result == TtsPlayer::Ok && !direct_timed_out_ &&
                                     (!local || !local_announcement_cancelled_);
                // A fallback to the reply player is still coming, so this is not an error yet.
                const bool falling_back = !local && !direct_timed_out_ && result == TtsPlayer::Unsupported &&
                                          strlen(CONFIG_JOHAN2_HA_REPLY_PLAYER);
                if (!remote_start_pending_) publish_state(success ? "idle" : falling_back ? "speaking" : "error");
                if (local) {
                    finish_local_announcement(success);
                } else {
                    ESP_LOGI(TAG, "Direct reply %s %lld ms after audio ready", success ? "played" : "failed",
                             (long long)((now - speech_at_) / 1000));
                    if (falling_back) forward_to_player();
                }
                retry_at_ = std::max(retry_at_, now + (local ? 80000 : 1000000));
            } else if (!direct_timed_out_ && now >= announcement_deadline_) {
                // Keep ownership of the shared task until it reports completion.
                direct_timed_out_ = true;
                tts_.cancel();
                ESP_LOGW(TAG, "Direct playback timed out");
                publish_state("error");
            }
        }
        if (cue_.playing && tts_.poll() == TtsPlayer::CueDone) {
            cue_.finished(esp_timer_get_time());
            xQueueReset(audio_queue_);
        }
        if (cue_.pending && !cue_.playing) {
            if ((!cue_.pending_end && (!protocol_.command_active || !protocol_.streaming)) || now >= cue_.wait_until) {
                if (now >= cue_.wait_until) ESP_LOGW(TAG, "Listening cue skipped: playback task busy");
                cue_.pending = false; // Never delay a command indefinitely for an old reply.
            } else if (now >= cue_.not_before && !direct_playing_ && tts_.start_listening_cue(cue_.pending_end)) {
                cue_.started();
                ESP_LOGI(TAG, "Listening %s cue started", cue_.playing_end ? "end" : "start");
            }
        }
        if (!deferred_reply_.empty() && !cue_.playing && !cue_.pending) {
            if (tts_.start(deferred_reply_)) {
                deferred_reply_.clear();
                direct_playing_ = true;
                direct_timed_out_ = false;
                announcement_deadline_ = now + 90000000;
                ESP_LOGI(TAG, "Playing spoken reply after listening end cue");
            } else if (now >= announcement_deadline_) {
                deferred_reply_.clear();
                forward_to_player();
            }
        }
        if (!local_announcement_token_.empty() && !local_announcement_playing_) {
            if (now >= local_announcement_deadline_) {
                ESP_LOGW(TAG, "Local announcement expired while waiting for voice playback");
                finish_local_announcement(false);
            } else if (!protocol_.command_active && !protocol_.announcement_pending && !remote_start_pending_ &&
                       !direct_playing_ && deferred_reply_.empty() && !cue_.blocked(now)) {
                // Stop wake monitoring, not the Sendspin stream or its music queue.
                if (protocol_.running && !send_text(protocol_.cancel_run())) transport_failed_.store(true);
                if (protocol_.stop_running && !send_text(protocol_.cancel_stop())) transport_failed_.store(true);
                reset_audio();
                if (!transport_failed_.load() && tts_.start(local_announcement_url_)) {
                    direct_playing_ = local_announcement_playing_ = true;
                    direct_timed_out_ = local_announcement_cancelled_ = false;
                    announcement_deadline_ = local_announcement_deadline_;
                    report_local_announcement(local_announcement_token_, "playing");
                    publish_state("speaking");
                    ESP_LOGI(TAG, "Playing local announcement over music");
                }
            }
        }
        // Keep a short "stop" listener alive while the answer is processed or spoken.
        const bool busy = kStopListener && protocol_.authenticated && !protocol_.announcement_pending &&
                          ((protocol_.running && !protocol_.streaming && strcmp(assist_state_, "processing") == 0) ||
                           (direct_playing_ && !local_announcement_playing_));
        if (busy && !protocol_.stop_running && now >= stop_retry_at_) {
            stop_retry_at_ = now + 300000;
            if (!send_text(protocol_.begin_stop(CONFIG_JOHAN2_HA_PIPELINE))) transport_failed_.store(true);
        } else if (!busy && protocol_.stop_running) {
            if (!send_text(protocol_.cancel_stop())) transport_failed_.store(true);
            if (!protocol_.streaming) { accepting_.store(false); xQueueReset(audio_queue_); }
        }
        const bool voice_allowed =
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
            protocol_.owns_voice;
#else
            true;
#endif
        if (voice_allowed && protocol_.authenticated && !protocol_.running && !protocol_.announcement_pending &&
            !direct_playing_ && local_announcement_token_.empty() && deferred_reply_.empty() &&
            !cue_.blocked(now) && now >= retry_at_) {
            reset_audio();
            // A pipeline that never starts must not leave the UI on "starting" for five minutes.
            run_deadline_ = now + 10000000;
            followup_speech_seen_ = false;
            // Reuse one conversation so the agent keeps context across turns; start a
            // new one after a long idle gap.
            if (conversation_id_[0] == '\0' || now - conversation_last_ > kConversationIdleUs) {
                snprintf(conversation_id_, sizeof(conversation_id_), "johan2-%08" PRIx32 "%08" PRIx32,
                         esp_random(), esp_random());
            }
            if (!send_text(protocol_.begin(CONFIG_JOHAN2_HA_PIPELINE, (kDirectReplies || strlen(CONFIG_JOHAN2_HA_REPLY_PLAYER) > 0),
                                           conversation_id_, remote_start_pending_))) transport_failed_.store(true);
            if (remote_start_pending_) wake_at_ = conversation_last_ = now;
            remote_start_pending_ = false;
        }
        // Drain a small batch, then return to control events so stt-end stops promptly.
        Audio block;
        for (int n = 0; n < 4 && accepting_.load() && xQueueReceive(audio_queue_, &block, 0) == pdTRUE; ++n) {
            // Keep the wake word's own tail out of STT, where VAD would take it for the command.
            const int64_t capture_now = esp_timer_get_time();
            if (capture_now < wake_tail_until_ || cue_.blocked(capture_now)) continue;
#if CONFIG_JOHAN2_WAKE_VAD
            if (wake_vad_ && protocol_.gate_wake_audio()) {
                wake_gating_ = true;
                if (!wake_gate_feed(&wake_gate_, block.samples, block.count, wake_speech, wake_vad_)) {
                    transport_failed_.store(true);
                    break;
                }
                Audio gated{};
                while ((gated.count = wake_gate_take(&wake_gate_, gated.samples, SAMPLES)) != 0) {
                    if (!send_audio(gated.samples, gated.count)) { transport_failed_.store(true); break; }
                }
                if (transport_failed_.load()) break;
                continue;
            }
            if (wake_gating_) {
                ESP_LOGI(TAG, "Wake VAD: sent %" PRIu32 "/%" PRIu32 " frames", wake_gate_.sent_frames, wake_gate_.seen_frames);
                wake_gate_reset(&wake_gate_);
                wake_gating_ = false;
            }
#endif
            if (!send_audio(block.samples, block.count)) {
                transport_failed_.store(true);
                break;
            }
        }
    }
}
