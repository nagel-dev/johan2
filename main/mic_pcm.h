#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// 16 kHz signed, left-aligned I2S -> Assist PCM. All state is capture-task local.
class MicPcm {
public:
    // 0 retains the original DC-removal-only path for A/B comparison.
    explicit MicPcm(int highpass_hz = 100) : highpass_(highpass_hz > 0) {
        if (!highpass_) return;
        const float k = std::tan(3.14159265358979323846f * highpass_hz / 16000.0f);
        const float norm = 1.0f / (1.0f + std::sqrt(2.0f) * k + k * k);
        b0_ = norm;
        b1_ = -2.0f * norm;
        b2_ = norm;
        a1_ = 2.0f * (k * k - 1.0f) * norm;
        a2_ = (1.0f - std::sqrt(2.0f) * k + k * k) * norm;
    }
    float filter(int32_t word) {
        const float sample = word / 65536.0f;
        dc_ += (sample - dc_) / 256.0f;
        float filtered = sample - dc_;
        if (highpass_) {
            // Second-order Butterworth high-pass, transposed direct form II.
            const float output = b0_ * filtered + z1_;
            z1_ = b1_ * filtered - a1_ * output + z2_;
            z2_ = b2_ * filtered - a2_ * output;
            filtered = output;
        }
        return filtered;
    }
    static int16_t quantize(float filtered, float gain) {
        return static_cast<int16_t>(std::clamp(filtered * gain, -32768.0f, 32767.0f));
    }
    int16_t convert(int32_t word, int gain) {
        return quantize(filter(word), gain);
    }
private:
    bool highpass_;
    float dc_ = 0;
    float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_ = 0, z2_ = 0;
};
