#pragma once
#include <cstddef>
#include <cstdint>

namespace mic_diagnostics {
void start();
int aec_delay_ms();
// Called only from the capture task; no networking or allocation here.
void feed(const int32_t* raw, const int16_t* pcm, size_t count, bool read_error, uint32_t processing_us = 0, float effective_gain = 0,
          const int16_t* reference = nullptr, const int16_t* clean = nullptr);
// Capture-task publication; counters are cumulative and recordings report deltas.
void aec_status(bool ready, uint32_t frames, uint32_t missing, uint32_t gaps, uint32_t reference_drops, uint32_t microphone_drops);
}
