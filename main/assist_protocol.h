#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include <string>
#include "runtime_metrics.h"

// HA Assist wire state, kept independent of ESP-IDF so it can be tested on a host.
class AssistProtocol {
public:
    enum class Event { None, Authenticate, Ready, Wake, Listening, Processing, RemoteStart, RemoteCancel, OwnerChanged, SyncError, WifiError, DiagnosticsError, LocalAnnouncement, LocalAnnouncementStop, LocalAnnouncementError, Transcript, SpeechReady, StopTranscript, PlaybackAccepted, PlaybackFailed, Silent, Finished, Error, AuthInvalid };
    bool listening_cue_enabled = true; // Follows input_boolean.johan2_listening_cue; on until HA says off.
    bool owns_voice = false; // Fail closed until HA grants this device voice ownership.
    bool authenticated = false;
    bool running = false;
    bool streaming = false;
    bool command_active = false;
    bool music_muted = false;    // only while the command is being spoken; processing leaves music playing
    bool unclear = false;        // STT heard nothing or the agent did not understand
    bool continue_conversation = false; // the agent asked a question and expects an answer
    bool speech_started = false; // HA reported voice activity in this run
    bool silent = false;         // agent intentionally produced no spoken reply (e.g. it only played music)
    uint8_t handler = 0;
    int run_id = 0;
    bool announcement_pending = false;
    int announcement_id = 0;
    // Short STT-only run that listens for "stop" while the main run is processing or replying.
    bool stop_running = false;
    bool stop_streaming = false;
    uint8_t stop_handler = 0;
    bool any_streaming() const { return streaming || stop_streaming; }
    uint8_t active_handler() const { return stop_streaming ? stop_handler : handler; }
    bool gate_wake_audio() const { return streaming && awaiting_wake_ && !stop_streaming; }

    void reset() {
        awaiting_wake_ = false;
        authenticated = running = streaming = command_active = music_muted = false;
        unclear = speech_started = continue_conversation = silent = false;
        transcript_.clear();
        stop_running = stop_streaming = false;
        stop_handler = 0;
        stop_id_ = 0;
        handler = 0;
        control_id_ = state_id_ = heartbeat_id_ = wifi_id_ = 0;
        diagnostics_id_ = 0;
        local_subscription_id_ = local_status_id_ = 0;
        local_url_.clear();
        local_token_.clear();
        last_local_url_.clear();
        owns_voice = false;
        last_start_.clear();
        last_cancel_.clear();
        announcement_pending = false;
        response_media_.clear();
        response_url_.clear();
        response_mime_.clear();
    }
    std::string begin(const char* pipeline, bool speak = false, const char* conversation_id = nullptr, bool skip_wake = false) {
        awaiting_wake_ = !skip_wake;
        JsonDocument doc;
        doc["id"] = run_id = ++next_id_;
        doc["type"] = "assist_pipeline/run";
        doc["start_stage"] = skip_wake ? "stt" : "wake_word";
        doc["end_stage"] = speak ? "tts" : "intent";
        doc["timeout"] = 300;
        doc["input"]["sample_rate"] = 16000;
        doc["input"]["timeout"] = 30;
        doc["input"]["noise_suppression_level"] = 2;
        // Firmware already owns microphone gain; do not re-level residual echo/noise in HA.
        doc["input"]["auto_gain_dbfs"] = 0;
        if (pipeline && *pipeline) doc["pipeline"] = pipeline;
        if (conversation_id && *conversation_id) doc["conversation_id"] = conversation_id;
        error_code_.clear();
        unclear = speech_started = continue_conversation = silent = false;
        transcript_.clear();
        response_media_.clear();
        response_url_.clear();
        response_mime_.clear();
        reply_text_.clear();
        speech_forwarded_ = false;
        running = true;
        streaming = command_active = music_muted = false;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string begin_stop(const char* pipeline) {
        JsonDocument doc;
        doc["id"] = stop_id_ = ++next_id_;
        doc["type"] = "assist_pipeline/run";
        doc["start_stage"] = "stt";
        doc["end_stage"] = "stt";
        doc["timeout"] = 20;
        doc["input"]["sample_rate"] = 16000;
        doc["input"]["timeout"] = 10;
        doc["input"]["noise_suppression_level"] = 2;
        // Firmware already owns microphone gain; do not re-level residual echo/noise in HA.
        doc["input"]["auto_gain_dbfs"] = 0;
        if (pipeline && *pipeline) doc["pipeline"] = pipeline;
        stop_running = true;
        stop_streaming = false;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string cancel_stop() {
        JsonDocument doc;
        doc["id"] = ++next_id_;
        doc["type"] = "unsubscribe_events";
        doc["subscription"] = stop_id_;
        stop_running = stop_streaming = false;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string cancel_run() {
        JsonDocument doc;
        doc["id"] = ++next_id_;
        doc["type"] = "unsubscribe_events";
        doc["subscription"] = run_id;
        finish();
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    const std::string& error_code() const { return error_code_; }
    std::string subscribe_control() {
        JsonDocument doc;
        doc["id"] = control_id_ = ++next_id_;
        doc["type"] = "subscribe_entities";
        doc["entity_ids"].to<JsonArray>().add("input_button.johan2_assist_start");
        doc["entity_ids"].add("input_button.johan2_assist_cancel");
        doc["entity_ids"].add("input_select.voice_device");
        doc["entity_ids"].add("input_boolean.johan2_listening_cue");
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string state_update(const char* state) {
        JsonDocument doc;
        doc["id"] = state_id_ = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "input_select";
        doc["service"] = "select_option";
        doc["target"]["entity_id"] = "input_select.johan2_assist_state";
        doc["service_data"]["option"] = state;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string subscribe_announcements() {
        JsonDocument doc;
        doc["id"] = local_subscription_id_ = ++next_id_;
        doc["type"] = "subscribe_entities";
        doc["entity_ids"].to<JsonArray>().add("input_text.johan2_announcement_url");
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    const std::string& local_announcement_url() const { return local_url_; }
    const std::string& local_announcement_token() const { return local_token_; }
    std::string local_announcement_status(const std::string& token, const char* state) {
        JsonDocument doc;
        doc["id"] = local_status_id_ = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "input_text";
        doc["service"] = "set_value";
        doc["target"]["entity_id"] = "input_text.johan2_announcement_status";
        doc["service_data"]["value"] = std::string(state) + ":" + token;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string heartbeat() {
        JsonDocument doc;
        doc["id"] = heartbeat_id_ = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "input_button";
        doc["service"] = "press";
        doc["target"]["entity_id"] = "input_button.johan2_voice_heartbeat";
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string wifi_report(int rssi) {
        JsonDocument doc;
        doc["id"] = wifi_id_ = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "script";
        doc["service"] = "johan2_wifi_report";
        doc["service_data"]["rssi"] = rssi;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string diagnostics_report(const RuntimeSnapshot& snapshot) {
        JsonDocument doc;
        doc["id"] = diagnostics_id_ = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "script";
        doc["service"] = "johan2_diagnostics_report";
        auto data = doc["service_data"]["metrics"].to<JsonObject>();
        data["boot_id"] = snapshot.boot_id;
        data["uptime_s"] = snapshot.uptime_s;
        data["internal_heap_bytes"] = snapshot.internal_heap_bytes;
        data["min_internal_heap_bytes"] = snapshot.min_internal_heap_bytes;
        data["main_loop_age_ms"] = snapshot.main_loop_age_ms;
        data["pcm_age_ms"] = snapshot.pcm_age_ms;
        data["pcm_gaps"] = snapshot.pcm_gaps;
        data["i2s_errors"] = snapshot.i2s_errors;
        data["assist_restarts"] = snapshot.assist_restarts;
        data["mic_queue_drops"] = snapshot.mic_queue_drops;
        data["mic_overruns"] = snapshot.mic_overruns;
        data["wifi_disconnects"] = snapshot.wifi_disconnects;
        data["sendspin_connected"] = snapshot.sendspin_connected;
        data["stream_active"] = snapshot.stream_active;
        data["status"] = snapshot.status();
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    std::string pipeline_update(const char* pipeline) {
        JsonDocument doc;
        doc["id"] = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "input_text";
        doc["service"] = "set_value";
        doc["target"]["entity_id"] = "input_text.johan2_voice_pipeline";
        doc["service_data"]["value"] = pipeline;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    Event receive(const char* text) {
        last_event_.clear();
        JsonDocument doc;
        if (deserializeJson(doc, text, DeserializationOption::NestingLimit(24))) {
            finish();
            return Event::Error;
        }
        const std::string type = doc["type"] | "";
        if (type == "auth_required") return Event::Authenticate;
        if (type == "auth_ok") { authenticated = true; return Event::Ready; }
        if (type == "auth_invalid") { reset(); return Event::AuthInvalid; }
        if (type == "result" && doc["success"] == false &&
            ((local_subscription_id_ && (doc["id"] | -1) == local_subscription_id_) ||
             (local_status_id_ && (doc["id"] | -1) == local_status_id_))) return Event::LocalAnnouncementError;
        if (authenticated && local_subscription_id_ && type == "event" &&
            (doc["id"] | -1) == local_subscription_id_) {
            // A reconnect snapshot must not replay an already spoken notification.
            const auto added = doc["event"]["a"]["input_text.johan2_announcement_url"]["s"];
            if (added.is<const char*>()) last_local_url_ = added.as<std::string>();
            for (JsonVariantConst removed : doc["event"]["r"].as<JsonArrayConst>()) {
                if (removed.as<std::string>() == "input_text.johan2_announcement_url") {
                    last_local_url_.clear();
                    return Event::LocalAnnouncementStop;
                }
            }
            const auto changed = doc["event"]["c"]["input_text.johan2_announcement_url"]["+"]["s"];
            if (!changed.is<const char*>()) return Event::None;
            const std::string url = changed.as<std::string>();
            if (url == last_local_url_) return Event::None;
            last_local_url_ = url;
            local_url_.clear();
            local_token_.clear();
            if (url.empty() || url == "unknown" || url == "unavailable") return Event::LocalAnnouncementStop;
            const auto scheme = url.find("://");
            const auto path = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
            static constexpr const char* proxy = "/api/homedash_announcement/";
            if (url.size() > 255 || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) ||
                path == std::string::npos || url.compare(path, strlen(proxy), proxy) != 0)
                return Event::LocalAnnouncementError;
            const std::string token = url.substr(path + strlen(proxy));
            if (token.empty() || token.size() > 64 ||
                token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
                return Event::LocalAnnouncementError;
            local_url_ = url;
            local_token_ = token;
            return Event::LocalAnnouncement;
        }
        if (type == "result" && announcement_pending && (doc["id"] | -1) == announcement_id) {
            announcement_pending = false;
            return doc["success"] == true ? Event::PlaybackAccepted : Event::PlaybackFailed;
        }
        if (type == "result" && doc["success"] == false &&
            wifi_id_ && (doc["id"] | -1) == wifi_id_) return Event::WifiError;
        if (type == "result" && doc["success"] == false &&
            diagnostics_id_ && (doc["id"] | -1) == diagnostics_id_) return Event::DiagnosticsError;
        if (type == "result" && doc["success"] == false &&
            ((control_id_ && (doc["id"] | -1) == control_id_) ||
             (state_id_ && (doc["id"] | -1) == state_id_) ||
             (heartbeat_id_ && (doc["id"] | -1) == heartbeat_id_))) return Event::SyncError;
        if (authenticated && control_id_ && type == "event" && (doc["id"] | -1) == control_id_) {
            // Initial/restored snapshots seed timestamps but never replay button presses.
            const auto added = doc["event"]["a"];
            if (added["input_button.johan2_assist_start"]["s"].is<const char*>())
                last_start_ = added["input_button.johan2_assist_start"]["s"].as<const char*>();
            if (added["input_button.johan2_assist_cancel"]["s"].is<const char*>())
                last_cancel_ = added["input_button.johan2_assist_cancel"]["s"].as<const char*>();
            const auto changed = doc["event"]["c"];
            const bool start = pressed(changed["input_button.johan2_assist_start"]["+"]["s"], last_start_);
            const bool cancel = pressed(changed["input_button.johan2_assist_cancel"]["+"]["s"], last_cancel_);
            JsonVariantConst owner = changed["input_select.voice_device"]["+"]["s"];
            if (owner.isNull()) owner = added["input_select.voice_device"]["s"];
            bool owner_removed = false;
            for (JsonVariantConst removed : doc["event"]["r"].as<JsonArrayConst>())
                if (removed.as<std::string>() == "input_select.voice_device") owner_removed = true;
            const bool owner_seen = owner.is<const char*>() || owner_removed;
            const bool previous_owner = owns_voice;
            if (owner_seen) owns_voice = owner.is<const char*>() && owner.as<std::string>() == "johan2";
            JsonVariantConst cue = changed["input_boolean.johan2_listening_cue"]["+"]["s"];
            if (cue.isNull()) cue = added["input_boolean.johan2_listening_cue"]["s"];
            if (cue.is<const char*>()) {
                const std::string value = cue.as<std::string>();
                if (value == "on") listening_cue_enabled = true;
                else if (value == "off") listening_cue_enabled = false;
            }
            for (JsonVariantConst removed : doc["event"]["r"].as<JsonArrayConst>())
                if (removed.as<std::string>() == "input_boolean.johan2_listening_cue") listening_cue_enabled = true;
            if (cancel) return Event::RemoteCancel;
            if (previous_owner != owns_voice) return Event::OwnerChanged;
            return start ? Event::RemoteStart : Event::None;
        }
        if (stop_running && stop_id_ && (doc["id"] | -1) == stop_id_) {
            if (type == "result" && doc["success"] == false) {
                stop_running = stop_streaming = false;
                return Event::None;
            }
            if (type != "event") return Event::None;
            const std::string kind = doc["event"]["type"] | "";
            if (kind == "run-start") {
                const auto id = doc["event"]["data"]["runner_data"]["stt_binary_handler_id"];
                if (id.is<int>() && id.as<int>() >= 1 && id.as<int>() <= 255) {
                    stop_handler = id.as<uint8_t>();
                    stop_streaming = true;
                } else {
                    stop_running = false;
                }
            } else if (kind == "stt-end") {
                stop_streaming = false;
                stop_transcript_ = doc["event"]["data"]["stt_output"]["text"] | "";
                if (!stop_transcript_.empty()) return Event::StopTranscript;
            } else if (kind == "run-end" || kind == "error") {
                stop_running = stop_streaming = false;
            }
            return Event::None;
        }
        if (!running || (doc["id"] | -1) != run_id) return Event::None;
        if (type == "result" && doc["success"] == false) {
            error_code_ = doc["error"]["code"] | "unknown";
            finish();
            if (error_code_ == "stt-no-text-recognized") return Event::Finished;
            return Event::Error;
        }
        if (type != "event") return Event::None;
        const std::string event = doc["event"]["type"] | "";
        last_event_ = event;
        if (event == "run-start") {
            const auto id = doc["event"]["data"]["runner_data"]["stt_binary_handler_id"];
            if (!id.is<int>() || id.as<int>() < 1 || id.as<int>() > 255) {
                finish();
                return Event::Error;
            }
            handler = id.as<uint8_t>();
            streaming = true;
        } else if (event == "wake_word-end") {
            if (!doc["event"]["data"]["wake_word_output"]["wake_word_id"].isNull()) {
                command_active = music_muted = true;
                awaiting_wake_ = false;
                return Event::Wake;
            }
        } else if (event == "stt-start") {
            awaiting_wake_ = false;
            command_active = music_muted = true;
            return Event::Listening;
        } else if (event == "stt-vad-start") {
            speech_started = true;
        } else if (event == "intent-end") {
            const std::string kind = doc["event"]["data"]["intent_output"]["response"]["response_type"] | "";
            const std::string code = doc["event"]["data"]["intent_output"]["response"]["data"]["code"] | "";
            if (doc["event"]["data"]["intent_output"]["continue_conversation"] == true) continue_conversation = true;
            // HA's flag is not always set; a spoken reply ending in a question also expects an answer.
            std::string spoken = doc["event"]["data"]["intent_output"]["response"]["speech"]["plain"]["speech"] | "";
            while (!spoken.empty() && (spoken.back() == ' ' || spoken.back() == '\n' || spoken.back() == '"' || spoken.back() == '\''))
                spoken.pop_back();
            if (!spoken.empty() && spoken.back() == '?') continue_conversation = true;
            reply_text_ = spoken;
            // HA's OpenAI agent reports a reply-less turn as this error; the action itself already ran.
            silent = kind == "error" && code == "unknown" && spoken == "Unable to get response";
            if (silent) {
                reply_text_.clear();
                return Event::Silent;
            }
            if (kind == "error" && (code == "no_intent_match" || code == "no_valid_targets")) unclear = true;
        } else if (event == "stt-vad-end") {
            music_muted = false;
            return Event::Processing;
        } else if (event == "stt-end") {
            streaming = music_muted = false;
            transcript_ = doc["event"]["data"]["stt_output"]["text"] | "";
            return transcript_.empty() ? Event::Processing : Event::Transcript;
        } else if (event == "tts-end" && silent) {
            streaming = command_active = music_muted = false;
        } else if (event == "tts-end" && !speech_forwarded_) {
            const auto output = doc["event"]["data"]["tts_output"];
            response_media_ = output["media_id"] | "";
            response_url_ = output["url"] | "";
            if (response_media_.empty()) response_media_ = response_url_;
            response_mime_ = output["mime_type"] | "music";
            streaming = command_active = music_muted = false;
            if (!response_media_.empty()) return Event::SpeechReady;
        } else if (event == "run-end") {
            finish();
            return Event::Finished;
        } else if (event == "error") {
            error_code_ = doc["event"]["data"]["code"] | "unknown";
            finish();
            // Silence/no recognized text is not an unclear command to retry.
            if (error_code_ == "wake-word-timeout" || error_code_ == "stt-no-text-recognized")
                return Event::Finished;
            return Event::Error;
        }
        return Event::None;
    }
    // A second automatic turn requires actual recognized input, not just VAD noise.
    bool wants_followup(bool already_followup, bool retry_used) const {
        return (continue_conversation && (!already_followup || !transcript_.empty())) ||
               (unclear && !retry_used && !already_followup);
    }
    // Absolute http(s) URL of the generated reply, or empty. Does not consume the reply.
    const std::string& transcript() const { return transcript_; }
    const std::string& last_event() const { return last_event_; }
    const std::string& stop_transcript() const { return stop_transcript_; }
    const std::string& reply_text() const { return reply_text_; }
    std::string reply_url(const char* websocket_url) const {
        // media_id is usually media-source://; the plain url is what HTTP can fetch.
        return resolve_media(response_url_.empty() ? response_media_ : response_url_, websocket_url, false);
    }
    std::string announcement(const char* entity, const char* websocket_url) {
        if (speech_forwarded_ || response_media_.empty() || !entity ||
            std::string(entity).rfind("media_player.", 0) != 0) return {};
        const std::string media = resolve_media(response_media_, websocket_url, true);
        if (media.empty()) return {};
        JsonDocument doc;
        doc["id"] = announcement_id = ++next_id_;
        doc["type"] = "call_service";
        doc["domain"] = "media_player";
        doc["service"] = "play_media";
        doc["target"]["entity_id"] = entity;
        doc["service_data"]["media_content_id"] = media;
        doc["service_data"]["media_content_type"] = response_mime_;
        doc["service_data"]["announce"] = true;
        doc["service_data"]["extra"]["use_pre_announce"] = false;
        announcement_pending = speech_forwarded_ = true;
        response_media_.clear();
        response_url_.clear();
        response_mime_.clear();
        std::string result;
        serializeJson(doc, result);
        return result;
    }
private:
    bool awaiting_wake_ = false;
    static std::string resolve_media(const std::string& source, const char* websocket_url, bool allow_media_source) {
        if (source.empty()) return {};
        std::string media = source;
        if (media[0] == '/' && media.rfind("//", 0) != 0) {
            std::string origin = websocket_url ? websocket_url : "";
            if (origin.rfind("wss://", 0) == 0) origin.replace(0, 3, "https");
            else if (origin.rfind("ws://", 0) == 0) origin.replace(0, 2, "http");
            else return {};
            const auto path = origin.find('/', origin.find("://") + 3);
            if (path != std::string::npos) origin.resize(path);
            media = origin + media;
        }
        if (media.size() > 4096 || (media.rfind("http://", 0) != 0 && media.rfind("https://", 0) != 0 &&
            !(allow_media_source && media.rfind("media-source://", 0) == 0))) return {};
        return media;
    }
    int next_id_ = 0;
    int control_id_ = 0, state_id_ = 0, heartbeat_id_ = 0, wifi_id_ = 0;
    int diagnostics_id_ = 0;
    int local_subscription_id_ = 0, local_status_id_ = 0;
    std::string local_url_, local_token_, last_local_url_;
    std::string last_start_, last_cancel_;
    static bool pressed(JsonVariantConst state, std::string& previous) {
        if (!state.is<const char*>()) return false;
        const std::string value = state.as<const char*>();
        if (value.empty() || value == "unknown" || value == "unavailable" || value == previous) return false;
        previous = value;
        return true;
    }
    bool speech_forwarded_ = false;
    std::string reply_text_;
    std::string response_media_;
    std::string response_url_;
    std::string transcript_;
    std::string stop_transcript_;
    std::string last_event_;
    int stop_id_ = 0;
    std::string error_code_;
    std::string response_mime_;
    void finish() { running = streaming = command_active = music_muted = false; }
};
