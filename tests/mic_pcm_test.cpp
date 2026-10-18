#include "mic_pcm.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
    MicPcm positive(0), negative(0);
    assert(positive.convert(0x40000000, 1) == 16320);
    assert(negative.convert(-0x40000000, 1) == -16320);
    MicPcm loud_positive, loud_negative;
    assert(loud_positive.convert(0x40000000, 16) == 32767);
    assert(loud_negative.convert(-0x40000000, 16) == -32768);
    MicPcm zero;
    for (int i = 0; i < 16000; ++i) assert(zero.convert(0, 16) == 0);
    MicPcm offset;
    int last = 0;
    for (int i = 0; i < 16000; ++i) last = offset.convert(0x10000000, 16);
    assert(std::abs(last) <= 2); // A constant input is removed rather than amplified forever.
    MicPcm symmetric_positive, symmetric_negative;
    for (int i = 0; i < 16000; ++i) {
        const int32_t word = int32_t(std::sin(i * 0.13) * 100000000);
        assert(symmetric_positive.convert(word, 16) == -symmetric_negative.convert(-word, 16));
    }
    // Measure steady-state response through the actual PCM converter, avoiding clipping.
    auto response_db = [](int cutoff, double frequency) {
        MicPcm filter(cutoff);
        double input_energy = 0, output_energy = 0;
        for (int i = 0; i < 32000; ++i) {
            const int32_t word = int32_t(std::sin(2 * 3.141592653589793 * frequency * i / 16000) * 100000000);
            const int16_t output = filter.convert(word, 1);
            if (i >= 16000) {
                const double input = word / 65536.0;
                input_energy += input * input;
                output_energy += double(output) * output;
            }
        }
        return 10 * std::log10(output_energy / input_energy);
    };
    assert(response_db(100, 30) < -20);
    assert(response_db(100, 50) < -12);
    assert(response_db(100, 100) > -3.3 && response_db(100, 100) < -2.8);
    assert(response_db(100, 500) > -0.2);
    assert(response_db(100, 1000) > -0.2);
    assert(response_db(0, 30) > -1); // Disabling preserves the legacy response.
    assert(response_db(300, 100) < -18); // Configured cutoff is actually applied.
    std::cout << "Microphone PCM tests passed\n";
}
