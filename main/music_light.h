#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct MusicLightLevels {
    uint8_t level{}, bass{}, mid{}, treble{};
};

// Audio-writer only. Two integer one-pole filters per stereo channel; no FFT,
// allocation or floating-point work. Independent channels preserve anti-phase bass.
class MusicBandMeter {
public:
    MusicLightLevels analyze(const uint8_t* pcm, size_t bytes) {
        int peak = 0, bass = 0, mid = 0, treble = 0;
        for (size_t frame = 0; frame + 4 <= bytes; frame += 4) {
            for (size_t channel = 0; channel < 2; ++channel) {
                int16_t sample;
                std::memcpy(&sample, pcm + frame + channel * 2, sizeof(sample));
                slow_[channel] += (sample - slow_[channel]) / 32;
                fast_[channel] += (sample - fast_[channel]) / 4;
                peak = std::max(peak, std::abs(int(sample)));
                bass = std::max<int>(bass, std::abs(slow_[channel]));
                mid = std::max<int>(mid, std::abs(fast_[channel] - slow_[channel]));
                treble = std::max<int>(treble, std::abs(sample - fast_[channel]));
            }
        }
        return {scale(peak, 96), scale(bass, 64), scale(mid, 48), scale(treble, 24)};
    }
private:
    static uint8_t scale(int value, int divisor) {
        return static_cast<uint8_t>(std::min(255, value / divisor));
    }
    int32_t slow_[2]{}, fast_[2]{};
};

struct MusicLightRgb { uint8_t r{}, g{}, b{}; };

// Main-loop only, one update per 25 ms. Preserve the colour glide while giving
// bass attacks, sustained mids and short treble accents separate visual envelopes.
class MusicLight {
public:
    MusicLightRgb update(MusicLightLevels input) {
        bass_ = envelope(bass_, input.bass, 4);
        mid_ = envelope(mid_, input.mid, 8);
        treble_ = envelope(treble_, input.treble, 2);
        level_ = envelope(level_, input.level, 4);
        beat_ = envelope(beat_, 0, 4);
        if (cooldown_) --cooldown_;
        const int average = bass_average_ / 256;
        // An onset must rise above both recent bass and the previous update.
        // Sustained bass must not turn into repeated flashes at the cooldown rate.
        if (!cooldown_ && input.bass >= 20 &&
            input.bass > average + std::max(10, average / 4) &&
            input.bass > previous_bass_ + std::max(8, previous_bass_ / 6)) {
            beat_ = std::min(255, 80 + 2 * (input.bass - average));
            cooldown_ = 5; // At least 125 ms between accents.
        }
        bass_average_ += (int(input.bass) * 256 - bass_average_) / 16;
        previous_bass_ = input.bass;
        if (input.level || input.bass || input.mid || input.treble)
            hue_ = (hue_ + 1 + input.treble / 48) % 768;

        const int energy = bass_ + mid_ + treble_;
        const int value = std::min(255, (2 * bass_ + mid_ + level_) / 4 +
                                        treble_ / 8 + beat_ / 4);
        if (!energy || !value) return {};
        const int step = hue_ % 256;
        int r = 0, g = 0, b = 0;
        switch (hue_ / 256) {
            case 0: r = 255 - step; g = step; break;
            case 1: g = 255 - step; b = step; break;
            default: b = 255 - step; r = step; break;
        }
        // Keep most of the existing rainbow; add warm bass, green/cyan mids,
        // and violet/blue treble. The balance follows the current music.
        r = (2 * r + (255 * bass_ + 35 * mid_ + 90 * treble_) / energy) / 3;
        g = (2 * g + (55 * bass_ + 230 * mid_ + 100 * treble_) / energy) / 3;
        b = (2 * b + (15 * bass_ + 155 * mid_ + 255 * treble_) / energy) / 3;
        // Brief pale highlights on attacks, without replacing the underlying colour.
        const int highlight = std::max(beat_ / 5, int(input.treble) / 8);
        auto channel = [value, highlight](int color) {
            color += (255 - color) * highlight / 255;
            return static_cast<uint8_t>(color * value / 255);
        };
        return {channel(r), channel(g), channel(b)};
    }
private:
    static int envelope(int previous, int input, int decay) {
        return std::max(input, previous - std::max(1, previous / decay));
    }
    int bass_{}, mid_{}, treble_{}, level_{}, beat_{};
    int bass_average_{}, previous_bass_{}, cooldown_{}, hue_{};
};
