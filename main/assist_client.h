#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include "assist_protocol.h"
#include "tts_player.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class AudioOutput;

class AssistClient {
public:
    static constexpr size_t SAMPLES = 256;
    void start(AudioOutput* audio = nullptr);
    // Called by the microphone task: bounded queue copy, never waits for networking.
    void submit(const int16_t* samples, size_t count);
    bool command_active() const { return command_active_.load(); }
private:
    struct Audio { uint16_t count; int16_t samples[SAMPLES]; };
    struct Message { int32_t type; char* text; };
    static void task(void* context);
    static void event(void* context, esp_event_base_t, int32_t id, void* data);
    void run();
    bool send_text(const std::string& text);
    void reset_audio();
    void forward_to_player();
    TtsPlayer tts_;
    bool direct_playing_{false};
    QueueHandle_t audio_queue_{nullptr};
    QueueHandle_t messages_{nullptr};
    esp_websocket_client_handle_t socket_{nullptr};
    AssistProtocol protocol_;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> command_active_{false};
    std::atomic<bool> transport_failed_{false};
    std::string received_; // Only the WebSocket event task touches this buffer.
    bool receiving_{false};
    int64_t retry_at_{0};
    int64_t run_deadline_{0};
    int64_t announcement_deadline_{0};
    static constexpr int64_t kConversationIdleUs = 300000000;
    char conversation_id_[32]{};
    int64_t conversation_last_{0};
    int64_t wake_at_{0};
    int64_t speech_at_{0};
};
