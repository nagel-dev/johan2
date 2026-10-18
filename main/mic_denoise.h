#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include "mic_noise_profile.h"

// Profile-based spectral suppression at 16 kHz. Capture-task only, fixed storage,
// no allocation/networking/logging. 512-sample (32 ms) streaming delay when on.
class MicDenoise {
public:
    static constexpr size_t N = 512, HOP = 256, DELAY = 512;
    explicit MicDenoise(bool enabled, int highpass_hz, float noise_scale = 1) : enabled_(enabled) {
        constexpr float PI = 3.14159265358979323846f;
        for (size_t i = 0; i < N; ++i)
            window_[i] = std::sqrt(std::max(0.0f, 0.5f - 0.5f * std::cos(2 * PI * i / N)));
        for (size_t i = 0; i < N / 2; ++i) {
            cosine_[i] = std::cos(2 * PI * i / N);
            sine_[i] = std::sin(2 * PI * i / N);
        }
        const float cutoff_k = std::tan(PI * highpass_hz / 16000.0f);
        for (size_t i = 0; i <= N / 2; ++i) {
            float response = 1;
            if (highpass_hz > 0) {
                const float ratio = std::tan(PI * i / N) / cutoff_k;
                const float power = ratio * ratio * ratio * ratio;
                response = power / (1 + power);
            }
            const float omega = 2 * PI * i / N;
            constexpr float pole = 255.0f / 256;
            const float dc = pole * pole * (2 - 2 * std::cos(omega)) /
                             (1 + pole * pole - 2 * pole * std::cos(omega));
            noise_[i] = MIC_NOISE_POWER[i] * response * dc * noise_scale;
            gain_[i] = 1;
        }
    }
    float process(float sample) {
        if (!enabled_) return sample;
        const float output = output_[position_];
        history_[HOP + position_] = sample;
        if (++position_ == HOP) {
            frame();
            position_ = 0;
        }
        return output;
    }
private:
    void fft(bool inverse) {
        for (size_t i = 1, j = 0; i < N; ++i) {
            size_t bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(real_[i], real_[j]); std::swap(imag_[i], imag_[j]); }
        }
        for (size_t length = 2; length <= N; length *= 2) {
            for (size_t base = 0; base < N; base += length) {
                for (size_t j = 0; j < length / 2; ++j) {
                    const size_t k = j * N / length, a = base + j, b = a + length / 2;
                    const float wi = inverse ? sine_[k] : -sine_[k];
                    const float tr = cosine_[k] * real_[b] - wi * imag_[b];
                    const float ti = cosine_[k] * imag_[b] + wi * real_[b];
                    real_[b] = real_[a] - tr; imag_[b] = imag_[a] - ti;
                    real_[a] += tr; imag_[a] += ti;
                }
            }
        }
        if (inverse) for (size_t i = 0; i < N; ++i) { real_[i] /= N; imag_[i] /= N; }
    }
    void frame() {
        for (size_t i = 0; i < N; ++i) { real_[i] = history_[i] * window_[i]; imag_[i] = 0; }
        fft(false);
        for (size_t i = 0; i <= N / 2; ++i) {
            const float power = real_[i] * real_[i] + imag_[i] * imag_[i];
            // Smooth power and gains to reduce flutter and isolated tonal artifacts.
            power_[i] = 0.6f * power_[i] + 0.4f * power;
            target_[i] = noise_[i] > 0 ? std::sqrt(std::max(0.01f,
                1.0f - 2.0f * noise_[i] / std::max(power_[i], 1e-12f))) : 1.0f;
        }
        for (size_t i = 0; i <= N / 2; ++i) {
            float target = target_[i];
            if (i > 0 && i < N / 2 && noise_[i] > 0)
                target = std::max(target, 0.25f * target_[i - 1] + 0.5f * target + 0.25f * target_[i + 1]);
            const float speed = target > gain_[i] ? 0.8f : 0.2f;
            gain_[i] += speed * (target - gain_[i]);
            real_[i] *= gain_[i]; imag_[i] *= gain_[i];
            if (i > 0 && i < N / 2) { real_[N - i] *= gain_[i]; imag_[N - i] *= gain_[i]; }
        }
        fft(true);
        for (size_t i = 0; i < HOP; ++i) {
            output_[i] = overlap_[i] + real_[i] * window_[i];
            overlap_[i] = real_[i + HOP] * window_[i + HOP];
            history_[i] = history_[i + HOP];
        }
    }
    bool enabled_;
    size_t position_ = 0;
    float history_[N]{}, window_[N]{}, real_[N]{}, imag_[N]{};
    float cosine_[N / 2]{}, sine_[N / 2]{};
    float overlap_[HOP]{}, output_[HOP]{};
    float noise_[N / 2 + 1]{}, power_[N / 2 + 1]{}, gain_[N / 2 + 1]{}, target_[N / 2 + 1]{};
};
