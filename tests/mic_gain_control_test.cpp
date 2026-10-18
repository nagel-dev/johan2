#include "mic_gain_control.h"
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    std::array<float, 256> input{};
    std::array<int16_t, 256> output{};
    MicGainControl bypass(false, 4, 16);
    for (size_t i = 0; i < input.size(); ++i) input[i] = (int(i) - 128) * 99.13f;
    bypass.process(input.data(), output.data(), input.size());
    for (size_t i = 0; i < input.size(); ++i) assert(output[i] == MicPcm::quantize(input[i], 4));

    MicGainControl control(true, 4, 16);
    input.fill(0);
    for (int block = 0; block < 1250; ++block) control.process(input.data(), output.data(), input.size());
    assert(control.gain() == 4); // Silence cannot wind gain up.
    input.fill(10);
    for (int block = 0; block < 1250; ++block) control.process(input.data(), output.data(), input.size());
    assert(control.gain() == 4); // Low residual noise cannot wind gain up either.

    auto voice = [&]() {
        for (size_t i = 0; i < input.size(); ++i) input[i] = 150 * std::sin(i * 0.3);
        control.process(input.data(), output.data(), input.size());
    };
    voice();
    assert(control.gain() > 4 && control.gain() < 4.2f); // Slow, bounded release.
    for (int block = 0; block < 1000; ++block) voice();
    assert(control.gain() > 15.9f && control.gain() <= 16);

    input.fill(0);
    for (int block = 0; block < 6; ++block) control.process(input.data(), output.data(), input.size());
    assert(control.gain() > 15.5f); // A 96 ms word gap must not reset speech gain.

    // Unexpected signed full-scale peaks, including the FIRST sample: zero clipping.
    input.fill(100);
    input[0] = -80000;
    input[255] = 80000;
    control.process(input.data(), output.data(), input.size());
    for (int16_t sample : output) assert(std::abs(int(sample)) <= 27853);
    assert(control.gain() < 0.35f); // Reduction below unity must be possible.
    const float after_peak = control.gain();
    input.fill(0);
    for (int block = 0; block < 100; ++block) control.process(input.data(), output.data(), input.size());
    assert(control.gain() >= after_peak && control.gain() <= 4); // Quiet recovery is capped at baseline.
    for (int block = 0; block < 1000; ++block) voice();
    assert(control.gain() > 15.9f);

    // Sustained music-like signal remains bounded; processing accepts partial reads.
    for (int block = 0; block < 200; ++block) {
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = 12000 * std::sin((block * 256 + i) * 0.17);
        const size_t count = block % 2 ? 256 : 73;
        control.process(input.data(), output.data(), count);
        for (size_t i = 0; i < count; ++i) assert(std::abs(int(output[i])) <= 27853);
    }
    assert(control.gain() < 4);
    const float before_empty = control.gain();
    control.process(nullptr, nullptr, 0);
    assert(control.gain() == before_empty);
    std::cout << "Microphone gain-control tests passed\n";
}
