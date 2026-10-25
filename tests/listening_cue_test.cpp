#include "listening_cue.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    std::vector<int16_t> pcm(ListeningCuePcm::kFrames * 2 + 2, 12345);
    assert(ListeningCuePcm::render(pcm.data(), 0, ListeningCuePcm::kFrames + 1) == 3840);
    assert(pcm[0] == 0 && pcm[7678] == 0);
    assert(pcm[7680] == 12345 && pcm[7681] == 12345);
    assert(ListeningCuePcm::render(pcm.data(), ListeningCuePcm::kFrames, 1) == 0);
    int peak = 0;
    double fundamental = 0, harmonic = 0, dc = 0, attack = 0, middle = 0;
    for (size_t frame = 0; frame < ListeningCuePcm::kFrames; ++frame) {
        assert(pcm[2 * frame] == pcm[2 * frame + 1]);
        const int sample = pcm[2 * frame];
        peak = std::max(peak, std::abs(sample));
        dc += sample;
        if (frame < 48) attack += double(sample) * sample;
        if (frame >= 960 && frame < 1008) middle += double(sample) * sample;
        fundamental += sample * std::sin(2 * 3.141592653589793 * 1200 * frame / 48000);
        harmonic += sample * std::sin(2 * 3.141592653589793 * 2400 * frame / 48000);
    }
    assert(peak == 4000 && std::abs(dc / 3840) < 1);
    assert(attack < middle / 20); // Fade-in prevents an abrupt full-volume click.
    assert(fundamental > 100 * std::abs(harmonic));
    std::vector<int16_t> chunks(7680);
    for (size_t offset = 0; offset < 3840;) {
        const size_t got = ListeningCuePcm::render(chunks.data() + 2 * offset, offset, 137);
        assert(got > 0 && got <= 137);
        offset += got;
    }
    assert(std::equal(chunks.begin(), chunks.end(), pcm.begin()));

    ListeningCueGate gate;
    assert(!gate.blocked(0));
    assert(gate.request(7, 1000)); // Wake word.
    assert(!gate.request(7, 2000)); // stt-start of the same run must not beep twice.
    assert(gate.blocked(2000));
    gate.started();
    assert(!gate.pending && gate.playing && gate.blocked(50000));
    gate.finished(100000);
    assert(gate.blocked(179999) && !gate.blocked(180000));
    assert(gate.request(9, 200000)); // Remote start or follow-up has a new run ID.
    gate.cancel();
    assert(!gate.blocked(200001) && !gate.pending);
    assert(gate.request(10, 300000));
    gate.started();
    gate.cancel();
    assert(gate.playing && gate.blocked(300001)); // Wait for the cancelled task's result.
    assert(gate.request(11, 310000)); // A new command can wait behind cancellation.
    gate.finished(320000);
    assert(gate.pending && !gate.playing && gate.blocked(500000));
    gate.started();
    gate.finished(510000);
    assert(!gate.blocked(590000));
    assert(gate.request(12, 600000, 1200000));
    assert(gate.not_before == 1200000 && gate.wait_until == 1500000);
    assert(gate.blocked(1000000)); // Do not signal readiness during the wake-word tail.
    gate.cancel();
    assert(gate.request(12, 1300000, 0, true));
    assert(!gate.request(12, 1300010, 0, true)); // VAD-end and transcript are one capture end.
    gate.cancel(true); // HA run-end must preserve its pending local end cue.
    assert(gate.pending && gate.pending_end);
    gate.started();
    assert(gate.playing_end);
    assert(!gate.request(12, 1300020, 0, true));
    gate.finished(1400000);
    gate.cancel(true);
    assert(gate.blocked(1479999) && !gate.blocked(1480000));
    std::vector<int16_t> low(7680);
    assert(ListeningCuePcm::render(low.data(), 0, 3840, true) == 3840);
    double low_energy = 0, high_energy = 0;
    for (size_t frame = 0; frame < 3840; ++frame) {
        low_energy += low[2 * frame] * std::sin(2 * 3.141592653589793 * 600 * frame / 48000);
        high_energy += low[2 * frame] * std::sin(2 * 3.141592653589793 * 1200 * frame / 48000);
    }
    assert(low_energy > 100 * std::abs(high_energy));
    std::cout << "Listening cue tests passed: PCM timing/frequency/envelope, chunking, deduplication and microphone gate\n";
}
