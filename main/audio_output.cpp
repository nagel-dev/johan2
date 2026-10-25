#include "sdkconfig.h"
#include "audio_output.h"
#include "audio_mix.h"
#include "runtime_metrics.h"
#include "assist_client.h"
#include "mic_diagnostics.h"
#include "mic_pcm.h"
#include "mic_aec.h"
#if CONFIG_JOHAN2_MIC_PROFILE_DENOISE
#include "mic_denoise.h"
#endif
#include <algorithm>
#include <cmath>
#include <new>
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include <string>
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char* TAG = "audio";
static_assert(CONFIG_JOHAN2_I2S_BCLK != CONFIG_JOHAN2_I2S_LRCK &&
              CONFIG_JOHAN2_I2S_BCLK != CONFIG_JOHAN2_I2S_DOUT &&
              CONFIG_JOHAN2_I2S_LRCK != CONFIG_JOHAN2_I2S_DOUT, "I2S pins must differ");

void AudioOutput::init(sendspin::PlayerRole& player, AssistClient* assist) {
    assist_ = assist;
    player_ = &player;
    ESP_ERROR_CHECK(nvs_open("sendspin", NVS_READWRITE, &volume_nvs_));
    uint8_t saved_volume = 0;
    const esp_err_t volume_err = nvs_get_u8(volume_nvs_, "volume", &saved_volume);
    if (volume_err == ESP_OK) {
        if (saved_volume <= 100) volume_.store(saved_volume);
        else ESP_LOGW(TAG, "Saved player volume is invalid; starting muted");
    } else if (volume_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(volume_err);
    }
    volume_save_timer_ = xTimerCreate("volume_save", VOLUME_SAVE_DELAY, pdFALSE, this,
                                      save_volume_timer_cb);
    ESP_ERROR_CHECK(volume_save_timer_ ? ESP_OK : ESP_ERR_NO_MEM);
    player.update_volume(volume_.load());
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
#if CONFIG_JOHAN2_HOMEDASH_LIGHT_PORT > 0
        if (CONFIG_JOHAN2_HOMEDASH_TRANSCRIPT_URL[0])
            xTaskCreate(led_mirror_task, "led_mirror", 4096, this, 2, nullptr);
#endif
    } else {
        led_enc_ = nullptr;
        ESP_LOGW(TAG, "LED init failed");
    }
#endif
    // Separate I2S controller: microphone clocks continue while playback is idle.
    i2s_chan_config_t mic_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
#if CONFIG_JOHAN2_MIC_AEC
    static_assert(DMA_FRAMES == PlaybackReference::kFrames, "AEC reference must match TX DMA frames");
    mic_channel.dma_frame_num = 256;
    reference_queue_ = xQueueCreate(24, sizeof(ReferenceBlock));
    microphone_queue_ = xQueueCreate(6, sizeof(MicrophoneBlock));
    ESP_ERROR_CHECK(reference_queue_ && microphone_queue_ ? ESP_OK : ESP_ERR_NO_MEM);
#endif
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
#if CONFIG_JOHAN2_MIC_AEC
    i2s_event_callbacks_t mic_callbacks{};
    mic_callbacks.on_recv = received;
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(mic_rx_, &mic_callbacks, this));
#endif
    tx_lock_ = xSemaphoreCreateMutex();
    // TTS is copied through task-local I2S scratch buffers, never used directly
    // by DMA/ISR. Keep its 32 KB payload out of scarce internal/DMA RAM.
    auto* tts_storage = static_cast<uint8_t*>(heap_caps_malloc(TTS_BUFFER_BYTES + 1,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tts_storage)
        tts_buf_ = xStreamBufferCreateStatic(TTS_BUFFER_BYTES + 1, 1, tts_storage, &tts_control_);
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
    // Reserve DMA/control resources before AEC performs its startup allocations.
    ESP_ERROR_CHECK(i2s_channel_enable(mic_rx_));
#if CONFIG_JOHAN2_MIC_AEC
    constexpr uint32_t microphone_stack = 8192;
#else
    constexpr uint32_t microphone_stack = 4096;
#endif
#if CONFIG_JOHAN2_MIC_AEC && !CONFIG_FREERTOS_UNICORE
    // Keep continuous DSP off the Wi-Fi/network core. FPU tasks otherwise pin
    // themselves to whichever core first executes their floating-point code.
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(microphone_task, "mic_level", microphone_stack, this, 4, nullptr, 1) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
#else
    ESP_ERROR_CHECK(xTaskCreate(microphone_task, "mic_level", microphone_stack, this, 4, nullptr) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}

void AudioOutput::on_volume_changed(uint8_t volume) {
    const uint8_t previous = volume_.exchange(volume);
    if (previous != volume && xTimerReset(volume_save_timer_, 0) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule player volume persistence");
}

void AudioOutput::save_volume_timer_cb(TimerHandle_t timer) {
    auto* self = static_cast<AudioOutput*>(pvTimerGetTimerID(timer));
    const uint8_t volume = self->volume_.load();
    esp_err_t err = nvs_set_u8(self->volume_nvs_, "volume", volume);
    if (err == ESP_OK) err = nvs_commit(self->volume_nvs_);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to persist player volume: %s", esp_err_to_name(err));
}

bool AudioOutput::sent(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    auto& self = *static_cast<AudioOutput*>(context);
    // ISR records progress and an optional bounded reference copy. Sendspin stays in task context.
    portENTER_CRITICAL_ISR(&self.progress_lock_);
    self.frames_ += event->size / 4; // stereo x 16-bit
    self.timestamp_ = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&self.progress_lock_);
#if CONFIG_JOHAN2_MIC_AEC
    BaseType_t woken = pdFALSE;
    if (event->dma_buf && event->size == sizeof(self.reference_isr_.stereo)) {
        self.reference_isr_.end_us = self.timestamp_;
        memcpy(self.reference_isr_.stereo, event->dma_buf, sizeof(self.reference_isr_.stereo));
        if (xQueueSendFromISR(self.reference_queue_, &self.reference_isr_, &woken) != pdTRUE)
            self.reference_drops_.fetch_add(1);
    } else {
        self.reference_drops_.fetch_add(1);
    }
    return woken == pdTRUE;
#else
    return false;
#endif
}

#if CONFIG_JOHAN2_MIC_AEC
bool AudioOutput::received(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    auto& self = *static_cast<AudioOutput*>(context);
    // One bounded DMA copy. No DSP, allocation, logging, or network calls in ISR.
    ++self.microphone_isr_.sequence;
    self.microphone_isr_.end_us = esp_timer_get_time();
    if (!event->dma_buf || event->size != sizeof(self.microphone_isr_.raw)) {
        self.microphone_drops_.fetch_add(1);
        return false;
    }
    memcpy(self.microphone_isr_.raw, event->dma_buf, sizeof(self.microphone_isr_.raw));
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(self.microphone_queue_, &self.microphone_isr_, &woken) != pdTRUE)
        self.microphone_drops_.fetch_add(1);
    return woken == pdTRUE;
}
#endif

void AudioOutput::set_led(uint8_t r, uint8_t g, uint8_t b) {
    // WS2812 retains its color: avoid periodic switching for unchanged values,
    // especially repeated black frames while listening in silence.
    const uint32_t color = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    auto full = [](uint8_t v) { return uint32_t(std::min(255, v * 255 / LED_MAX_BRIGHTNESS)); };
    led_mirror_color_.store((full(r) << 16) | (full(g) << 8) | full(b));
    if (led_color_valid_ && color == led_last_color_) return;
    const uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t cfg{};
    if (rmt_transmit(led_chan_, led_enc_, grb, sizeof(grb), &cfg) == ESP_OK) {
        // RMT consumes the buffer asynchronously; retain it until transmission finishes.
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan_, 100));
        led_last_color_ = color;
        led_color_valid_ = true;
    }
}

// Mirrors the LED colour to the HomeDash proxy over UDP (up to 40 Hz, best effort).
// The task owns DNS and the socket so the LED path never blocks on the network.
void AudioOutput::led_mirror_task(void* context) {
    auto& self = *static_cast<AudioOutput*>(context);
    std::string host = CONFIG_JOHAN2_HOMEDASH_TRANSCRIPT_URL;
    const auto scheme = host.find("://");
    if (scheme != std::string::npos) host.erase(0, scheme + 3);
    host.resize(host.find_first_of(":/") == std::string::npos ? host.size() : host.find_first_of(":/"));
    int sock = -1;
    sockaddr_in dest{};
    uint32_t last = 0xFFFFFFFF;
    int64_t resend_at = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (sock < 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            dest = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
            freeaddrinfo(result);
            dest.sin_port = htons(CONFIG_JOHAN2_HOMEDASH_LIGHT_PORT);
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        }
        const uint32_t color = self.led_mirror_color_.load();
        const int64_t now = esp_timer_get_time();
        // Repeat now and then so a HomeDash that connects later catches up.
        if (color == last && now < resend_at) continue;
        const uint8_t packet[3] = {uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color)};
        sendto(sock, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        last = color;
        resend_at = now + 2000000;
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
    const bool playing = active_.load();
    const uint8_t music_bass = bass_peak_.exchange(0), music_treble = treble_peak_.exchange(0);
    const uint8_t music_mid = mid_peak_.exchange(0);
    const uint8_t mic_bass = mic_bass_peak_.exchange(0), mic_treble = mic_treble_peak_.exchange(0);
    if (playing) {
        const auto color = music_light_.update({source_peak, music_bass, music_mid, music_treble});
        // Mic activity adds a little white without obscuring the music colours.
        auto channel = [this](uint8_t v) {
            const int tinted = v + (255 - v) * led_level_ / (255 * 3);
            return static_cast<uint8_t>(tinted * LED_MAX_BRIGHTNESS / 255);
        };
        set_led(channel(color.r), channel(color.g), channel(color.b));
        return;
    }
    uint8_t level = music_level_ > 12 ? music_level_ - music_level_ / 4 : 0;
    const uint8_t bass_peak = mic_bass;
    const uint8_t treble_peak = mic_treble;
    bass_level_ = std::max<uint8_t>(bass_level_ > 12 ? bass_level_ - bass_level_ / 4 : 0, bass_peak);
    treble_level_ = std::max<uint8_t>(treble_level_ > 12 ? treble_level_ - treble_level_ / 8 : 0, treble_peak);
    // Hold the hue when there is no new audio; treble speeds the glide while bass sets brightness.
    if (bass_peak || treble_peak)
        led_hue_ = (led_hue_ + 1 + treble_level_ / 24) % 768;
    music_level_ = std::max(bass_level_, uint8_t(level / 2));
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
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    xStreamBufferReset(tts_buf_);
    tts_pending_bytes_ = 0;
    tts_running_.store(true);
    tx_enable_locked();
    xSemaphoreGive(tx_lock_);
    return true;
}

bool AudioOutput::music_writer_recent() const {
    const int64_t last = music_write_at_.load();
    return active_.load() && last && esp_timer_get_time() - last < 100000;
}

size_t AudioOutput::load_tts_locked(size_t bytes) {
    const size_t capacity = std::min(bytes, sizeof(mix_scratch_));
    if (tts_pending_bytes_ < capacity)
        tts_pending_bytes_ += xStreamBufferReceive(tts_buf_,
            reinterpret_cast<uint8_t*>(mix_scratch_) + tts_pending_bytes_, capacity - tts_pending_bytes_, 0);
    return std::min(bytes, tts_pending_bytes_);
}

void AudioOutput::consume_tts_locked(size_t bytes) {
    bytes = std::min(bytes, tts_pending_bytes_);
    tts_pending_bytes_ -= bytes;
    if (tts_pending_bytes_)
        memmove(mix_scratch_, reinterpret_cast<uint8_t*>(mix_scratch_) + bytes, tts_pending_bytes_);
}

bool AudioOutput::write_tts_locked(const void* data, size_t bytes, size_t& written) {
    written = 0;
    const esp_err_t err = i2s_channel_write(tx_, data, bytes, &written, 200);
    if (err == ESP_OK && written == bytes) return true;
    runtime_metrics.i2s_errors.fetch_add(1);
    ESP_LOGW(TAG, "Direct speech write failed: %s (%u/%u bytes)", esp_err_to_name(err),
             static_cast<unsigned>(written), static_cast<unsigned>(bytes));
    return false;
}

bool AudioOutput::tts_write(const int16_t* frames, size_t frame_count, const std::atomic<bool>& cancel) {
    const int gain = CONFIG_JOHAN2_TTS_VOLUME;
    size_t done = 0;
    while (done < frame_count && !cancel.load()) {
        const size_t n = std::min(frame_count - done, sizeof(tts_scratch_) / (CHANNELS * sizeof(int16_t)));
        for (size_t i = 0; i < n * CHANNELS; ++i)
            tts_scratch_[i] = static_cast<int16_t>(static_cast<int32_t>(frames[done * CHANNELS + i]) * gain / 100);
        const size_t bytes = n * CHANNELS * sizeof(int16_t);
        size_t sent_bytes = 0;
        while (sent_bytes < bytes && !cancel.load()) {
            if (music_writer_recent()) {
                sent_bytes += xStreamBufferSend(tts_buf_, reinterpret_cast<uint8_t*>(tts_scratch_) + sent_bytes,
                                                bytes - sent_bytes, pdMS_TO_TICKS(100));
                continue;
            }
            xSemaphoreTake(tx_lock_, portMAX_DELAY);
            if (music_writer_recent()) {
                xSemaphoreGive(tx_lock_);
                continue;
            } else {
                // Stream-start does not guarantee PCM delivery. Drain older speech first.
                const size_t pending = load_tts_locked(sizeof(mix_scratch_));
                size_t written = 0;
                const bool ok = pending ? write_tts_locked(mix_scratch_, pending, written) :
                    write_tts_locked(reinterpret_cast<uint8_t*>(tts_scratch_) + sent_bytes, bytes - sent_bytes, written);
                if (pending) consume_tts_locked(written);
                else sent_bytes += written;
                xSemaphoreGive(tx_lock_);
                if (!ok) return false;
            }
        }
        done += n;
    }
    return !cancel.load();
}

bool AudioOutput::tts_drain(uint32_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (true) {
        xSemaphoreTake(tx_lock_, portMAX_DELAY);
        const size_t bytes = load_tts_locked(sizeof(mix_scratch_));
        if (!bytes) {
            xSemaphoreGive(tx_lock_);
            break;
        }
        if (esp_timer_get_time() >= deadline) {
            xSemaphoreGive(tx_lock_);
            return false;
        }
        if (music_writer_recent()) {
            xSemaphoreGive(tx_lock_);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t written = 0;
        const bool ok = write_tts_locked(mix_scratch_, bytes, written);
        consume_tts_locked(written);
        xSemaphoreGive(tx_lock_);
        if (!ok) return false;
    }
    // Samples already copied to DMA still need up to one hardware queue to play.
    vTaskDelay(pdMS_TO_TICKS(35));
    return true;
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
    xStreamBufferReset(tts_buf_);
    tts_pending_bytes_ = 0;
    xSemaphoreGive(tx_lock_);
}

void AudioOutput::on_stream_start() {
    // Only 48 kHz / stereo / 16-bit formats are advertised, so no clock changes.
    portENTER_CRITICAL(&progress_lock_);
    frames_ = 0;
    portEXIT_CRITICAL(&progress_lock_);
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    music_write_at_.store(0);
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
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    if (!active_.load()) {
        xSemaphoreGive(tx_lock_);
        return 0;
    }
    const size_t bytes = std::min(length, sizeof(scratch_)) & ~size_t{3};
    int gain = muted_.load() ? 0 : volume_.load();
    const bool tts = tts_running_.load();
    if (assist_ && assist_->command_active()) gain = 0;
    for (size_t i = 0; i < bytes / 2; ++i) {
        int16_t sample;
        memcpy(&sample, data + i * 2, sizeof(sample));
        scratch_[i] = audio_mix::music_sample(sample, gain, tts);
    }
    if (tts) {
        const size_t got = load_tts_locked(bytes) / 2;
        for (size_t i = 0; i < got; ++i)
            scratch_[i] = audio_mix::add(scratch_[i], mix_scratch_[i]);
    }
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(tx_, scratch_, bytes, &written, timeout_ms);
    if (tts) consume_tts_locked(written);
    if (written) {
        const int64_t now = esp_timer_get_time();
        const int64_t previous = music_write_at_.exchange(now);
        if (is_pcm_delivery_gap(previous, now)) runtime_metrics.pcm_gaps.fetch_add(1);
    }
    if (err != ESP_OK) runtime_metrics.i2s_errors.fetch_add(1);
    xSemaphoreGive(tx_lock_);
    // Analyze only accepted source frames, once, including partial-write retries.
    // Volume/mute still do not change the music display. One stereo pass collects
    // all four levels instead of separate peak and left-channel band passes.
    const auto levels = music_bands_.analyze(data, written);
    const auto bass_level = levels.bass;
    const auto treble_level = levels.treble;
    uint8_t bass_prev = bass_peak_.load();
    while (bass_level > bass_prev && !bass_peak_.compare_exchange_weak(bass_prev, bass_level)) {}
    uint8_t treble_prev = treble_peak_.load();
    while (treble_level > treble_prev && !treble_peak_.compare_exchange_weak(treble_prev, treble_level)) {}
    uint8_t mid_prev = mid_peak_.load();
    while (levels.mid > mid_prev && !mid_peak_.compare_exchange_weak(mid_prev, levels.mid)) {}
    const auto music_level = levels.level;
    uint8_t previous = music_peak_.load();
    while (music_level > previous && !music_peak_.compare_exchange_weak(previous, music_level)) {}
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "I2S: %s", esp_err_to_name(err));
    return written;
}

void AudioOutput::microphone_task(void* context) {
    auto& self = *static_cast<AudioOutput*>(context);
#if CONFIG_JOHAN2_MIC_AEC
    // Fixed AEC frame/history storage is capture-task-only and can live in PSRAM.
    // Allocate once, with the alignment required by Espressif's DSP input buffers.
    void* aec_storage = heap_caps_aligned_alloc(16, sizeof(MicAec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(aec_storage ? ESP_OK : ESP_ERR_NO_MEM);
    auto& aec = *new (aec_storage) MicAec;
    aec.init();
    static MicrophoneBlock microphone, pending_microphone;
    bool have_pending_microphone = false;
    uint32_t aec_processing_us = 0;
    static ReferenceBlock reference;
    uint32_t previous_sequence = 0;
    static constexpr size_t capture_size = MicAec::kFrame;
#else
    static constexpr size_t capture_size = 256;
#endif
    static int32_t samples[capture_size];
    static int16_t pcm[capture_size];
    MicPcm converter(CONFIG_JOHAN2_MIC_HIGHPASS_HZ);
#if CONFIG_JOHAN2_MIC_PROFILE_DENOISE
    // Fixed storage in BSS: the FFT buffers must not consume the 4 KB task stack.
    static MicDenoise denoiser(true, CONFIG_JOHAN2_MIC_HIGHPASS_HZ);
#endif
    // Learn ambient noise for about one second; keep quiet during startup.
    // ESP32-S3 has a single-precision FPU; per-sample double arithmetic is
    // software-emulated and competes with AEC/decoding on the audio core.
    float noise = 0;
    float led_lp_slow = 0, led_lp_fast = 0;
    size_t calibration_samples = 0;
    unsigned calibration_blocks = 0;
    uint32_t max_block_us = 0, over_budget_blocks = 0;
    int64_t next_stats_us = 0;
    while (true) {
        size_t bytes = 0;
        int64_t processing_started_wait = 0;
#if CONFIG_JOHAN2_MIC_AEC
        // Copy DMA data in the callback so sample timestamps survive task latency.
        // The driver's own RX pointer queue is unused in this mode.
        const esp_err_t err = xQueueReceive(self.microphone_queue_, &microphone, pdMS_TO_TICKS(100)) == pdTRUE
                                 ? ESP_OK : ESP_ERR_TIMEOUT;
        aec.set_delay_ms(mic_diagnostics::aec_delay_ms());
        processing_started_wait = esp_timer_get_time();
        for (unsigned n = 0; n < 24 && xQueueReceive(self.reference_queue_, &reference, 0) == pdTRUE; ++n)
            aec.push_reference(reference.stereo, reference.end_us);
        if (err == ESP_OK) {
            // One DMA block of lookahead lets confirmed TX samples arrive before
            // their matching microphone block is processed (independent IRQ phases).
            bool ready = false;
            if (have_pending_microphone) {
                const bool gap = previous_sequence && pending_microphone.sequence != previous_sequence + 1;
                previous_sequence = pending_microphone.sequence;
                ready = aec.append(pending_microphone.raw, 256, pending_microphone.end_us, gap);
            }
            pending_microphone = microphone;
            have_pending_microphone = true;
            if (!ready) {
                aec_processing_us += esp_timer_get_time() - processing_started_wait;
                continue;
            }
            memcpy(samples, aec.raw(), sizeof(samples));
            bytes = sizeof(samples);
        }
#else
        (void)processing_started_wait;
        const esp_err_t err = i2s_channel_read(self.mic_rx_, samples, sizeof(samples), &bytes, 100);
#endif
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            mic_diagnostics::feed(nullptr, nullptr, 0, true);
            self.peak_.store(0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const size_t count = bytes / sizeof(samples[0]);
        if (!count) {
            mic_diagnostics::feed(nullptr, nullptr, 0, err != ESP_OK);
            continue;
        }
        const int64_t processing_started =
#if CONFIG_JOHAN2_MIC_AEC
            processing_started_wait; // Includes reference resampling/AEC; excludes the queue wait.
#else
            esp_timer_get_time();
#endif
        for (size_t i = 0; i < count; ++i) {
#if CONFIG_JOHAN2_MIC_AEC
            float filtered = converter.filter(int32_t(aec.clean()[i]) * 65536);
#else
            float filtered = converter.filter(samples[i]);
#endif
#if CONFIG_JOHAN2_MIC_PROFILE_DENOISE
            filtered = denoiser.process(filtered);
#endif
            pcm[i] = MicPcm::quantize(filtered, CONFIG_JOHAN2_MIC_GAIN);
        }
        const int64_t dsp_finished = esp_timer_get_time();
        uint32_t processing_us = dsp_finished - processing_started;
#if CONFIG_JOHAN2_MIC_AEC
        processing_us += aec_processing_us;
        aec_processing_us = 0;
        mic_diagnostics::aec_status(aec.ready(), aec.frames(), aec.missing(), aec.gaps(),
                                    self.reference_drops_.load(), self.microphone_drops_.load());
        mic_diagnostics::feed(samples, pcm, count, err != ESP_OK, processing_us, CONFIG_JOHAN2_MIC_GAIN,
                              aec.reference(), aec.clean());
#else
        mic_diagnostics::feed(samples, pcm, count, err != ESP_OK, processing_us, CONFIG_JOHAN2_MIC_GAIN);
#endif
        if (self.assist_) {
            for (size_t offset = 0; offset < count; offset += AssistClient::SAMPLES)
                self.assist_->submit(pcm + offset, std::min(AssistClient::SAMPLES, count - offset));
        }
        // INMP441 supplies signed 24-bit samples in the high bits of each 32-bit slot.
        // Remove DC before measuring RMS, so a static offset cannot light the LED.
        int64_t sum = 0;
        for (size_t i = 0; i < count; ++i) {
            sum += samples[i] / 256;
        }
        const float mean = static_cast<float>(sum) / count;
        // Center first: E[x*x] - E[x]*E[x] loses precision in float when the
        // microphone has a large DC offset and little actual sound.
        float squares = 0;
        for (size_t i = 0; i < count; ++i) {
            const float centered = samples[i] / 256 - mean;
            squares += centered * centered;
        }
        const float rms = std::sqrt(squares / count);
        if (calibration_samples < 16000) {
            noise += rms;
            ++calibration_blocks;
            calibration_samples += count;
            if (calibration_samples >= 16000) noise /= calibration_blocks;
            continue;
        }
        const float threshold = std::max(500.0f, noise * 3.0f);
        // Follow slow ambient changes without immediately absorbing speech/claps.
        noise += (rms < threshold ? 0.01f : 0.0001f) * (rms - noise);
        const auto level = static_cast<uint8_t>(std::clamp(
            (rms - threshold) * 255.0f / std::max(8000.0f, threshold * 2.0f), 0.0f, 255.0f));
        uint8_t previous = self.peak_.load();
        while (level > previous && !self.peak_.compare_exchange_weak(previous, level)) {}
        // Band split for the idle LED: same filters as the music path, gated by the noise threshold.
        // Music supplies its own bands; keep only the white mic-activity meter
        // above while streaming, rather than computing unused microphone bands.
        if (!self.active_.load()) {
            float bass_max = 0, treble_max = 0;
            for (size_t i = 0; i < count; ++i) {
                const float sample = samples[i] / 256 - mean;
                led_lp_slow += (sample - led_lp_slow) / 32.0f;
                led_lp_fast += (sample - led_lp_fast) / 4.0f;
                bass_max = std::max(bass_max, std::abs(led_lp_slow));
                treble_max = std::max(treble_max, std::abs(sample - led_lp_fast));
            }
            const float range = std::max(8000.0f, threshold * 2.0f);
            const auto bass_level = static_cast<uint8_t>(std::clamp((bass_max - threshold / 2) * 255.0f / range, 0.0f, 255.0f));
            const auto treble_level = static_cast<uint8_t>(std::clamp((treble_max * 2 - threshold / 2) * 255.0f / range, 0.0f, 255.0f));
            uint8_t bass_prev = self.mic_bass_peak_.load();
            while (bass_level > bass_prev && !self.mic_bass_peak_.compare_exchange_weak(bass_prev, bass_level)) {}
            uint8_t treble_prev = self.mic_treble_peak_.load();
            while (treble_level > treble_prev && !self.mic_treble_peak_.compare_exchange_weak(treble_prev, treble_level)) {}
        }
        // Existing WAV processing metrics stop before LED analysis. Measure the
        // whole capture iteration too, including both AEC half-frames, so CPU
        // overruns cannot hide behind apparently healthy DSP-only timings.
        const uint32_t block_us = processing_us + (esp_timer_get_time() - dsp_finished);
        max_block_us = std::max(max_block_us, block_us);
        if (block_us > count * 1000000 / 16000) {
            ++over_budget_blocks;
            runtime_metrics.mic_overruns.fetch_add(1);
        }
        const int64_t stats_now = esp_timer_get_time();
        if (stats_now >= next_stats_us) {
            next_stats_us = stats_now + 10000000;
            ESP_LOGI(TAG, "Mic workload: max_block_us=%lu; budget_us=%u; over_budget=%lu",
                     static_cast<unsigned long>(max_block_us), unsigned(count * 1000000 / 16000),
                     static_cast<unsigned long>(over_budget_blocks));
            max_block_us = over_budget_blocks = 0;
        }
    }
}
