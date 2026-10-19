#pragma once
#include <cstdint>
struct aec_handle_t {};
enum aec_mode_t { AEC_MODE_SR_LOW_COST };
aec_handle_t* aec_create(int, int, int, aec_mode_t);
int aec_get_chunksize(aec_handle_t*);
void aec_process(aec_handle_t*, int16_t*, int16_t*, int16_t*);
void aec_destroy(aec_handle_t*);
