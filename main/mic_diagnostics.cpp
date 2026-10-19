#include "mic_diagnostics.h"
#include "sdkconfig.h"
#if CONFIG_JOHAN2_MIC_DIAGNOSTICS
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr size_t SAMPLE_COUNT = 16000 * 15;
enum State { Idle, Recording, Ready };
std::atomic<State> state{Idle};
enum Mode { Mono, Compare, Aec };
Mode mode = Mono, compare_mode = Compare, aec_mode = Aec;
std::atomic<bool> aec_ready{false};
#if CONFIG_JOHAN2_MIC_AEC
std::atomic<int> requested_delay{CONFIG_JOHAN2_MIC_AEC_DELAY_MS};
#else
std::atomic<int> requested_delay{0};
#endif
std::atomic<uint32_t> aec_counters[5]{};
uint32_t aec_initial[5]{}, aec_final[5]{};
constexpr size_t BUFFER_BYTES = SAMPLE_COUNT * 8;
uint8_t* buffer = nullptr; // Allocated once; never freed while capture can access it.
size_t captured = 0;
unsigned read_errors = 0;
uint32_t max_processing_us = 0;
float minimum_gain = 0, maximum_gain = 0;
int64_t started = 0, last_block = 0, max_interval = 0, elapsed = 0;
void le16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
void le32(uint8_t* p, uint32_t n) { le16(p, n); le16(p + 2, n >> 16); }
esp_err_t record(httpd_req_t* req) {
    // HTTP server invokes handlers serially. Only the capture task publishes Ready.
    if (state.load(std::memory_order_acquire) == Recording) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Capture still running; retry later");
    }
    if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No recording RAM");
    mode = req->user_ctx ? *static_cast<Mode*>(req->user_ctx) : Mono;
    const bool compare = mode == Compare;
    if (mode == Aec && !aec_ready.load()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "AEC unavailable: enable JOHAN2_MIC_AEC and check initialization");
    }
    // The normal download is mono Assist input, suitable for ordinary players.
    // Explicit channel=all preserves the simultaneous four-channel diagnostic.
    int aec_channel = 3;
    static const char* const channel_names[] = {"before", "reference", "clean", "assist"};
    static const char* const channel_files[] = {"johan2-mic-before-aec.wav", "johan2-reference.wav",
                                               "johan2-mic-after-aec.wav", "johan2-assist-input.wav"};
    if (mode == Aec) {
        char query[96]{}, value[24]{};
        if (httpd_req_get_url_query_len(req) >= sizeof(query))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query too long");
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            if (httpd_query_key_value(query, "channel", value, sizeof(value)) == ESP_OK) {
                aec_channel = -2;
                if (!strcmp(value, "all")) aec_channel = -1;
                for (int i = 0; i < 4; ++i) if (!strcmp(value, channel_names[i])) aec_channel = i;
                if (aec_channel == -2)
                    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel: all, before, reference, clean or assist");
            }
            if (httpd_query_key_value(query, "delay_ms", value, sizeof(value)) == ESP_OK) {
                char* end = nullptr;
                const long delay = strtol(value, &end, 10);
                if (!value[0] || *end || delay < 0 || delay > 150)
                    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "delay_ms: integer 0..150");
                if (delay != requested_delay.load()) {
                    requested_delay.store(delay);
                    // Let the capture task reset/adapt its filter before measurement.
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
        }
    }
    for (size_t i = 0; i < 5; ++i) aec_initial[i] = aec_counters[i].load();
    captured = 0;
    read_errors = 0;
    max_processing_us = 0;
    minimum_gain = maximum_gain = 0;
    last_block = max_interval = elapsed = 0;
    started = esp_timer_get_time();
    state.store(Recording, std::memory_order_release);
    while (state.load(std::memory_order_acquire) != Ready) {
        if (esp_timer_get_time() - started > 20000000)
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture stalled; no WAV available");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    double squares = 0;
    int peak = 0;
    size_t clipped = 0;
    for (size_t i = 0; i < captured; ++i) {
        const int sample = compare ? reinterpret_cast<const int32_t*>(buffer)[i * 2 + 1] / 65536
                                   : reinterpret_cast<const int16_t*>(buffer)[mode == Aec ? i * 4 + 3 : i];
        peak = std::max(peak, std::abs(sample));
        squares += double(sample) * sample;
        clipped += sample == -32768 || sample == 32767;
    }
    const double rms = std::sqrt(squares / captured);
#if CONFIG_JOHAN2_MIC_PROFILE_DENOISE
    constexpr int denoise = 1, delay_samples = 512;
#else
    constexpr int denoise = 0, delay_samples = 0;
#endif
    char metrics[768];
    snprintf(metrics, sizeof(metrics),
             "rms_dbfs=%.1f; peak_dbfs=%.1f; clipped=%u; read_errors=%u; elapsed_ms=%lld; max_block_interval_ms=%.1f; gain=%d; highpass_hz=%d; profile_denoise=%d; delay_samples=%d; max_processing_us=%u; agc=0; effective_gain_min=%.3f; effective_gain_max=%.3f; aec=%d; aec_frame_samples=%d; aec_reference_delay_ms=%d; aec_frames=%u; aec_missing_samples=%u; aec_reference_gaps=%u; aec_reference_drops=%u; aec_mic_drops=%u",
             20 * std::log10(std::max(rms, 0.000001) / 32768),
             20 * std::log10(std::max(double(peak), 0.000001) / 32768),
             unsigned(clipped), read_errors, static_cast<long long>(elapsed / 1000),
             max_interval / 1000.0, CONFIG_JOHAN2_MIC_GAIN, CONFIG_JOHAN2_MIC_HIGHPASS_HZ,
             denoise, delay_samples, unsigned(max_processing_us),
             minimum_gain, maximum_gain, int(aec_ready.load()),
#if CONFIG_JOHAN2_MIC_AEC
             512, requested_delay.load(),
#else
             0, 0,
#endif
             unsigned(aec_final[0] - aec_initial[0]),
             unsigned(aec_final[1] - aec_initial[1]),
             unsigned(aec_final[2] - aec_initial[2]),
             unsigned(aec_final[3] - aec_initial[3]),
             unsigned(aec_final[4] - aec_initial[4]));
    const bool aec_mono = mode == Aec && aec_channel >= 0;
    if (aec_mono) {
        // Ready means the producer has stopped writing. Compact in place, keeping
        // the selected channel's original level; no additional RAM or normalization.
        auto* samples = reinterpret_cast<int16_t*>(buffer);
        for (size_t i = 0; i < captured; ++i) samples[i] = samples[i * 4 + aec_channel];
    }
    const size_t frame_bytes = mode == Mono || aec_mono ? 2 : 8;
    const size_t data_bytes = captured * frame_bytes;
    uint8_t header[44]{};
    memcpy(header, "RIFF", 4); le32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8); le32(header + 16, 16);
    le16(header + 20, 1); le16(header + 22, compare ? 2 : (mode == Aec && !aec_mono ? 4 : 1)); le32(header + 24, 16000);
    le32(header + 28, 16000 * frame_bytes); le16(header + 32, frame_bytes); le16(header + 34, compare ? 32 : 16);
    memcpy(header + 36, "data", 4); le32(header + 40, data_bytes);
    httpd_resp_set_type(req, "audio/wav");
    char disposition[96];
    const char* filename = compare ? "johan2-mic-compare.wav" :
                           (mode == Aec ? (aec_mono ? channel_files[aec_channel] : "johan2-mic-aec.wav") : "johan2-mic.wav");
    snprintf(disposition, sizeof(disposition), "attachment; filename=%s", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Mic-Metrics", metrics);
    if (compare) httpd_resp_set_hdr(req, "X-Mic-Channels", "left=raw-i2s-s32; right=assist-s16-scaled-to-s32");
    if (mode == Aec) httpd_resp_set_hdr(req, "X-Mic-Channels", aec_mono ? channel_names[aec_channel] :
                                      "1=mic-before-aec; 2=playback-reference; 3=mic-after-aec; 4=assist-input");
    esp_err_t result = httpd_resp_send_chunk(req, reinterpret_cast<char*>(header), sizeof(header));
    for (size_t offset = 0; result == ESP_OK && offset < data_bytes; offset += 4096)
        result = httpd_resp_send_chunk(req, reinterpret_cast<char*>(buffer) + offset,
                                      std::min(size_t(4096), data_bytes - offset));
    // Erase audio after download (including disconnect); next request starts a fresh capture.
    memset(buffer, 0, BUFFER_BYTES);
    state.store(Idle, std::memory_order_release);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, nullptr, 0);
    return result;
}
}
int mic_diagnostics::aec_delay_ms() { return requested_delay.load(); }
void mic_diagnostics::start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8081;
    config.max_open_sockets = 1;
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE("mic_diag", "Cannot start diagnostics server");
        return;
    }
    httpd_uri_t uri{};
    uri.uri = "/mic.wav";
    uri.method = HTTP_GET;
    uri.handler = record;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    uri.uri = "/mic-compare.wav";
    uri.user_ctx = &compare_mode;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    uri.uri = "/mic-aec.wav";
    uri.user_ctx = &aec_mode;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    ESP_LOGI("mic_diag", "On-demand 15 s microphone recording: HTTP port 8081 /mic.wav");
}
void mic_diagnostics::feed(const int32_t* raw, const int16_t* pcm, size_t count, bool read_error, uint32_t processing_us, float effective_gain, const int16_t* reference, const int16_t* clean) {
    if (state.load(std::memory_order_acquire) != Recording) return;
    if (read_error) ++read_errors;
    max_processing_us = std::max(max_processing_us, processing_us);
    if (!count) return;
    if (!captured) minimum_gain = maximum_gain = effective_gain;
    else {
        minimum_gain = std::min(minimum_gain, effective_gain);
        maximum_gain = std::max(maximum_gain, effective_gain);
    }
    const int64_t now = esp_timer_get_time();
    if (last_block) max_interval = std::max(max_interval, now - last_block);
    last_block = now;
    const size_t take = std::min(count, SAMPLE_COUNT - captured);
    if (mode == Aec) {
        auto* frames = reinterpret_cast<int16_t*>(buffer);
        for (size_t i = 0; i < take; ++i) {
            frames[(captured + i) * 4] = static_cast<int16_t>(raw[i] / 65536);
            frames[(captured + i) * 4 + 1] = reference ? reference[i] : 0;
            frames[(captured + i) * 4 + 2] = clean ? clean[i] : static_cast<int16_t>(raw[i] / 65536);
            frames[(captured + i) * 4 + 3] = pcm[i];
        }
    } else if (mode == Compare) {
        auto* frames = reinterpret_cast<int32_t*>(buffer);
        for (size_t i = 0; i < take; ++i) {
            frames[(captured + i) * 2] = raw[i];
            frames[(captured + i) * 2 + 1] = int32_t(pcm[i]) * 65536;
        }
    } else {
        memcpy(buffer + captured * sizeof(int16_t), pcm, take * sizeof(int16_t));
    }
    captured += take;
    if (captured == SAMPLE_COUNT) {
        for (size_t i = 0; i < 5; ++i) aec_final[i] = aec_counters[i].load();
        elapsed = now - started;
        state.store(Ready, std::memory_order_release);
    }
}
void mic_diagnostics::aec_status(bool ready, uint32_t frames, uint32_t missing, uint32_t gaps,
                                 uint32_t reference_drops, uint32_t microphone_drops) {
    aec_ready.store(ready);
    aec_counters[0].store(frames);
    aec_counters[1].store(missing);
    aec_counters[2].store(gaps);
    aec_counters[3].store(reference_drops);
    aec_counters[4].store(microphone_drops);
}
#else
void mic_diagnostics::start() {}
int mic_diagnostics::aec_delay_ms() {
#if CONFIG_JOHAN2_MIC_AEC
    return CONFIG_JOHAN2_MIC_AEC_DELAY_MS;
#else
    return 0;
#endif
}
void mic_diagnostics::feed(const int32_t*, const int16_t*, size_t, bool, uint32_t, float, const int16_t*, const int16_t*) {}
void mic_diagnostics::aec_status(bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
#endif
