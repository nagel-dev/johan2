#include "tts_player.h"
#include <algorithm>
#include <cstring>
#include "audio_output.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "tts";
static constexpr uint32_t OUTPUT_RATE = 48000;

static uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

void TtsPlayer::init(AudioOutput& audio) {
    audio_ = &audio;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    ESP_ERROR_CHECK(xTaskCreate(task, "tts_play", 12288, this, 3, &task_) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

bool TtsPlayer::start(const std::string& url) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return false;
    url_ = url;
    result_.store(None);
    cancel_.store(false);
    xTaskNotifyGive(task_);
    return true;
}

void TtsPlayer::task(void* context) {
    auto& self = *static_cast<TtsPlayer*>(context);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const Result result = self.play();
        self.result_.store(result);
        self.busy_.store(false);
    }
}

void TtsPlayer::flush() {
    if (out_len_) audio_->tts_write(out_, out_len_ / 2);
    out_len_ = 0;
}

void TtsPlayer::emit(int32_t left, int32_t right) {
    out_[out_len_++] = static_cast<int16_t>(left);
    out_[out_len_++] = static_cast<int16_t>(right);
    if (out_len_ >= sizeof(out_) / sizeof(out_[0])) flush();
}

// Linear-interpolating resampler from the WAV rate to 48 kHz stereo.
void TtsPlayer::feed_samples(const uint8_t* data, size_t length) {
    const size_t frame_bytes = channels_ * 2;
    while (length) {
        const uint8_t* frame = data;
        if (carry_len_ || length < frame_bytes) {
            const size_t take = std::min(frame_bytes - carry_len_, length);
            memcpy(carry_ + carry_len_, data, take);
            carry_len_ += take;
            data += take;
            length -= take;
            if (carry_len_ < frame_bytes) return;
            frame = carry_;
            carry_len_ = 0;
        } else {
            data += frame_bytes;
            length -= frame_bytes;
        }
        const int32_t left = static_cast<int16_t>(le16(frame));
        const int32_t right = channels_ == 2 ? static_cast<int16_t>(le16(frame + 2)) : left;
        if (!have_prev_) {
            prev_l_ = left;
            prev_r_ = right;
            have_prev_ = true;
        }
        while (frac_ < 65536) {
            emit(prev_l_ + static_cast<int32_t>((static_cast<int64_t>(left - prev_l_) * frac_) >> 16),
                 prev_r_ + static_cast<int32_t>((static_cast<int64_t>(right - prev_r_) * frac_) >> 16));
            frac_ += step_;
        }
        frac_ -= 65536;
        prev_l_ = left;
        prev_r_ = right;
    }
}

// Feeds compressed bytes to the decoder and plays whatever PCM it produces.
bool TtsPlayer::decode(uint8_t* data, size_t length) {
    esp_audio_simple_dec_raw_t raw{};
    raw.buffer = data;
    raw.len = length;
    while (raw.len) {
        esp_audio_simple_dec_out_t out{};
        out.buffer = decoded_;
        out.len = decoded_capacity_;
        const auto err = esp_audio_simple_dec_process(decoder_, &raw, &out);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            auto* bigger = static_cast<uint8_t*>(realloc(decoded_, out.needed_size));
            if (!bigger) return false;
            decoded_ = bigger;
            decoded_capacity_ = out.needed_size;
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) return false;
        if (out.decoded_size) {
            if (!info_ready_) {
                esp_audio_simple_dec_info_t info{};
                if (esp_audio_simple_dec_get_info(decoder_, &info) != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16 ||
                    info.channel < 1 || info.channel > 2 || info.sample_rate < 8000 || info.sample_rate > OUTPUT_RATE)
                    return false;
                channels_ = info.channel;
                rate_ = info.sample_rate;
                step_ = static_cast<uint32_t>((static_cast<uint64_t>(rate_) << 16) / OUTPUT_RATE);
                info_ready_ = true;
                started_ = true;
                audio_->tts_begin();
            }
            feed_samples(decoded_, out.decoded_size);
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (!raw.consumed && !out.decoded_size) break;
    }
    return true;
}

TtsPlayer::Result TtsPlayer::play() {
    const int64_t begin = esp_timer_get_time();
    info_ready_ = have_prev_ = started_ = false;
    rate_ = channels_ = 0;
    carry_len_ = out_len_ = 0;
    frac_ = 0;
    decoded_capacity_ = 8192;
    decoded_ = static_cast<uint8_t*>(malloc(decoded_capacity_));
    if (!decoded_) return Failed;
    esp_http_client_config_t config{};
    config.url = url_.c_str();
    config.timeout_ms = 30000; // HA synthesizes on demand; slow TTS must not fall back to the player
    config.buffer_size = 2048;
    auto client = esp_http_client_init(&config);
    if (!client) return Failed;
    Result result = Failed;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "Reply fetch returned HTTP %d", status);
        } else {
            static uint8_t buffer[2048];
            bool ok = true;
            bool cancelled = false;
            size_t sniffed = 0;
            while (true) {
                const int n = esp_http_client_read(client, reinterpret_cast<char*>(buffer + sniffed),
                                                   sizeof(buffer) - sniffed);
                if (cancel_.load()) { cancelled = true; break; }
                if (n < 0) { ok = false; break; }
                if (n == 0) {
                    ok = esp_http_client_is_complete_data_received(client);
                    break;
                }
                sniffed += n;
                if (!decoder_) {
                    if (sniffed < 4) continue;
                    // HA's Assist pipeline usually returns MP3; Piper/Wyoming may return WAV.
                    esp_audio_simple_dec_cfg_t cfg{};
                    if (!memcmp(buffer, "RIFF", 4)) cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
                    else if (!memcmp(buffer, "ID3", 3) || (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0))
                        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
                    else {
                        ESP_LOGW(TAG, "Unknown reply format %02x %02x %02x %02x", buffer[0], buffer[1], buffer[2], buffer[3]);
                        ok = false;
                        break;
                    }
                    if (esp_audio_simple_dec_open(&cfg, &decoder_) != ESP_AUDIO_ERR_OK) { ok = false; break; }
                }
                if (!decode(buffer, sniffed)) { ok = false; break; }
                sniffed = 0;
            }
            if (cancelled) {
                result = Ok;
            } else if (ok && started_) {
                flush();
                result = Ok;
            } else if (!started_) {
                ESP_LOGW(TAG, "Reply could not be decoded (need 16-bit MP3/WAV at 8-48 kHz); use the reply player fallback");
                result = Unsupported;
            } else {
                flush();
            }
        }
    } else {
        ESP_LOGW(TAG, "Reply fetch failed to connect");
    }
    if (started_) audio_->tts_end();
    cancel_.store(false);
    if (decoder_) esp_audio_simple_dec_close(decoder_);
    decoder_ = nullptr;
    free(decoded_);
    decoded_ = nullptr;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Reply playback %s after %lld ms (%u Hz, %u ch)", result == Ok ? "finished" : "ended early",
             static_cast<long long>((esp_timer_get_time() - begin) / 1000), static_cast<unsigned>(rate_),
             static_cast<unsigned>(channels_));
    // Failed before any audio: nothing was spoken, so a fallback is safe.
    if (result == Failed && !started_) result = Unsupported;
    return result;
}
