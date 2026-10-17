#pragma once
#include <ArduinoJson.h>
#include <string>

// HA Assist wire state, kept independent of ESP-IDF so it can be tested on a host.
class AssistProtocol {
public:
    enum class Event { None, Authenticate, Ready, Wake, Transcript, SpeechReady, PlaybackAccepted, PlaybackFailed, Finished, Error, AuthInvalid };
    bool authenticated = false;
    bool running = false;
    bool streaming = false;
    bool command_active = false;
    uint8_t handler = 0;
    int run_id = 0;
    bool announcement_pending = false;
    int announcement_id = 0;

    void reset() {
        authenticated = running = streaming = command_active = false;
        handler = 0;
        announcement_pending = false;
        response_media_.clear();
        response_url_.clear();
        response_mime_.clear();
    }
    std::string begin(const char* pipeline, bool speak = false, const char* conversation_id = nullptr) {
        JsonDocument doc;
        doc["id"] = run_id = ++next_id_;
        doc["type"] = "assist_pipeline/run";
        doc["start_stage"] = "wake_word";
        doc["end_stage"] = speak ? "tts" : "intent";
        doc["timeout"] = 300;
        doc["input"]["sample_rate"] = 16000;
        doc["input"]["timeout"] = 30;
        doc["input"]["noise_suppression_level"] = 2;
        doc["input"]["auto_gain_dbfs"] = 31;
        if (pipeline && *pipeline) doc["pipeline"] = pipeline;
        if (conversation_id && *conversation_id) doc["conversation_id"] = conversation_id;
        response_media_.clear();
        response_url_.clear();
        response_mime_.clear();
        speech_forwarded_ = false;
        running = true;
        streaming = command_active = false;
        std::string result;
        serializeJson(doc, result);
        return result;
    }
    Event receive(const char* text) {
        JsonDocument doc;
        if (deserializeJson(doc, text, DeserializationOption::NestingLimit(24))) {
            finish();
            return Event::Error;
        }
        const std::string type = doc["type"] | "";
        if (type == "auth_required") return Event::Authenticate;
        if (type == "auth_ok") { authenticated = true; return Event::Ready; }
        if (type == "auth_invalid") { reset(); return Event::AuthInvalid; }
        if (type == "result" && announcement_pending && (doc["id"] | -1) == announcement_id) {
            announcement_pending = false;
            return doc["success"] == true ? Event::PlaybackAccepted : Event::PlaybackFailed;
        }
        if (!running || (doc["id"] | -1) != run_id) return Event::None;
        if (type == "result" && doc["success"] == false) {
            finish();
            return Event::Error;
        }
        if (type != "event") return Event::None;
        const std::string event = doc["event"]["type"] | "";
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
                command_active = true;
                return Event::Wake;
            }
        } else if (event == "stt-end") {
            streaming = false;
            transcript_ = doc["event"]["data"]["stt_output"]["text"] | "";
            if (!transcript_.empty()) return Event::Transcript;
        } else if (event == "tts-end" && !speech_forwarded_) {
            const auto output = doc["event"]["data"]["tts_output"];
            response_media_ = output["media_id"] | "";
            response_url_ = output["url"] | "";
            if (response_media_.empty()) response_media_ = response_url_;
            response_mime_ = output["mime_type"] | "music";
            streaming = command_active = false;
            if (!response_media_.empty()) return Event::SpeechReady;
        } else if (event == "run-end") {
            finish();
            return Event::Finished;
        } else if (event == "error") {
            finish();
            // Quiet rooms routinely time out; rearm promptly instead of backing off.
            if (doc["event"]["data"]["code"] == "wake-word-timeout") return Event::Finished;
            return Event::Error;
        }
        return Event::None;
    }
    // Absolute http(s) URL of the generated reply, or empty. Does not consume the reply.
    const std::string& transcript() const { return transcript_; }
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
    bool speech_forwarded_ = false;
    std::string response_media_;
    std::string response_url_;
    std::string transcript_;
    std::string response_mime_;
    void finish() { running = streaming = command_active = false; }
};
