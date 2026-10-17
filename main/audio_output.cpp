#include "audio_output.h"
#include "assist_client.h"
#include <algorithm>
#include <cmath>
#include "freertos/task.h"
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char* TAG = "audio";
static_assert(CONFIG_JOHAN2_I2S_BCLK != CONFIG_JOHAN2_I2S_LRCK &&
              CONFIG_JOHAN2_I2S_BCLK != CONFIG_JOHAN2_I2S_DOUT &&
              CONFIG_JOHAN2_I2S_LRCK != CONFIG_JOHAN2_I2S_DOUT, "I2S pins must differ");

void AudioOutput::init(sendspin::PlayerRole& player, AssistClient* assist) {
    assist_ = assist;
    player_ = &player;
#if CONFIG_JOHAN2_LED_GPIO >= 0
    // WS2812 at 10 MHz ticks: bit0 = 0.3/0.9 us, bit1 = 0.9/0.3 us.
    rmt_tx_channel_config_t tx{};
    tx.gpio_num = static_cast<gpio_num_t>(CONFIG_JOHAN2_LED_GPIO);
    tx.clk_src = RMT_CLK_SRC_DEFAULT;
    tx.resolution_hz = 10 * 1000 * 1000;
    tx.mem_block_symbols = 64;
    tx.trans_queue_depth = 1;
    rmt_bytes_encoder_config_t enc{};
    enc.bit0 = {3, 1, 9, 0};
    enc.bit1 = {9, 1, 3, 0};
    enc.flags.msb_first = 1;
    if (rmt_new_tx_channel(&tx, &led_chan_) == ESP_OK && rmt_new_bytes_encoder(&enc, &led_enc_) == ESP_OK &&
        rmt_enable(led_chan_) == ESP_OK) {
        set_led(0, 0, 0);
    } else {
        led_enc_ = nullptr;
        ESP_LOGW(TAG, "LED init failed");
    }
#endif
    // Separate I2S controller: microphone clocks continue while playback is idle.
    i2s_chan_config_t mic_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&mic_channel, nullptr, &mic_rx_));
    i2s_std_config_t mic{};
    mic.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    mic.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    mic.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // INMP441 L/R tied to GND.
    mic.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    mic.gpio_cfg.bclk = GPIO_NUM_15;
    mic.gpio_cfg.ws = GPIO_NUM_16;
    mic.gpio_cfg.dout = I2S_GPIO_UNUSED;
    mic.gpio_cfg.din = GPIO_NUM_17;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_rx_, &mic));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_rx_));
    ESP_ERROR_CHECK(xTaskCreate(microphone_task, "mic_level", 4096, this, 4, nullptr) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    volume_.store(CONFIG_JOHAN2_INITIAL_VOLUME);
    tx_lock_ = xSemaphoreCreateMutex();
    tts_buf_ = xStreamBufferCreate(TTS_BUFFER_BYTES, 1);
    ESP_ERROR_CHECK(tx_lock_ && tts_buf_ ? ESP_OK : ESP_ERR_NO_MEM);
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
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

void AudioOutput::set_led(uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t cfg{};
    if (rmt_transmit(led_chan_, led_enc_, grb, sizeof(grb), &cfg) == ESP_OK) {
        // RMT consumes the buffer asynchronously; retain it until transmission finishes.
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan_, 100));
    }
}

void AudioOutput::update_led() {
    if (!led_enc_) return;
    const int64_t now = esp_timer_get_time();
    if (now < led_next_us_) return;
    led_next_us_ = now + 25000;
    const uint8_t mic_peak = peak_.exchange(0);
    led_level_ = std::max<int>(mic_peak, led_level_ > 4 ? led_level_ - std::max(1, led_level_ / 5) : 0);
    const uint8_t source_peak = music_peak_.exchange(0);
    const uint8_t peak = active_.load() ? source_peak : 0;
    uint8_t level = music_level_ > 12 ? music_level_ - music_level_ / 4 : 0;
    if (peak > level + 40) led_hue_ = (led_hue_ + 85) % 768;
    else led_hue_ = (led_hue_ + 1) % 768;
    music_level_ = std::max(level, peak);
    if (led_level_) {
        const uint8_t white = led_level_ * LED_MAX_BRIGHTNESS / 255;
        set_led(white, white, white);
        return;
    }
    const int v = music_level_ * LED_MAX_BRIGHTNESS / 255;
    const int step = led_hue_ % 256;
    switch (led_hue_ / 256) {
        case 0: set_led(v * (255 - step) / 255, v * step / 255, 0); break;
        case 1: set_led(0, v * (255 - step) / 255, v * step / 255); break;
        default: set_led(v * step / 255, 0, v * (255 - step) / 255); break;
    }
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

void AudioOutput::tx_enable_locked() {
    if (tx_enabled_) return;
    // Overwrite every DMA descriptor before enabling, so the previous audio cannot replay.
    static const uint8_t silence[DMA_DESCRIPTORS * DMA_FRAMES * CHANNELS * sizeof(int16_t)]{};
    size_t loaded = 0;
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx_, silence, sizeof(silence), &loaded));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_));
    tx_enabled_ = true;
}

bool AudioOutput::tts_begin() {
    xStreamBufferReset(tts_buf_);
    tts_running_.store(true);
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    tx_enable_locked();
    xSemaphoreGive(tx_lock_);
    return true;
}

void AudioOutput::tts_write(const int16_t* frames, size_t frame_count) {
    const int gain = CONFIG_JOHAN2_TTS_VOLUME;
    size_t done = 0;
    while (done < frame_count) {
        const size_t n = std::min(frame_count - done, sizeof(tts_scratch_) / (CHANNELS * sizeof(int16_t)));
        for (size_t i = 0; i < n * CHANNELS; ++i)
            tts_scratch_[i] = static_cast<int16_t>(static_cast<int32_t>(frames[done * CHANNELS + i]) * gain / 100);
        const size_t bytes = n * CHANNELS * sizeof(int16_t);
        xSemaphoreTake(tx_lock_, portMAX_DELAY);
        if (active_.load()) {
            // Music is streaming: its writer mixes this buffer in.
            xSemaphoreGive(tx_lock_);
            size_t sent_bytes = 0;
            while (sent_bytes < bytes && active_.load()) {
                sent_bytes += xStreamBufferSend(tts_buf_, reinterpret_cast<uint8_t*>(tts_scratch_) + sent_bytes,
                                                bytes - sent_bytes, pdMS_TO_TICKS(100));
            }
        } else {
            size_t written = 0;
            i2s_channel_write(tx_, tts_scratch_, bytes, &written, 1000);
            xSemaphoreGive(tx_lock_);
        }
        done += n;
    }
}

void AudioOutput::tts_end() {
    static const int16_t silence[DMA_FRAMES * CHANNELS]{};
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    if (!active_.load()) {
        // Flush the DMA queue so the tail of the reply is not cut off.
        for (size_t i = 0; i < DMA_DESCRIPTORS + 1; ++i) {
            size_t written = 0;
            i2s_channel_write(tx_, silence, sizeof(silence), &written, 200);
        }
    }
    tts_running_.store(false);
    if (!active_.load() && tx_enabled_) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_));
        tx_enabled_ = false;
    }
    xSemaphoreGive(tx_lock_);
    xStreamBufferReset(tts_buf_);
}

void AudioOutput::on_stream_start() {
    // Only 48 kHz / stereo / 16-bit formats are advertised, so no clock changes.
    portENTER_CRITICAL(&progress_lock_);
    frames_ = 0;
    portEXIT_CRITICAL(&progress_lock_);
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    tx_enable_locked();
    active_.store(true);
    xSemaphoreGive(tx_lock_);
    ESP_LOGI(TAG, "Stream started: 48 kHz, stereo, 16-bit");
}

void AudioOutput::on_stream_end() {
    // The library waits for its writer to become idle before this callback.
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    active_.store(false);
    if (!tts_running_.load() && tx_enabled_) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_));
        tx_enabled_ = false;
    }
    xSemaphoreGive(tx_lock_);
    ESP_LOGI(TAG, "Stream stopped");
}

size_t AudioOutput::on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (!active_.load()) return 0;
    const size_t bytes = std::min(length, sizeof(scratch_)) & ~size_t{3};
    int gain = muted_.load() ? 0 : volume_.load();
    const bool tts = tts_running_.load();
    if (tts || (assist_ && assist_->command_active())) gain /= 5;
    for (size_t i = 0; i < bytes / 2; ++i) {
        int16_t sample;
        memcpy(&sample, data + i * 2, sizeof(sample));
        scratch_[i] = static_cast<int16_t>(static_cast<int32_t>(sample) * gain / 100);
    }
    if (tts) {
        const size_t got = xStreamBufferReceive(tts_buf_, mix_scratch_, bytes, 0) / 2;
        for (size_t i = 0; i < got; ++i)
            scratch_[i] = static_cast<int16_t>(std::clamp<int32_t>(
                static_cast<int32_t>(scratch_[i]) + mix_scratch_[i], -32768, 32767));
    }
    // Preserve the original source-level music effect, independent of playback volume.
    int music_peak = 0;
    for (size_t i = 0; i < bytes / 2; ++i) {
        int16_t sample;
        memcpy(&sample, data + i * 2, sizeof(sample));
        music_peak = std::max(music_peak, std::abs(static_cast<int>(sample)));
    }
    const auto music_level = static_cast<uint8_t>(std::min(255, music_peak / 96));
    uint8_t previous = music_peak_.load();
    while (music_level > previous && !music_peak_.compare_exchange_weak(previous, music_level)) {}
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(tx_, scratch_, bytes, &written, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "I2S: %s", esp_err_to_name(err));
    return written;
}

void AudioOutput::microphone_task(void* context) {
    auto& self = *static_cast<AudioOutput*>(context);
    int32_t samples[256];
    int16_t pcm[256];
    float dc = 0;
    // Learn ambient noise for about one second; keep quiet during startup.
    double noise = 0;
    size_t calibration_samples = 0;
    unsigned calibration_blocks = 0;
    while (true) {
        size_t bytes = 0;
        const esp_err_t err = i2s_channel_read(self.mic_rx_, samples, sizeof(samples), &bytes, 100);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            self.peak_.store(0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const size_t count = bytes / sizeof(samples[0]);
        if (!count) continue;
        for (size_t i = 0; i < count; ++i) {
            const float sample = samples[i] / 65536.0f;
            dc += (sample - dc) / 256.0f;
            pcm[i] = static_cast<int16_t>(std::clamp((sample - dc) * CONFIG_JOHAN2_MIC_GAIN,
                                                   -32768.0f, 32767.0f));
        }
        if (self.assist_) self.assist_->submit(pcm, count);
        // INMP441 supplies signed 24-bit samples in the high bits of each 32-bit slot.
        // Remove DC before measuring RMS, so a static offset cannot light the LED.
        double sum = 0, squares = 0;
        for (size_t i = 0; i < count; ++i) {
            const double sample = samples[i] / 256;
            sum += sample;
            squares += sample * sample;
        }
        const double mean = sum / count;
        const double rms = std::sqrt(std::max(0.0, squares / count - mean * mean));
        if (calibration_samples < 16000) {
            noise += rms;
            ++calibration_blocks;
            calibration_samples += count;
            if (calibration_samples >= 16000) noise /= calibration_blocks;
            continue;
        }
        const double threshold = std::max(500.0, noise * 3.0);
        // Follow slow ambient changes without immediately absorbing speech/claps.
        noise += (rms < threshold ? 0.01 : 0.0001) * (rms - noise);
        const auto level = static_cast<uint8_t>(std::clamp(
            (rms - threshold) * 255.0 / std::max(8000.0, threshold * 2.0), 0.0, 255.0));
        uint8_t previous = self.peak_.load();
        while (level > previous && !self.peak_.compare_exchange_weak(previous, level)) {}
    }
}
