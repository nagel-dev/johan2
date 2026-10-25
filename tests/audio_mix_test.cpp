#include "audio_mix.h"
#include <cassert>
#include <iostream>

int main() {
    for (int gain = 0; gain <= 100; ++gain) {
        for (int sample : {-32768, -30000, -500, 0, 500, 30000, 32767}) {
            assert(audio_mix::music_sample(sample, gain, false) == sample * gain / 100);
            assert(audio_mix::music_sample(sample, gain, true) == sample * gain / 200);
        }
    }
    // Quiet player volumes still retain music underneath an announcement.
    for (int gain = 1; gain <= 5; ++gain) {
        assert(audio_mix::music_sample(30000, gain, true) > 0);
        assert(audio_mix::music_sample(-30000, gain, true) < 0);
    }
    assert(audio_mix::music_sample(30000, 5, true) == 750); // A 5% setting becomes 2.5%, not 2%.
    assert(audio_mix::music_sample(30000, 8, true) == 1200);
    assert(audio_mix::music_sample(30000, 8, false) == 2400); // Full volume returns without restarting music.
    assert(audio_mix::music_sample(30000, 0, true) == 0); // Explicit music mute remains respected.
    assert(audio_mix::add(30000, 30000) == 32767);
    assert(audio_mix::add(-30000, -30000) == -32768);
    assert(audio_mix::add(30000, -30000) == 0);
    assert(audio_mix::add(1200, 12000) == 13200);
    std::cout << "Audio mix tests passed\n";
}
