#include "mic_diagnostics.h"
#include "sdkconfig.h"
#if CONFIG_JOHAN2_MIC_DIAGNOSTICS
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
bool compare = false;
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
    compare = req->user_ctx != nullptr;
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
                                   : reinterpret_cast<const int16_t*>(buffer)[i];
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
#ifdef CONFIG_JOHAN2_MIC_AUTO_GAIN
    constexpr int agc = 1;
#else
    constexpr int agc = 0;
#endif
    char metrics[512];
    snprintf(metrics, sizeof(metrics),
             "rms_dbfs=%.1f; peak_dbfs=%.1f; clipped=%u; read_errors=%u; elapsed_ms=%lld; max_block_interval_ms=%.1f; gain=%d; highpass_hz=%d; profile_denoise=%d; delay_samples=%d; max_processing_us=%u; agc=%d; max_gain=%d; effective_gain_min=%.3f; effective_gain_max=%.3f",
             20 * std::log10(std::max(rms, 0.000001) / 32768),
             20 * std::log10(std::max(double(peak), 0.000001) / 32768),
             unsigned(clipped), read_errors, static_cast<long long>(elapsed / 1000),
             max_interval / 1000.0, CONFIG_JOHAN2_MIC_GAIN, CONFIG_JOHAN2_MIC_HIGHPASS_HZ,
             denoise, delay_samples, unsigned(max_processing_us), agc, CONFIG_JOHAN2_MIC_MAX_GAIN,
             minimum_gain, maximum_gain);
    const size_t frame_bytes = compare ? 8 : 2;
    const size_t data_bytes = captured * frame_bytes;
    uint8_t header[44]{};
    memcpy(header, "RIFF", 4); le32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8); le32(header + 16, 16);
    le16(header + 20, 1); le16(header + 22, compare ? 2 : 1); le32(header + 24, 16000);
    le32(header + 28, 16000 * frame_bytes); le16(header + 32, frame_bytes); le16(header + 34, compare ? 32 : 16);
    memcpy(header + 36, "data", 4); le32(header + 40, data_bytes);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", compare ? "attachment; filename=johan2-mic-compare.wav" : "attachment; filename=johan2-mic.wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Mic-Metrics", metrics);
    if (compare) httpd_resp_set_hdr(req, "X-Mic-Channels", "left=raw-i2s-s32; right=assist-s16-scaled-to-s32");
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
    uri.user_ctx = &state; // Non-null selects simultaneous raw/processed capture.
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    ESP_LOGI("mic_diag", "On-demand 15 s microphone recording: HTTP port 8081 /mic.wav");
}
void mic_diagnostics::feed(const int32_t* raw, const int16_t* pcm, size_t count, bool read_error, uint32_t processing_us, float effective_gain) {
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
    if (compare) {
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
        elapsed = now - started;
        state.store(Ready, std::memory_order_release);
    }
}
#else
void mic_diagnostics::start() {}
void mic_diagnostics::feed(const int32_t*, const int16_t*, size_t, bool, uint32_t, float) {}
#endif
