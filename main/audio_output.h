#pragma once
#include <atomic>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "sendspin/player_role.h"
#include "led_strip.h"

class AudioOutput : public sendspin::PlayerRoleListener {
public:
    void init(sendspin::PlayerRole& player);
    void poll();
    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override { volume_.store(volume); }
    void on_mute_changed(bool mute) override { muted_.store(mute); }
private:
    // Restore the working 30 ms hardware queue (6 x 240 frames at 48 kHz).
    static constexpr size_t DMA_DESCRIPTORS = 6;
    static constexpr size_t DMA_FRAMES = 240;
    static constexpr size_t CHANNELS = 2;
    static bool sent(i2s_chan_handle_t, i2s_event_data_t*, void*);
    void update_led();
    led_strip_handle_t led_{nullptr};
    std::atomic<uint8_t> peak_{0};
    uint8_t led_level_{0};
    int64_t led_next_us_{0};
    sendspin::PlayerRole* player_{nullptr};
    i2s_chan_handle_t tx_{nullptr};
    std::atomic<uint8_t> volume_{35};
    std::atomic<bool> muted_{false};
    std::atomic<bool> active_{false};
    portMUX_TYPE progress_lock_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t frames_{0};
    int64_t timestamp_{0};
    // Used only by Sendspin's single audio writer; never modify its retry buffer.
    int16_t scratch_[1024]{};
};
