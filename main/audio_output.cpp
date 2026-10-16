#include "audio_output.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char* TAG = "audio";
static_assert(CONFIG_JOHAN2_I2S_BCLK != CONFIG_JOHAN2_I2S_LRCK &&
              CONFIG_JOHAN2_I2S_BCLK != CONFIG_JOHAN2_I2S_DOUT &&
              CONFIG_JOHAN2_I2S_LRCK != CONFIG_JOHAN2_I2S_DOUT, "I2S pins must differ");

void AudioOutput::init(sendspin::PlayerRole& player) {
    player_ = &player;
#if CONFIG_JOHAN2_LED_GPIO >= 0
    led_strip_config_t strip{};
    strip.strip_gpio_num = CONFIG_JOHAN2_LED_GPIO;
    strip.max_leds = 1;
    led_strip_rmt_config_t rmt{};
    rmt.resolution_hz = 10 * 1000 * 1000;
    if (led_strip_new_rmt_device(&strip, &rmt, &led_) == ESP_OK) led_strip_clear(led_);
    else ESP_LOGW(TAG, "LED init failed");
#endif
    volume_.store(CONFIG_JOHAN2_INITIAL_VOLUME);
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_DESCRIPTORS;
    channel.dma_frame_num = DMA_FRAMES;
    channel.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &tx_, nullptr));
    i2s_std_config_t config{};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_JOHAN2_I2S_BCLK);
    config.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_JOHAN2_I2S_LRCK);
    config.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_JOHAN2_I2S_DOUT);
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_, &config));
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = sent;
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx_, &callbacks, this));
}

bool AudioOutput::sent(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    auto& self = *static_cast<AudioOutput*>(context);
    // ISR only records progress. Sendspin uses locks and must run in task context.
    portENTER_CRITICAL_ISR(&self.progress_lock_);
    self.frames_ += event->size / 4; // stereo x 16-bit
    self.timestamp_ = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&self.progress_lock_);
    return false;
}

void AudioOutput::update_led() {
    if (!led_) return;
    const int64_t now = esp_timer_get_time();
    if (now < led_next_us_) return;
    led_next_us_ = now + 25000;
    // Instant attack, fast decay; sqrt-like curve keeps quiet passages visible.
    const uint8_t peak = active_.load() ? peak_.exchange(0) : 0;
    uint8_t level = led_level_ > 12 ? led_level_ - led_level_ / 4 : 0;
    if (peak > level) level = peak;
    if (level == led_level_) return;
    led_level_ = level;
    led_strip_set_pixel(led_, 0, level, level / 2, level);
    led_strip_refresh(led_);
}

void AudioOutput::poll() {
    update_led();
    portENTER_CRITICAL(&progress_lock_);
    const uint32_t frames = frames_;
    const int64_t timestamp = timestamp_;
    frames_ = 0;
    portEXIT_CRITICAL(&progress_lock_);
    if (active_.load() && frames) player_->notify_audio_played(frames, timestamp);
}

void AudioOutput::on_stream_start() {
    // Only 48 kHz / stereo / 16-bit formats are advertised, so no clock changes.
    portENTER_CRITICAL(&progress_lock_);
    frames_ = 0;
    portEXIT_CRITICAL(&progress_lock_);
    // Overwrite every DMA descriptor before enabling, so the previous track cannot replay.
    static const uint8_t silence[DMA_DESCRIPTORS * DMA_FRAMES * CHANNELS * sizeof(int16_t)]{};
    size_t loaded = 0;
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx_, silence, sizeof(silence), &loaded));
    active_.store(true);
    ESP_ERROR_CHECK(i2s_channel_enable(tx_));
    ESP_LOGI(TAG, "Stream started: 48 kHz, stereo, 16-bit");
}

void AudioOutput::on_stream_end() {
    // The library waits for its writer to become idle before this callback.
    active_.store(false);
    ESP_ERROR_CHECK(i2s_channel_disable(tx_));
    ESP_LOGI(TAG, "Stream stopped");
}

size_t AudioOutput::on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (!active_.load()) return 0;
    const size_t bytes = std::min(length, sizeof(scratch_)) & ~size_t{3};
    const int gain = muted_.load() ? 0 : volume_.load();
    for (size_t i = 0; i < bytes / 2; ++i) {
        int16_t sample;
        memcpy(&sample, data + i * 2, sizeof(sample));
        scratch_[i] = static_cast<int16_t>(static_cast<int32_t>(sample) * gain / 100);
    }
    // Level from the source signal, so the LED follows music independent of volume.
    int peak = 0;
    for (size_t i = 0; i < bytes / 2; ++i) {
        int16_t sample;
        memcpy(&sample, data + i * 2, sizeof(sample));
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    const int level = std::min(255, peak / 96);
    if (level > peak_.load()) peak_.store(static_cast<uint8_t>(level));
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(tx_, scratch_, bytes, &written, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "I2S: %s", esp_err_to_name(err));
    return written;
}
