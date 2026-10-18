#include "mic_denoise.h"
#include <cassert>
#include <cmath>
#include <iostream>

static double level(int frequency, bool enabled, float amplitude) {
    MicDenoise filter(enabled, 100);
    double energy = 0;
    for (int i = 0; i < 48000; ++i) {
        // Pre-gain PCM units; compare quiet noise-like and strong voice-like tones.
        float out = filter.process(amplitude * std::sin(2 * 3.141592653589793 * frequency * i / 16000));
        assert(std::isfinite(out));
        if (i >= 16000) energy += out * out;
    }
    return energy;
}
int main() {
    MicDenoise bypass(false, 180);
    for (int i = -2000; i < 2000; ++i) assert(bypass.process(float(i)) == i);
    // WOLA/FFT must reconstruct perfectly (apart from documented fixed delay)
    // when gains are unity, including across block boundaries and arbitrary calls.
    MicDenoise identity(true, 100, 0);
    for (int i = 0; i < 32000; ++i) {
        const float input = 123 * std::sin(i * 0.17) + 33 * std::cos(i * 0.037);
        const float output = identity.process(input);
        const int j = i - int(MicDenoise::DELAY);
        const float expected = j < 0 ? 0 : 123 * std::sin(j * 0.17) + 33 * std::cos(j * 0.037);
        assert(std::isfinite(output));
        assert(std::abs(output - expected) < 0.002f);
    }
    MicDenoise silence(true, 180);
    for (int i = 0; i < 48000; ++i) assert(silence.process(0) == 0);
    const double rejection = 10 * std::log10(level(140, true, 20) / level(140, false, 20));
    const double preservation = 10 * std::log10(level(1000, true, 200) / level(1000, false, 200));
    assert(rejection < -8);
    assert(preservation > -1);
    const double strong_same_band = 10 * std::log10(level(140, true, 500) / level(140, false, 500));
    assert(strong_same_band > -1); // Suppression must release for strong signal in a noisy band.
    std::cout << "Denoise tests passed: weak 140 Hz " << rejection
              << " dB, strong 1 kHz " << preservation << " dB\n";
}
