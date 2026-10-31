#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Three bass tones at exact 16 kHz DFT bins, separated by silence. The source
// level is bounded; measurement uses the actual DMA reference.
struct SpeakerProbePcm {
    static constexpr size_t kRate = 48000, kFrames = kRate * 8;
    static constexpr int kAmplitude = 256;
    static constexpr float kHz[3] = {93.75f, 156.25f, 218.75f};
    static size_t render(int16_t* stereo, size_t offset, size_t capacity, int amplitude) {
        const size_t frames = std::min(capacity, kFrames - std::min(offset, kFrames));
        amplitude = std::clamp(amplitude, 0, 2048);
        for (size_t i = 0; i < frames; ++i) {
            const size_t at = offset + i;
            const int segment = int(at / (2 * kRate));
            const size_t local = at % (2 * kRate);
            int16_t sample = 0;
            if (segment < 3 && local >= kRate / 2) {
                const size_t t = local - kRate / 2, length = 3 * kRate / 2;
                const size_t fade = kRate / 50;
                const float envelope = std::min({t, length - 1 - t, fade}) / float(fade);
                if (t < length) {
                    // Polynomial sine with range reduction: no per-sample libm call.
                    const float phase = ((t * size_t(3 + segment * 2)) % 1536) / 1536.0f;
                    float x = phase <= 0.5f ? phase * 6.28318530718f : (phase - 1) * 6.28318530718f;
                    const float x2 = x * x;
                    const float sine = x * (1 + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 / 362880))));
                    sample = static_cast<int16_t>(amplitude * envelope * sine);
                }
            }
            stereo[2 * i] = stereo[2 * i + 1] = sample;
        }
        return frames;
    }
};

struct SpeakerProbeResult {
    struct Band { float gain_db = 0, coherence = 0; unsigned windows = 0; bool valid = false; } bands[3];
    bool valid = false;
    unsigned clipped = 0;
};

// Runs only after on-demand capture, in the HTTP task. No continuous DSP/ISR work.
// H1 = averaged mic/reference cross spectrum divided by reference power.
// Coherence is averaged over independent windows, not a single-frame score.
inline SpeakerProbeResult analyze_speaker_probe(const int16_t* interleaved, size_t count, void (*yield_work)() = nullptr) {
    constexpr size_t N = 512;
    float xx[3]{}, yy[3]{}, cr[3]{}, ci[3]{};
    float background_r[3]{}, background_i[3]{};
    unsigned background_windows = 0, quiet_windows = 0, consecutive[3]{};
    SpeakerProbeResult result;
    // Estimate stationary tonal interference during the probe's silent sections.
    // Integer-cycle DFT bins keep its phase comparable between adjacent windows.
    for (size_t first = 0; first + N <= count; first += N) {
        if (yield_work && first && first % (16 * N) == 0) yield_work();
        float power = 0, mean = 0;
        for (size_t n = 0; n < N; ++n) {
            const float ref = interleaved[(first + n) * 4 + 1] / 32768.0f;
            power += ref * ref; mean += interleaved[(first + n) * 4] / 32768.0f;
        }
        if (power / N >= 1e-7f) { quiet_windows = 0; continue; }
        if (++quiet_windows <= 10) continue; // Exclude speaker/room decay from background.
        mean /= N;
        for (int b = 0; b < 3; ++b) {
            const float w = 6.28318530718f * SpeakerProbePcm::kHz[b] / 16000;
            const float cosine = std::cos(w), sine = std::sin(w);
            float y1 = 0, y2 = 0;
            for (size_t n = 0; n < N; ++n) {
                const float y = interleaved[(first + n) * 4] / 32768.0f - mean + 2 * cosine * y1 - y2;
                y2 = y1; y1 = y;
            }
            background_r[b] += y1 - cosine * y2; background_i[b] += sine * y2;
        }
        ++background_windows;
    }
    if (background_windows < 16) return result;
    for (int b = 0; b < 3; ++b) {
        background_r[b] /= background_windows; background_i[b] /= background_windows;
    }
    for (size_t first = 0; first + N <= count; first += N) {
        if (yield_work && first && first % (16 * N) == 0) yield_work();
        float mx = 0, my = 0;
        for (size_t n = 0; n < N; ++n) {
            mx += interleaved[(first + n) * 4 + 1] / 32768.0f;
            my += interleaved[(first + n) * 4] / 32768.0f;
        }
        mx /= N; my /= N;
        float ref_power = 0;
        for (size_t n = 0; n < N; ++n) {
            const float x = interleaved[(first + n) * 4 + 1] / 32768.0f - mx;
            ref_power += x * x;
        }
        if (ref_power / N < 1e-7f) {
            std::fill_n(consecutive, 3, 0);
            continue;
        }
        for (int band = 0; band < 3; ++band) {
            const float w = 6.28318530718f * SpeakerProbePcm::kHz[band] / 16000;
            const float cosine = std::cos(w), sine = std::sin(w), coefficient = 2 * cosine;
            float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            unsigned clipped = 0;
            for (size_t n = 0; n < N; ++n) {
                const int16_t mic = interleaved[(first + n) * 4];
                clipped += mic >= 32760 || mic <= -32760;
                const float x = interleaved[(first + n) * 4 + 1] / 32768.0f - mx + coefficient * x1 - x2;
                const float y = mic / 32768.0f - my + coefficient * y1 - y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
            }
            const float xr = x1 - cosine * x2, xi = sine * x2;
            const float yr = y1 - cosine * y2 - background_r[band], yi = sine * y2 - background_i[band];
            const float px = xr * xr + xi * xi;
            // Accept only windows dominated by this test tone, excluding music,
            // silence and most transitions. A hum-only mic cannot supply reference.
            if (2 * px < 0.85f * N * ref_power) { consecutive[band] = 0; continue; }
            if (++consecutive[band] <= 10) continue; // 320 ms for speaker/room settling.
            result.clipped += clipped;
            xx[band] += px; yy[band] += yr * yr + yi * yi;
            cr[band] += yr * xr + yi * xi; ci[band] += yi * xr - yr * xi;
            ++result.bands[band].windows;
        }
    }
    unsigned accepted = 0;
    for (int b = 0; b < 3; ++b) {
        auto& band = result.bands[b];
        const float cross = cr[b] * cr[b] + ci[b] * ci[b];
        if (xx[b] > 0 && yy[b] > 0) {
            band.coherence = std::clamp(cross / (xx[b] * yy[b]), 0.0f, 1.0f);
            band.gain_db = 10 * std::log10(std::max(cross / (xx[b] * xx[b]), 1e-20f));
        }
        band.valid = band.windows >= 12 && band.coherence >= 0.8f && !result.clipped;
        accepted += band.valid;
    }
    result.valid = accepted >= 2;
    return result;
}
