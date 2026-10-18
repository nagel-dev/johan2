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
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "assist";
static constexpr bool kDirectReplies = CONFIG_JOHAN2_TTS_DIRECT;

void AssistClient::start(AudioOutput* audio) {
    if (!strlen(CONFIG_JOHAN2_HA_TOKEN)) {
        ESP_LOGI(TAG, "Assist disabled: set HA token in local sdkconfig");
        return;
    }
#if CONFIG_JOHAN2_TTS_DIRECT
    if (audio) tts_.init(*audio);
#endif
    if (!kDirectReplies && !strlen(CONFIG_JOHAN2_HA_REPLY_PLAYER)) {
        ESP_LOGW(TAG, "Spoken replies disabled: enable direct replies or set reply media_player in menuconfig");
    }
    // Never emit WebSocket payloads or authentication data at debug level.
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    audio_queue_ = xQueueCreate(16, sizeof(Audio)); // At most 256 ms of microphone audio.
    messages_ = xQueueCreate(12, sizeof(Message));
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
            self.receiving_ = false;
            self.transport_failed_.store(true);
            return;
        }
        self.received_.append(data.data_ptr, data.data_len);
        if (!data.fin || data.payload_offset + data.data_len != data.payload_len) return;
        self.receiving_ = false;
        message.text = strdup(self.received_.c_str());
        if (!message.text) { self.transport_failed_.store(true); return; }
    } else if (id == WEBSOCKET_EVENT_CONNECTED || id == WEBSOCKET_EVENT_DISCONNECTED) {
        self.accepting_.store(false);
        self.command_active_.store(false);
        self.received_.clear();
        self.receiving_ = false;
    } else {
        return;
    }
    if (xQueueSend(self.messages_, &message, 0) != pdTRUE) {
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
        if (!send_text(protocol_.state_update(state))) transport_failed_.store(true);
    }
#else
    (void)changed;
    (void)force;
#endif
}

void AssistClient::reset_audio() {
    accepting_.store(false);
    command_active_.store(false);
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

// Shows what Assist heard on HomeDash (via its proxy). Best effort, off the websocket task.
static void post_transcript(void* context) {
    std::unique_ptr<std::string> text(static_cast<std::string*>(context));
    esp_http_client_config_t config{};
    config.url = CONFIG_JOHAN2_HOMEDASH_TRANSCRIPT_URL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 3000;
    if (auto client = esp_http_client_init(&config)) {
        esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
        esp_http_client_set_post_field(client, text->c_str(), text->size());
        if (esp_http_client_perform(client) != ESP_OK) ESP_LOGW(TAG, "Could not send transcript to HomeDash");
        esp_http_client_cleanup(client);
    }
    vTaskDelete(nullptr);
}

void AssistClient::task(void* context) { static_cast<AssistClient*>(context)->run(); }

void AssistClient::run() {
    ESP_ERROR_CHECK(esp_websocket_client_start(socket_));
    while (true) {
        const bool failed = transport_failed_.exchange(false);
        if (failed ||
            (protocol_.running && esp_timer_get_time() >= run_deadline_) ||
            (protocol_.announcement_pending && esp_timer_get_time() >= announcement_deadline_)) {
            publish_state("error");
            reset_audio();
            // Must be called outside the WebSocket callback/task.
            esp_websocket_client_stop(socket_);
            protocol_.reset();
            Message stale;
            while (xQueueReceive(messages_, &stale, 0) == pdTRUE) free(stale.text);
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_ERROR_CHECK(esp_websocket_client_start(socket_));
        }
        Message message;
        if (xQueueReceive(messages_, &message, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (message.type == WEBSOCKET_EVENT_CONNECTED || message.type == WEBSOCKET_EVENT_DISCONNECTED) {
                reset_audio();
                protocol_.reset();
                ESP_LOGI(TAG, "%s", message.type == WEBSOCKET_EVENT_CONNECTED ? "HA connected" : "HA disconnected");
            } else if (message.text) {
                const bool was_streaming = protocol_.streaming;
                const auto result = protocol_.receive(message.text);
                free(message.text);
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
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
                    if (!send_text(protocol_.subscribe_control())) transport_failed_.store(true);
#endif
                    publish_state(remote_start_pending_ ? "starting" : "idle", true);
                } else if (result == AssistProtocol::Event::AuthInvalid) {
                    ESP_LOGE(TAG, "HA token rejected; update local sdkconfig and rebuild");
                    reset_audio();
                    esp_websocket_client_stop(socket_);
                    vTaskDelete(nullptr);
                    return;
                } else if (result == AssistProtocol::Event::SyncError) {
                    ESP_LOGW(TAG, "HomeDash sync rejected; check HA helpers and device permissions");
                } else if (result == AssistProtocol::Event::RemoteStart) {
                    // HA cancels an Assist run through its normal subscription API.
                    // Keep the authenticated socket, avoiding reconnect latency.
                    if (!remote_start_pending_ && strcmp(assist_state_, "starting") != 0 &&
                        !protocol_.command_active && !direct_playing_ && !protocol_.announcement_pending) {
                        remote_start_pending_ = true;
                        reset_audio();
                        if (protocol_.running && !send_text(protocol_.cancel_run())) transport_failed_.store(true);
                        retry_at_ = 0;
                        publish_state("starting");
                        ESP_LOGI(TAG, "Remote wake requested");
                    }
                } else if (result == AssistProtocol::Event::RemoteCancel) {
                    if ((protocol_.command_active || remote_start_pending_ || strcmp(assist_state_, "starting") == 0) &&
                        !direct_playing_ && !protocol_.announcement_pending) {
                        remote_start_pending_ = false;
                        reset_audio();
                        if (protocol_.running && !send_text(protocol_.cancel_run())) transport_failed_.store(true);
                        retry_at_ = 0;
                        publish_state("idle");
                    }
                } else if (result == AssistProtocol::Event::Wake) {
                    wake_at_ = conversation_last_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "Wake word detected; listening for command");
                    publish_state("listening");
                } else if (result == AssistProtocol::Event::Listening) {
                    if (!wake_at_ || strcmp(assist_state_, "starting") == 0)
                        wake_at_ = conversation_last_ = esp_timer_get_time();
                    publish_state("listening");
                } else if (result == AssistProtocol::Event::Processing) {
                    publish_state("processing");
                } else if (result == AssistProtocol::Event::Transcript) {
                    publish_state("processing");
                    ESP_LOGI(TAG, "Heard: %s", protocol_.transcript().c_str());
                    if (CONFIG_JOHAN2_HOMEDASH_TRANSCRIPT_URL[0]) {
                        auto* text = new std::string(protocol_.transcript());
                        if (xTaskCreate(post_transcript, "transcript", 4096, text, 3, nullptr) != pdPASS) delete text;
                    }
                } else if (result == AssistProtocol::Event::SpeechReady) {
                    // Unduck before Sendspin receives the answer audio. HA/MA manages
                    // announcement playback and restoration of the music queue.
                    publish_state("speaking");
                    speech_at_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "Reply audio ready %lld ms after wake word", (long long)((speech_at_ - wake_at_) / 1000));
                    reset_audio();
                    const std::string url = kDirectReplies ? protocol_.reply_url(CONFIG_JOHAN2_HA_WS_URL) : std::string{};
                    if (!url.empty() && tts_.start(url)) {
                        direct_playing_ = true;
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
                } else if (result == AssistProtocol::Event::Error || result == AssistProtocol::Event::Finished) {
                    if (!direct_playing_ && !protocol_.announcement_pending)
                        publish_state(result == AssistProtocol::Event::Error ? "error" : "idle");
                    reset_audio();
                    retry_at_ = std::max(retry_at_, esp_timer_get_time() + (result == AssistProtocol::Event::Error ? 10000000 : 250000));
                    if (result == AssistProtocol::Event::Error) ESP_LOGW(TAG, "Assist run failed: %s", protocol_.error_code().c_str());
                }
                if (!was_streaming && protocol_.streaming) {
                    xQueueReset(audio_queue_);
                    accepting_.store(true);
                }
                if (was_streaming && !protocol_.streaming) {
                    accepting_.store(false);
                    xQueueReset(audio_queue_);
                }
                command_active_.store(protocol_.command_active);
            }
        }
        const int64_t now = esp_timer_get_time();
#ifdef CONFIG_JOHAN2_HOMEDASH_SYNC
        // HA helpers may finish loading after socket authentication or be reloaded.
        // Reassert the device-owned state so a missed initial update self-heals.
        if (protocol_.authenticated && now >= state_refresh_at_) publish_state(assist_state_, true);
#endif
        if (direct_playing_) {
            const auto result = tts_.poll();
            if (result != TtsPlayer::None) {
                direct_playing_ = false;
                publish_state(result == TtsPlayer::Ok ? "idle" : "error");
                ESP_LOGI(TAG, "Direct reply %s %lld ms after audio ready", result == TtsPlayer::Ok ? "played" : "failed",
                         (long long)((now - speech_at_) / 1000));
                if (result == TtsPlayer::Unsupported && strlen(CONFIG_JOHAN2_HA_REPLY_PLAYER)) forward_to_player();
                retry_at_ = std::max(retry_at_, now + 1000000);
            } else if (now >= announcement_deadline_) {
                direct_playing_ = false;
                publish_state("error");
            }
        }
        if (protocol_.authenticated && !protocol_.running && !protocol_.announcement_pending && !direct_playing_ && now >= retry_at_) {
            reset_audio();
            run_deadline_ = now + 310000000;
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
            uint8_t packet[1 + SAMPLES * 2];
            packet[0] = protocol_.handler;
            for (size_t i = 0; i < block.count; ++i) {
                const uint16_t sample = static_cast<uint16_t>(block.samples[i]);
                packet[1 + i * 2] = sample & 0xff;
                packet[2 + i * 2] = sample >> 8;
            }
            const int length = 1 + block.count * 2;
            if (esp_websocket_client_send_bin(socket_, reinterpret_cast<char*>(packet), length,
                                              pdMS_TO_TICKS(100)) != length) {
                transport_failed_.store(true);
                break;
            }
        }
    }
}
