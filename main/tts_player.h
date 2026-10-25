#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AudioOutput;

// Fetches an HA TTS WAV over HTTP and plays it through AudioOutput.
class TtsPlayer {
public:
    enum Result { None = 0, Ok, Unsupported, Failed, CueDone };
    void init(AudioOutput& audio);
    // Returns false if a request is already running.
    bool start(const std::string& url);
    // Uses the same single playback task/buffer as replies; never overlaps them.
    bool start_listening_cue(bool ending = false);
    // Stops playback early; poll() then reports Ok.
    void cancel();
    // Returns the finished request's result once, then None.
    Result poll() { return static_cast<Result>(result_.exchange(None)); }
    // Unsupported: nothing was played, so the caller may fall back. Failed: audio may have played.
private:
    static void task(void* context);
    static esp_err_t http_event(esp_http_client_event_t* event);
    Result play();
    Result play_listening_cue();
    bool feed_samples(const uint8_t* data, size_t length);
    bool emit(int32_t left, int32_t right);
    bool flush();
    AudioOutput* audio_{nullptr};
    TaskHandle_t task_{nullptr};
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> result_{None};
    std::mutex http_mutex_;
    int http_socket_{-1}; // Published/withdrawn by HTTP events before the socket closes.
    std::string url_;
    bool cue_{false}; // Published by task notification, owned until busy_ clears.
    bool cue_ending_{false};
    // Decoder and resampler state (task only).
    bool decode(uint8_t* data, size_t length);
    void* decoder_{nullptr};
    uint8_t* decoded_{nullptr};
    size_t decoded_capacity_{0};
    uint16_t channels_{0};
    uint32_t rate_{0};
    bool info_ready_{false};
    uint8_t carry_[4]{};
    size_t carry_len_{0};
    uint32_t step_{0};
    uint32_t frac_{0};
    int32_t prev_l_{0}, prev_r_{0};
    bool have_prev_{false};
    bool started_{false};
    int16_t out_[512 * 2]{};
    size_t out_len_{0};
};
