#include "speaker_probe.h"
#include <cassert>
#include <iostream>
#include <vector>

static std::vector<int16_t> reference(int amplitude) {
    std::vector<int16_t> stereo(SpeakerProbePcm::kFrames * 2);
    for (size_t first = 0; first < SpeakerProbePcm::kFrames;) {
        const size_t n = SpeakerProbePcm::render(stereo.data() + first * 2, first, 137, amplitude);
        assert(n > 0 && n <= 137);
        first += n;
    }
    std::vector<int16_t> mono(SpeakerProbePcm::kFrames / 3);
    for (size_t i = 0; i < mono.size(); ++i) {
        assert(stereo[i * 6] == stereo[i * 6 + 1]);
        assert(std::abs(stereo[i * 6]) <= amplitude + 2);
        mono[i] = stereo[i * 6];
    }
    int16_t untouched[2] = {123, 456};
    assert(!SpeakerProbePcm::render(untouched, SpeakerProbePcm::kFrames, 1, amplitude));
    assert(untouched[0] == 123 && untouched[1] == 456);
    return mono;
}
static SpeakerProbeResult measure(int amplitude, float gain, int delay, bool unrelated = false, bool clipping = false, size_t offset = 0) {
    const auto ref = reference(amplitude);
    std::vector<int16_t> samples(ref.size() * 4);
    uint32_t noise = 42;
    for (size_t i = 0; i < ref.size() - offset; ++i) {
        noise = noise * 1664525 + 1013904223;
        const float random = (int(noise >> 16) - 32768) / 32768.0f;
        const size_t at = i + offset;
        float hum = 200 * std::sin(6.28318530718 * 140 * at / 16000);
        for (float hz : SpeakerProbePcm::kHz) hum += 100 * std::sin(6.28318530718 * hz * at / 16000);
        const float echo = at >= size_t(delay) ? ref[at - delay] * gain : 0;
        samples[i * 4] = clipping ? 32767 : static_cast<int16_t>(8000 + hum + random * 20 + (unrelated ? 0 : echo));
        samples[i * 4 + 1] = ref[at];
    }
    return analyze_speaker_probe(samples.data(), ref.size() - offset);
}
int main() {
    for (int amplitude : {256, 1024}) {
        for (float gain : {0.5f, 1.0f, 2.0f}) {
            for (int delay : {0, 80, 160}) {
                const auto r = measure(amplitude, gain, delay);
                assert(r.valid && !r.clipped);
                unsigned valid = 0;
                for (const auto& b : r.bands) if (b.valid) {
                    assert(std::abs(b.gain_db - 20 * std::log10(gain)) < 1.2f);
                    ++valid;
                }
                assert(valid >= 2);
            }
        }
    }
    assert(!measure(1024, 1, 0, true).valid); // Hum/DC/noise do not identify Bose level.
    assert(!measure(1024, 1, 0, false, true).valid); // ADC overload must not look credible.
    assert(!measure(0, 1, 0).valid); // Silent/muted DAC reference.
    for (size_t offset : {0, 127, 255, 511}) {
        const auto r = measure(256, 1, 80, false, false, offset);
        assert(r.valid);
        for (const auto& band : r.bands) if (band.valid) assert(std::abs(band.gain_db) < 0.4f);
    }
    std::cout << "Speaker probe: bass PCM, source-level compensation, gain/delay, hum/noise/silence and clipping passed\n";
}
