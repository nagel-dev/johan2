#include "mic_aec.h"
#include <cassert>
#include <iostream>
#include <memory>

// Stub tests the adapter and alignment, not Espressif's proprietary DSP.
static bool fail_init = false;
static unsigned calls = 0;
aec_handle_t* aec_create(int rate, int length, int channels, aec_mode_t) {
    assert(rate == 16000 && length == 4 && channels == 1);
    static aec_handle_t handle;
    return fail_init ? nullptr : &handle;
}
int aec_get_chunksize(aec_handle_t*) { return 512; }
void aec_destroy(aec_handle_t*) {}
void aec_process(aec_handle_t*, int16_t* near, int16_t* ref, int16_t* out) {
    ++calls;
    for (size_t i = 0; i < 512; ++i) out[i] = near[i] - ref[i];
}
int main() {
    auto aec = std::make_unique<MicAec>();
    assert(aec->init());
    int32_t raw[256];
    int16_t stereo[480];
    for (auto& value : raw) value = 1200 * 65536;
    for (auto& value : stereo) value = 1000;
    assert(!aec->append(raw, 256, 1016000, false));
    assert(aec->append(raw, 256, 1032000, false));
    assert(!calls && aec->clean()[0] == 1200); // No reference: transparent bypass.
    for (int block = 0; block < 32; ++block) aec->push_reference(stereo, 1005000 + block * 5000);
    assert(!aec->append(raw, 256, 1116000, false));
    assert(aec->append(raw, 256, 1132000, false));
    assert(calls == 1 && aec->frames() == 1);
    for (size_t i = 0; i < 512; ++i) {
        assert(aec->raw()[i] == raw[0]);
        assert(aec->reference()[i] >= 999 && aec->reference()[i] <= 1000);
        assert(aec->clean()[i] >= 200 && aec->clean()[i] <= 201);
    }
    // Packet loss discards the half-frame rather than joining unrelated microphone samples.
    assert(!aec->append(raw, 256, 1148000, false));
    assert(!aec->append(raw, 256, 1180000, true));
    assert(aec->append(raw, 256, 1196000, false));
    assert(calls == 1 && aec->missing() > 0); // Incomplete reference: bypass whole frame.
    assert(aec->clean()[0] == 1200);
    assert(!aec->append(raw, 256, 1516000, false));
    assert(aec->append(raw, 256, 1532000, false));
    assert(calls == 1 && aec->reference()[0] == 0); // Old playback is not reused.

    aec->set_delay_ms(25);
    assert(aec->delay_ms() == 25 && aec->ready());
    aec->set_delay_ms(151);
    assert(aec->delay_ms() == 25);
    aec->set_delay_ms(-1);
    assert(aec->delay_ms() == 25);
    fail_init = true;
    auto bypass = std::make_unique<MicAec>();
    assert(!bypass->init());
    assert(!bypass->append(raw, 256, 1016000, false));
    assert(bypass->append(raw, 256, 1032000, false));
    assert(bypass->clean()[0] == 1200);
    std::cout << "Mic AEC adapter tests passed (DSP stub)\n";
}
