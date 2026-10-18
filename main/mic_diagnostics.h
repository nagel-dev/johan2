#pragma once
#include <cstddef>
#include <cstdint>

namespace mic_diagnostics {
void start();
// Called only from the capture task; no networking or allocation here.
void feed(const int32_t* raw, const int16_t* pcm, size_t count, bool read_error, uint32_t processing_us = 0);
}
