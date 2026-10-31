#pragma once
#include <algorithm>
#include <cstdint>

namespace audio_mix {
inline int16_t music_sample(int16_t sample, int gain, bool ducked) {
    // Scale PCM rather than rounding fractional player volumes down to zero.
    return static_cast<int16_t>(static_cast<int32_t>(sample) * gain / (ducked ? 200 : 100));
}

inline int16_t add(int16_t music, int16_t speech) {
    return static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(music) + speech, -32768, 32767));
}

inline int16_t half_output(int16_t sample) {
    return sample / 2;
}

inline int16_t add_half_output(int16_t music, int16_t speech) {
    return static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(music) + speech, -16384, 16383));
}
}
