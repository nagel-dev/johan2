#include "playback_reference.h"
#include <cassert>
#include <cmath>
#include <iostream>

static constexpr double pi = 3.14159265358979323846;
static void tone(PlaybackReference& ref, int hz, int blocks, int64_t start_us = 1000000) {
    int16_t stereo[480];
    for (int block = 0; block < blocks; ++block) {
        for (int i = 0; i < 240; ++i) {
            const int sample = block * 240 + i;
            stereo[i * 2] = stereo[i * 2 + 1] = 10000 * std::sin(2 * pi * hz * sample / 48000);
        }
        ref.push(stereo, start_us + (block + 1) * 5000 - 21);
    }
}
int main() {
    PlaybackReference ref;
    int16_t value = 123;
    assert(!ref.sample(1000000, value) && value == 0);
    tone(ref, 1000, 80);
    assert(!ref.sample(1000000, value)); // History wrap never replays stale data.
    double error = 0, power = 0;
    for (int i = 0; i < 4000; ++i) {
        const double time = 1100000 + i * 62.5;
        assert(ref.sample(time, value));
        const double expected = 10000 * std::sin(2 * pi * 1000 * (time - 1000000) / 1000000);
        error += (value - expected) * (value - expected);
        power += expected * expected;
    }
    assert(std::sqrt(error / power) < 0.03); // Passband + group-delay/timestamp alignment.
    assert(!ref.sample(1600000, value) && value == 0);
    assert(!ref.recent(1600000));

    PlaybackReference alias;
    tone(alias, 12000, 40);
    double residual = 0;
    for (int i = 0; i < 1000; ++i) {
        assert(alias.sample(1100000 + i * 62.5, value));
        residual += double(value) * value;
    }
    assert(std::sqrt(residual / 1000) < 50); // Prevent playback above Nyquist aliasing into AEC.

    int16_t stereo[480]{};
    ref.push(stereo, 1500000); // Deliberate missing DMA blocks.
    assert(ref.gaps() == 1);
    assert(!ref.sample(1450000, value) && value == 0);
    assert(ref.sample(1497000, value) && value == 0);
    // Downmix cannot overflow at either PCM rail.
    for (auto& sample : stereo) sample = 32767;
    for (int i = 0; i < 4; ++i) ref.push(stereo, 1505000 + i * 5000);
    assert(ref.sample(1516000, value) && value > 32000);
    for (auto& sample : stereo) sample = -32768;
    for (int i = 0; i < 4; ++i) ref.push(stereo, 1525000 + i * 5000);
    assert(ref.sample(1536000, value) && value < -32000);
    std::cout << "Playback reference tests passed\n";
}
