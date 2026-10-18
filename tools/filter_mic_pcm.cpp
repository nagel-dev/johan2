// Offline audition of the exact firmware suppressor. stdin/stdout: signed 16-bit
// little-endian mono PCM, already HP/DC filtered at the specified cutoff and gain.
// Emits 512 trailing flush samples; discard first 512 output samples to align.
#include "mic_denoise.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Usage: filter_mic_pcm CUTOFF_HZ GAIN < in.pcm > out.pcm\n"; return 1; }
    const int cutoff = std::atoi(argv[1]), gain = std::atoi(argv[2]);
    if (gain < 1 || gain > 32 || cutoff < 0 || cutoff > 300) return 1;
    MicDenoise filter(true, cutoff);
    auto emit = [&](float sample) {
        const int16_t out = static_cast<int16_t>(std::clamp(filter.process(sample) * gain, -32768.0f, 32767.0f));
        const uint16_t bits = static_cast<uint16_t>(out);
        char bytes[] = {static_cast<char>(bits & 255), static_cast<char>(bits >> 8)};
        std::cout.write(bytes, 2);
    };
    char bytes[2];
    while (std::cin.read(bytes, 2)) {
        const uint16_t bits = uint8_t(bytes[0]) | (uint16_t(uint8_t(bytes[1])) << 8);
        const int32_t sample = bits < 32768 ? bits : int32_t(bits) - 65536;
        emit(sample / float(gain));
    }
    if (std::cin.gcount() != 0) return 1;
    for (size_t i = 0; i < MicDenoise::DELAY; ++i) emit(0);
}
