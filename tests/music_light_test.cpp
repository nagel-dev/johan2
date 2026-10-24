#include "music_light.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static MusicLightLevels tone(int hz, bool left, bool right, bool opposite = false) {
    MusicBandMeter meter;
    int16_t pcm[480 * 2]{};
    MusicLightLevels levels{};
    for (int block = 0; block < 10; ++block) {
        for (int i = 0; i < 480; ++i) {
            const int16_t sample = 6000 * std::sin(2 * 3.141592653589793 * hz *
                                                  (block * 480 + i) / 48000);
            pcm[i * 2] = left ? sample : 0;
            pcm[i * 2 + 1] = right ? (opposite ? -sample : sample) : 0;
        }
        levels = meter.analyze(reinterpret_cast<uint8_t*>(pcm), sizeof(pcm));
    }
    return levels;
}

static int brightness(MusicLightRgb color) { return std::max({color.r, color.g, color.b}); }

int main() {
    const auto bass = tone(90, true, true);
    const auto mid = tone(1000, true, true);
    const auto treble = tone(7000, true, true);
    assert(bass.bass > mid.bass * 3);
    assert(mid.mid > bass.mid * 2);
    assert(treble.treble > bass.treble * 5);
    const auto left = tone(90, true, false);
    const auto right = tone(90, false, true);
    const auto anti = tone(90, true, true, true);
    assert(left.level == right.level && left.bass == right.bass);
    assert(std::abs(int(anti.bass) - int(left.bass)) <= 1);

    // Arbitrary alignment and partial callback boundaries must preserve levels.
    int16_t pcm[256 * 2];
    for (int i = 0; i < 512; ++i) pcm[i] = (i & 1) ? -32768 : 32767;
    std::vector<uint8_t> unaligned(sizeof(pcm) + 1);
    std::memcpy(unaligned.data() + 1, pcm, sizeof(pcm));
    MusicBandMeter whole, split;
    const auto all = whole.analyze(unaligned.data() + 1, sizeof(pcm));
    const auto first = split.analyze(unaligned.data() + 1, 240 * 4);
    const auto last = split.analyze(unaligned.data() + 1 + 240 * 4, 16 * 4);
    assert(all.level == 255 && all.level == std::max(first.level, last.level));
    assert(all.bass == std::max(first.bass, last.bass));
    assert(all.mid == std::max(first.mid, last.mid));
    assert(all.treble == std::max(first.treble, last.treble));

    MusicLight light;
    assert(brightness(light.update({})) == 0);
    const int attack = brightness(light.update({180, 180, 100, 30}));
    // Sustained bass should settle, without retriggering flashes at 125 ms.
    int minimum = 255, maximum = 0;
    for (int i = 0; i < 80; ++i) {
        const int value = brightness(light.update({180, 180, 100, 30}));
        if (i > 40) { minimum = std::min(minimum, value); maximum = std::max(maximum, value); }
    }
    assert(attack > minimum);
    // The chromatic glide may change the brightest RGB channel; no hard pulses.
    assert(maximum - minimum < 40);
    for (int i = 0; i < 80; ++i) light.update({});
    assert(brightness(light.update({})) == 0);
    const int new_attack = brightness(light.update({180, 180, 100, 30}));
    assert(new_attack > 0);
    MusicLight low_color, high_color;
    const auto warm = low_color.update({180, 180, 0, 0});
    const auto cool = high_color.update({180, 0, 0, 180});
    // Compare chroma rather than brightness: bass attacks also brighten the LED.
    assert(warm.r > warm.b && int(cool.b) * warm.r > int(warm.b) * cool.r);
    std::cout << "Music light tests passed: stereo bands, PCM boundaries, attacks, decay and colour\n";
}
