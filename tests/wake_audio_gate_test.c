#include "wake_audio_gate.h"
#include <assert.h>
#include <stdio.h>

static bool detect(void *context, int16_t *pcm)
{
    (void)context;
    for (size_t i = 1; i < WAKE_GATE_FRAME; ++i) assert(pcm[i] == pcm[0]);
    return (pcm[0] >= 120 && pcm[0] <= 124) || pcm[0] == 401;
}

static void check_stream(size_t chunk)
{
    static wake_audio_gate_t gate;
    wake_gate_reset(&gate);
    int16_t input[256], output[WAKE_GATE_FRAME];
    int expected = 71, emitted = 0;
    const size_t total = 650 * WAKE_GATE_FRAME;
    for (size_t offset = 0; offset < total;) {
        size_t count = total - offset < chunk ? total - offset : chunk;
        for (size_t i = 0; i < count; ++i) input[i] = (offset + i) / WAKE_GATE_FRAME + 1;
        assert(wake_gate_feed(&gate, input, count, detect, NULL));
        offset += count;
        while (wake_gate_take(&gate, output, WAKE_GATE_FRAME)) {
            if (expected == 225) expected = 352;
            assert(output[0] == expected++);
            for (size_t i = 1; i < WAKE_GATE_FRAME; ++i) assert(output[i] == output[0]);
            ++emitted;
        }
    }
    assert(emitted == 304 && expected == 502);
    assert(gate.seen_frames == 650 && gate.sent_frames == 304);
    assert(gate.count == WAKE_GATE_PREROLL && gate.ready == 0);
    /* Partial frames and history cannot leak into a new session/owner. */
    assert(wake_gate_feed(&gate, input, 17, detect, NULL));
    wake_gate_reset(&gate);
    assert(!wake_gate_take(&gate, output, WAKE_GATE_FRAME));
    for (size_t i = 0; i < WAKE_GATE_FRAME; ++i) input[i] = 401;
    assert(wake_gate_feed(&gate, input, WAKE_GATE_FRAME, detect, NULL));
    assert(!wake_gate_take(&gate, output, WAKE_GATE_FRAME - 1));
    assert(wake_gate_take(&gate, output, WAKE_GATE_FRAME) == WAKE_GATE_FRAME);
    assert(output[0] == 401 && !wake_gate_take(&gate, output, WAKE_GATE_FRAME));
}

int main(void)
{
    check_stream(256); /* Johan2 DMA chunks */
    check_stream(128); /* HomeDash microphone chunks */
    check_stream(1);   /* arbitrarily fragmented input */
    check_stream(160);
    puts("Wake gate: exact pre-roll/tail, ordered/no-duplicate audio, silence suppression, reset and chunking passed");
}
