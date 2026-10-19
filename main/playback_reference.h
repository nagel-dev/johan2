#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Capture-task-only history. DMA timestamps anchor each block independently so
// TX/RX scheduler delay and separate I2S clocks do not accumulate sample drift.
class PlaybackReference {
public:
    static constexpr size_t kFrames = 240; // 5 ms at 48 kHz
    static constexpr size_t kSamples = 80;
    static constexpr size_t kBlocks = 64; // 320 ms
    static constexpr size_t kTaps = 63;
    PlaybackReference() {
        // Windowed-sinc low-pass before 3:1 decimation. 6 kHz cutoff, unity DC.
        double sum = 0;
        for (size_t i = 0; i < kTaps; ++i) {
            const int n = int(i) - int(kTaps / 2);
            const double sinc = n ? std::sin(2 * kPi * 6000 * n / 48000) / (kPi * n) : 0.25;
            taps_[i] = sinc * (0.54 - 0.46 * std::cos(2 * kPi * i / (kTaps - 1)));
            sum += taps_[i];
        }
        for (auto& tap : taps_) tap /= sum;
    }
    void push(const int16_t* stereo, int64_t end_us) {
        const bool gap = last_end_ && std::abs(end_us - last_end_ - 5000) > 2000;
        const bool warming = !last_end_ || gap;
        if (gap) {
            // Missing DMA blocks must not be silently joined into a continuous signal.
            std::fill_n(history_, kTaps, 0.0f);
            ++gaps_;
        }
        last_end_ = end_us;
        Block& block = blocks_[next_];
        // FIR group delay is 31 source frames. First output is at source index 2.
        block.first_tick = end_us * 2 - ((kFrames - 1 - 2 + kTaps / 2) * 125 + 1) / 3;
        block.valid_from = warming ? 21 : 0;
        for (size_t i = 0; i < kFrames; ++i) {
            history_[head_] = (int32_t(stereo[i * 2]) + stereo[i * 2 + 1]) / 2.0f;
            head_ = (head_ + 1) % kTaps;
            if (i % 3 != 2) continue;
            float value = 0;
            size_t position = head_ ? head_ - 1 : kTaps - 1;
            for (size_t j = 0; j < kTaps; ++j) {
                value += taps_[j] * history_[position];
                position = position ? position - 1 : kTaps - 1;
            }
            block.pcm[i / 3] = static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
        }
        next_ = (next_ + 1) % kBlocks;
        used_ = std::min(used_ + 1, kBlocks);
    }
    // Returns false for stale/missing audio; never repeats the last music sample.
    bool sample(double at_us, int16_t& out) const { return sample_ticks(static_cast<int64_t>(at_us * 2), out); }
    // Half-microsecond ticks: integer timestamp arithmetic avoids software-double
    // operations in the ESP32-S3's real-time processing loop. 16 kHz = 125 ticks.
    bool sample_ticks(int64_t at_tick, int16_t& out) const {
        out = 0;
        for (size_t n = 0; n < used_; ++n) {
            const Block& block = blocks_[(next_ + kBlocks - 1 - n) % kBlocks];
            const int64_t offset = at_tick - block.first_tick;
            if (offset < -62) continue;
            // Half-sample tolerance at a block edge; interpolation within the block.
            if (offset >= int64_t(kSamples * 125 + 62)) return false;
            const int position = static_cast<int>(std::clamp<int64_t>(offset, 0, (kSamples - 1) * 125));
            const size_t i = position / 125;
            if (i < block.valid_from) return false;
            const float fraction = (position % 125) / 125.0f;
            out = static_cast<int16_t>(block.pcm[i] + fraction *
                  (block.pcm[std::min(i + 1, kSamples - 1)] - block.pcm[i]));
            return true;
        }
        return false;
    }
    bool recent(int64_t now) const { return last_end_ && now - last_end_ < 200000; }
    uint32_t gaps() const { return gaps_; }
private:
    static constexpr double kPi = 3.14159265358979323846;
    struct Block { int64_t first_tick = 0; size_t valid_from = 0; int16_t pcm[kSamples]{}; };
    Block blocks_[kBlocks]{};
    float taps_[kTaps]{}, history_[kTaps]{};
    size_t head_ = 0, next_ = 0, used_ = 0;
    int64_t last_end_ = 0;
    uint32_t gaps_ = 0;
};
