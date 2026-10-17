#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AudioOutput;

// Fetches an HA TTS WAV over HTTP and plays it through AudioOutput.
class TtsPlayer {
public:
    enum Result { None = 0, Ok, Unsupported, Failed };
    void init(AudioOutput& audio);
    // Returns false if a request is already running.
    bool start(const std::string& url);
    // Returns the finished request's result once, then None.
    Result poll() { return static_cast<Result>(result_.exchange(None)); }
    // Unsupported: nothing was played, so the caller may fall back. Failed: audio may have played.
private:
    static void task(void* context);
    Result play();
    void feed_samples(const uint8_t* data, size_t length);
    void emit(int32_t left, int32_t right);
    void flush();
    AudioOutput* audio_{nullptr};
    TaskHandle_t task_{nullptr};
    std::atomic<bool> busy_{false};
    std::atomic<int> result_{None};
    std::string url_;
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
