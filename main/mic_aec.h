#pragma once
#include "sdkconfig.h"
#include <cstddef>
#include <cstdint>
#if CONFIG_JOHAN2_MIC_AEC
#include "esp_aec.h"
#include "playback_reference.h"

// Owned exclusively by the microphone task. All storage is fixed; no allocation
// per frame. Raw, reference, and output retain matching sample positions for WAV QA.
class MicAec {
public:
    static constexpr size_t kFrame = 512;
    bool init();
    void set_delay_ms(int delay_ms);
    int delay_ms() const { return delay_ms_; }
    void push_reference(const int16_t* stereo, int64_t end_us) { reference_.push(stereo, end_us); }
    // Append up to one 256-sample microphone DMA block. True means 512 samples ready.
    bool append(const int32_t* raw, size_t count, int64_t end_us, bool discontinuity);
    const int32_t* raw() const { return raw_; }
    const int16_t* near() const { return near_; }
    const int16_t* reference() const { return ref_; }
    const int16_t* clean() const { return clean_; }
    bool ready() const { return handle_ != nullptr; }
    uint32_t frames() const { return frames_; }
    uint32_t missing() const { return missing_; }
    uint32_t gaps() const { return reference_.gaps(); }
private:
    int delay_ms_ = CONFIG_JOHAN2_MIC_AEC_DELAY_MS;
    PlaybackReference reference_;
    aec_handle_t* handle_ = nullptr;
    alignas(16) int16_t near_[kFrame]{}, ref_[kFrame]{}, clean_[kFrame]{};
    int32_t raw_[kFrame]{};
    size_t filled_ = 0;
    bool frame_reference_ok_ = true;
    uint32_t frames_ = 0, missing_ = 0;
};
#endif
