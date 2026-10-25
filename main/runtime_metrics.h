#pragma once
#include <atomic>
#include <cstdint>

// Task-context diagnostics only. No networking, allocation or ISR instrumentation.
struct RuntimeSnapshot {
    uint32_t boot_id = 0, uptime_s = 0;
    uint32_t internal_heap_bytes = 0, min_internal_heap_bytes = 0;
    int32_t main_loop_age_ms = -1, pcm_age_ms = -1;
    uint32_t pcm_gaps = 0, i2s_errors = 0, assist_restarts = 0;
    uint32_t mic_queue_drops = 0, mic_overruns = 0, wifi_disconnects = 0;
    bool sendspin_connected = false, stream_active = false;

    const char* status() const {
        if (main_loop_age_ms >= 2000) return "main_loop_stalled";
        if (!sendspin_connected) return "disconnected";
        if (!stream_active) return "idle";
        if (pcm_age_ms < 0) return "awaiting_pcm";
        return pcm_age_ms >= 1000 ? "pcm_stalled" : "streaming";
    }
};

inline int32_t metric_age_ms(int64_t now, int64_t then) {
    if (!then || now < then) return -1;
    const int64_t age = (now - then) / 1000;
    return static_cast<int32_t>(age > INT32_MAX ? INT32_MAX : age);
}

inline bool is_pcm_delivery_gap(int64_t previous, int64_t now) {
    return previous > 0 && now - previous >= 500000;
}

struct RuntimeMetrics {
    std::atomic<int64_t> main_loop_at_us{0};
    std::atomic<bool> sendspin_connected{false};
    std::atomic<uint32_t> pcm_gaps{0}, i2s_errors{0}, assist_restarts{0};
    std::atomic<uint32_t> mic_queue_drops{0}, mic_overruns{0}, wifi_disconnects{0};
};

inline RuntimeMetrics runtime_metrics;
