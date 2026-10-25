#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Short, click-free cues: 1200 Hz start / 600 Hz end, 80 ms at native 48 kHz stereo.
// Generated in the playback task, without allocation or a network request.
struct ListeningCuePcm {
    static constexpr size_t kFrames = 3840;
    static constexpr size_t kFadeFrames = 240; // 5 ms attack/release.
    static size_t render(int16_t* stereo, size_t offset, size_t capacity, bool ending = false) {
        static constexpr int16_t wave[40] = {
            0, 626, 1236, 1816, 2351, 2828, 3236, 3564, 3804, 3951,
            4000, 3951, 3804, 3564, 3236, 2828, 2351, 1816, 1236, 626,
            0, -626, -1236, -1816, -2351, -2828, -3236, -3564, -3804, -3951,
            -4000, -3951, -3804, -3564, -3236, -2828, -2351, -1816, -1236, -626
        };
        if (offset >= kFrames) return 0;
        const size_t frames = std::min(capacity, kFrames - offset);
        for (size_t i = 0; i < frames; ++i) {
            const size_t frame = offset + i;
            const size_t envelope = std::min({frame, kFrames - 1 - frame, kFadeFrames});
            const size_t phase = ending ? (frame / 2) % 40 : frame % 40;
            const int16_t sample = int32_t(wave[phase]) * int32_t(envelope) / int32_t(kFadeFrames);
            stereo[2 * i] = stereo[2 * i + 1] = sample;
        }
        return frames;
    }
};

// Assist-task state: wake_word-end and stt-start belong to the same command.
// Retain a playing cue on cancellation until its playback result is consumed.
struct ListeningCueGate {
    bool request(int run_id, int64_t now, int64_t capture_ready_at = 0, bool ending = false) {
        if (ending) {
            if (run_id == last_end_run || (pending && pending_end && pending_run == run_id)) return false;
        } else {
            if (run_id == last_run) return false;
            last_run = run_id;
        }
        pending = true;
        pending_end = ending;
        pending_run = run_id;
        not_before = ending ? now : std::max(now, capture_ready_at);
        wait_until = not_before + 300000; // Bound waiting for a cancelled reply.
        return true;
    }
    void started() {
        pending = false;
        playing = true;
        playing_end = pending_end;
        if (playing_end) last_end_run = pending_run;
    }
    void finished(int64_t now) { playing = false; resume_at = now + 80000; }
    void cancel(bool preserve_end = false) {
        if (!preserve_end || !pending_end) pending = false;
        if (!preserve_end) resume_at = 0;
    }
    bool blocked(int64_t now) const { return pending || playing || now < resume_at; }
    int last_run{-1}, last_end_run{-1}, pending_run{-1};
    bool pending{false}, playing{false};
    bool pending_end{false}, playing_end{false};
    int64_t not_before{0}, wait_until{0}, resume_at{0};
};
