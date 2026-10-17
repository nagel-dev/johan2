#pragma once
#include <atomic>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "sendspin/player_role.h"
#include "driver/rmt_tx.h"

class AssistClient;

class AudioOutput : public sendspin::PlayerRoleListener {
public:
    void init(sendspin::PlayerRole& player, AssistClient* assist = nullptr);
    void poll();
    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override { volume_.store(volume); }
    void on_mute_changed(bool mute) override { muted_.store(mute); }
    // Direct reply playback (called from the TTS task): 48 kHz, stereo, 16-bit frames.
    // Mixed over music when Sendspin is streaming; otherwise written straight to I2S.
    bool tts_begin();
    void tts_write(const int16_t* frames, size_t frame_count);
    void tts_end();
private:
    static constexpr size_t TTS_BUFFER_BYTES = 32768;
    void tx_enable_locked();
    SemaphoreHandle_t tx_lock_{nullptr};
    StreamBufferHandle_t tts_buf_{nullptr};
    std::atomic<bool> tts_running_{false};
    bool tx_enabled_{false};
    int16_t tts_scratch_[1024]{}; // TTS task only
    int16_t mix_scratch_[1024]{}; // Sendspin writer only
    // Restore the working 30 ms hardware queue (6 x 240 frames at 48 kHz).
    static constexpr size_t DMA_DESCRIPTORS = 6;
    static constexpr size_t DMA_FRAMES = 240;
    static constexpr size_t CHANNELS = 2;
    static constexpr int LED_MAX_BRIGHTNESS = 90;
    static bool sent(i2s_chan_handle_t, i2s_event_data_t*, void*);
    void update_led();
    void set_led(uint8_t r, uint8_t g, uint8_t b);
    rmt_channel_handle_t led_chan_{nullptr};
    rmt_encoder_handle_t led_enc_{nullptr};
    std::atomic<uint8_t> peak_{0};
    uint8_t led_level_{0};
    std::atomic<uint8_t> music_peak_{0};
    uint8_t music_level_{0};
    uint16_t led_hue_{0};
    static void microphone_task(void* context);
    i2s_chan_handle_t mic_rx_{nullptr};
    int64_t led_next_us_{0};
    AssistClient* assist_{nullptr};
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
