/* Fixed-storage 16 kHz wake-only speech gate; shared with HomeDash.
 * One owner task feeds <=256 samples, then drains ready frames before feeding
 * again. The detector is injected so buffering can be tested without ESP-IDF. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WAKE_GATE_FRAME 160 /* WebRTC VAD: 10 ms at 16 kHz */
#define WAKE_GATE_PREROLL 50 /* 500 ms, including the triggering frame */
#define WAKE_GATE_TAIL 100 /* 1 s after the last speech frame */
#define WAKE_GATE_CAPACITY (WAKE_GATE_PREROLL + 2)

typedef bool (*wake_gate_detect_t)(void *context, int16_t *frame);
typedef struct {
    int16_t frames[WAKE_GATE_CAPACITY][WAKE_GATE_FRAME];
    int16_t partial[WAKE_GATE_FRAME];
    size_t head, count, ready, filled;
    unsigned tail;
    uint32_t seen_frames, sent_frames;
} wake_audio_gate_t;

static inline void wake_gate_reset(wake_audio_gate_t *gate)
{
    memset(gate, 0, sizeof(*gate));
}

static inline bool wake_gate_feed(wake_audio_gate_t *gate, const int16_t *pcm,
                                  size_t count, wake_gate_detect_t detect, void *context)
{
    for (size_t i = 0; i < count; ++i) {
        gate->partial[gate->filled++] = pcm[i];
        if (gate->filled != WAKE_GATE_FRAME) continue;
        gate->filled = 0;
        const bool speech = detect(context, gate->partial);
        ++gate->seen_frames;
        /* Only discard old silence. Never overwrite audio waiting for transport. */
        if (!gate->ready && !gate->tail && gate->count >= WAKE_GATE_PREROLL) {
            gate->head = (gate->head + 1) % WAKE_GATE_CAPACITY;
            --gate->count;
        }
        if (gate->count == WAKE_GATE_CAPACITY) return false;
        const size_t slot = (gate->head + gate->count++) % WAKE_GATE_CAPACITY;
        memcpy(gate->frames[slot], gate->partial, sizeof(gate->partial));
        if (speech) {
            gate->tail = WAKE_GATE_TAIL;
            gate->ready = gate->count;
        } else if (gate->tail) {
            --gate->tail;
            gate->ready = gate->count;
        }
    }
    return true;
}

static inline size_t wake_gate_take(wake_audio_gate_t *gate, void *pcm, size_t capacity_samples)
{
    if (!gate->ready || capacity_samples < WAKE_GATE_FRAME) return 0;
    memcpy(pcm, gate->frames[gate->head], sizeof(gate->partial));
    gate->head = (gate->head + 1) % WAKE_GATE_CAPACITY;
    --gate->count;
    --gate->ready;
    ++gate->sent_frames;
    return WAKE_GATE_FRAME;
}
