#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "mic_pcm.h"

// Post-denoise, pre-quantization gain control at 16 kHz. Capture-task local;
// fixed storage, bounded two-pass block processing, no additional audio delay.
class MicGainControl {
public:
    MicGainControl(bool enabled, float fixed_gain, float maximum_gain)
        : enabled_(enabled), maximum_(std::max(1.0f, maximum_gain)),
          baseline_(enabled ? std::min(fixed_gain, maximum_) : fixed_gain), gain_(baseline_) {}

    void process(const float* input, int16_t* output, size_t count) {
        if (!count) return;
        if (!enabled_) {
            for (size_t i = 0; i < count; ++i) output[i] = MicPcm::quantize(input[i], baseline_);
            return;
        }
        float peak = 0;
        double squares = 0;
        for (size_t i = 0; i < count; ++i) {
            peak = std::max(peak, std::abs(input[i]));
            squares += double(input[i]) * input[i];
        }
        const float rms_squared = squares / count;
        const float rms = std::sqrt(rms_squared);
        const float seconds = count / SAMPLE_RATE;
        const float level_weight = 1.0f - std::exp(-seconds / LEVEL_SECONDS);
        level_squared_ += level_weight * (rms_squared - level_squared_);
        // Immediate peak acquisition, slow envelope decay: do not pump between peaks.
        peak_ = std::max(peak, peak_ * std::exp(-seconds / PEAK_HOLD_SECONDS));
        const float ceiling = std::min(maximum_, HEADROOM / std::max(peak_, 1e-12f));
        // Keep level control active over brief gaps between words, then return
        // only to baseline. This is an energy gate, not a speech classifier.
        if (rms >= ACTIVITY_RMS) active_samples_ = ACTIVITY_HOLD_SAMPLES;
        else active_samples_ = count >= active_samples_ ? 0 : active_samples_ - count;
        float target = active_samples_ > 0
            ? std::min(maximum_, TARGET_RMS / std::max(1e-12f, std::max(rms, std::sqrt(level_squared_))))
            : baseline_;
        target = std::min(target, ceiling);
        // The whole block is already captured. Apply its safety ceiling before
        // quantization, including the very first sample of an unexpected peak.
        gain_ = std::min(gain_, ceiling);
        for (size_t i = 0; i < count; ++i) {
            const float rate = target < gain_ ? ATTACK : RELEASE;
            gain_ += rate * (target - gain_);
            output[i] = MicPcm::quantize(input[i], gain_);
        }
    }
    float gain() const { return gain_; }
private:
    static constexpr float SAMPLE_RATE = 16000.0f;
    static constexpr float HEADROOM = 0.85f * 32767;
    static constexpr float TARGET_RMS = 0.125f * 32768; // about -18 dBFS
    static constexpr float ACTIVITY_RMS = 32; // about -60 dBFS, before gain
    static constexpr size_t ACTIVITY_HOLD_SAMPLES = 4800; // 300 ms between words
    static constexpr float LEVEL_SECONDS = 0.080f;
    static constexpr float PEAK_HOLD_SECONDS = 0.400f;
    static constexpr float ATTACK = 0.00623051f; // 10 ms time constant at 16 kHz
    static constexpr float RELEASE = 0.0000416658f; // 1.5 s time constant
    bool enabled_;
    float maximum_, baseline_, gain_;
    float peak_ = 0, level_squared_ = 0;
    size_t active_samples_ = 0;
};
