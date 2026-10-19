#include "mic_aec.h"
#if CONFIG_JOHAN2_MIC_AEC
#include <cstring>
#include "esp_log.h"

bool MicAec::init() {
    handle_ = aec_create(16000, 4, 1, AEC_MODE_SR_LOW_COST);
    if (handle_ && aec_get_chunksize(handle_) != kFrame) {
        aec_destroy(handle_);
        handle_ = nullptr;
    }
    if (!handle_) ESP_LOGW("mic_aec", "AEC unavailable; microphone will use bypass");
    else ESP_LOGI("mic_aec", "AEC ready: 16 kHz, 512 samples, reference delay %d ms", delay_ms_);
    return handle_ != nullptr;
}
void MicAec::set_delay_ms(int delay_ms) {
    if (delay_ms < 0 || delay_ms > 150 || delay_ms == delay_ms_) return;
    delay_ms_ = delay_ms;
    filled_ = 0;
    // A new alignment invalidates the learned echo path. Recreate once here in
    // capture-task context, never per frame or in an interrupt.
    if (handle_) aec_destroy(handle_);
    handle_ = nullptr;
    init();
}
bool MicAec::append(const int32_t* raw, size_t count, int64_t end_us, bool discontinuity) {
    if (discontinuity || filled_ + count > kFrame) {
        filled_ = 0;
        frame_reference_ok_ = true;
    }
    if (count > kFrame) return false;
    if (!filled_) frame_reference_ok_ = true;
    for (size_t i = 0; i < count; ++i) {
        const size_t at = filled_ + i;
        raw_[at] = raw[i];
        near_[at] = static_cast<int16_t>(raw[i] / 65536);
        const int64_t time = end_us * 2 - int64_t(count - 1 - i) * 125 - delay_ms_ * 2000;
        if (!reference_.sample_ticks(time, ref_[at])) {
            frame_reference_ok_ = false;
            if (reference_.recent(end_us)) ++missing_;
        }
    }
    filled_ += count;
    if (filled_ != kFrame) return false;
    filled_ = 0;
    if (handle_ && frame_reference_ok_) {
        aec_process(handle_, near_, ref_, clean_);
        ++frames_;
    } else {
        // Lost/stale reference is safer to bypass than to cancel against unrelated audio.
        memcpy(clean_, near_, sizeof(clean_));
    }
    return true;
}
#endif
