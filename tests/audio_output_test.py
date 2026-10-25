"""Run actual audio-output methods against a deterministic partial-write I2S sink."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/audio_output.cpp").read_text()
methods = source[source.index("bool AudioOutput::tts_begin()"):
                 source.index("void AudioOutput::microphone_task(")]
fixture = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>
#include "audio_mix.h"
#include "music_light.h"
#include "runtime_metrics.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(error) assert((error) == ESP_OK)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define CONFIG_JOHAN2_TTS_VOLUME 100
constexpr int ESP_OK = 0, ESP_ERR_TIMEOUT = 1, portMAX_DELAY = 0;
using esp_err_t = int;
static int64_t now = 1000000;
static std::deque<size_t> limits;
static std::vector<uint8_t> delivered;
static int64_t esp_timer_get_time() { return now; }
static void vTaskDelay(uint32_t ms) { now += ms * 1000; }
static void xSemaphoreTake(int, int) {}
static void xSemaphoreGive(int) {}
static void xStreamBufferReset(std::vector<uint8_t>* queue) { queue->clear(); }
static size_t xStreamBufferReceive(std::vector<uint8_t>* queue, void* out, size_t bytes, int) {
    bytes = std::min(bytes, queue->size());
    if (bytes) memcpy(out, queue->data(), bytes);
    queue->erase(queue->begin(), queue->begin() + bytes);
    return bytes;
}
static size_t xStreamBufferSend(std::vector<uint8_t>* queue, const void* in, size_t bytes, int) {
    const auto* data = static_cast<const uint8_t*>(in);
    queue->insert(queue->end(), data, data + bytes);
    return bytes;
}
static int i2s_channel_write(int, const void* in, size_t bytes, size_t* written, uint32_t) {
    *written = limits.empty() ? bytes : std::min(bytes, limits.front());
    if (!limits.empty()) limits.pop_front();
    const auto* data = static_cast<const uint8_t*>(in);
    delivered.insert(delivered.end(), data, data + *written);
    return *written == bytes ? ESP_OK : ESP_ERR_TIMEOUT;
}
static int i2s_channel_disable(int) { return ESP_OK; }
struct Assist { bool command_active() { return false; } };
struct AudioOutput {
    static constexpr size_t DMA_FRAMES = 240, DMA_DESCRIPTORS = 6, CHANNELS = 2;
    bool tts_begin();
    bool music_writer_recent() const;
    bool write_tts_locked(const void*, size_t, size_t&);
    size_t load_tts_locked(size_t);
    void consume_tts_locked(size_t);
    bool tts_write(const int16_t*, size_t, const std::atomic<bool>&);
    bool tts_drain(uint32_t);
    void tts_end();
    void on_stream_start();
    void on_stream_end();
    size_t on_audio_write(uint8_t*, size_t, uint32_t);
    void tx_enable_locked() { tx_enabled_ = true; }
    int tx_lock_ = 0, tx_ = 0;
    bool tx_enabled_ = true;
    uint32_t frames_ = 0;
    std::atomic<bool> active_{true}, muted_{false}, tts_running_{true};
    std::atomic<int> volume_{0};
    std::atomic<int64_t> music_write_at_{now};
    std::atomic<uint8_t> bass_peak_{0}, treble_peak_{0}, mid_peak_{0}, music_peak_{0};
    int16_t scratch_[1024]{}, mix_scratch_[1024]{}, tts_scratch_[1024]{};
    size_t tts_pending_bytes_ = 0;
    std::vector<uint8_t> queue;
    std::vector<uint8_t>* tts_buf_ = &queue;
    Assist* assist_ = nullptr;
    MusicBandMeter music_bands_;
};
static std::vector<uint8_t> speech() {
    std::vector<uint8_t> bytes(4096);
    for (size_t i = 0; i < bytes.size() / 2; ++i) {
        const int16_t sample = 100 + i / 2;
        memcpy(bytes.data() + i * 2, &sample, 2);
    }
    return bytes;
}
"""
checks = r"""
int main() {
    const auto expected = speech();
    uint8_t music[4096]{};
    AudioOutput partial;
    partial.queue = expected;
    limits = {960, 0, 192};
    while (delivered.size() < expected.size())
        partial.on_audio_write(music, std::min(size_t{2048}, expected.size() - delivered.size()), 20);
    assert(delivered == expected && partial.queue.empty() && !partial.tts_pending_bytes_);

    delivered.clear();
    AudioOutput fallback;
    fallback.queue = expected;
    limits = {960};
    assert(fallback.on_audio_write(music, 2048, 20) == 960);
    assert(fallback.tts_pending_bytes_ == 1088);
    now += 100001;
    assert(fallback.tts_drain(250));
    assert(delivered == expected && fallback.queue.empty() && !fallback.tts_pending_bytes_);

    delivered.clear();
    AudioOutput timeout;
    timeout.queue = expected;
    limits = {0};
    assert(timeout.on_audio_write(music, 2048, 20) == 0);
    assert(!timeout.tts_drain(20));
    assert(timeout.tts_pending_bytes_ == 2048 && delivered.empty());
    timeout.on_stream_end();
    assert(timeout.tts_drain(250));
    assert(delivered == expected);

    delivered.clear();
    AudioOutput direct;
    direct.active_.store(false);
    direct.queue = expected;
    int16_t more[4]{1, 1, 2, 2};
    std::atomic<bool> cancel{false};
    limits = {960};
    assert(!direct.tts_write(more, 2, cancel));
    assert(direct.tts_pending_bytes_ == 1088);
    assert(direct.tts_drain(250));
    assert(delivered == expected); // An already-written prefix is not replayed.
    cancel.store(true);
    assert(!direct.tts_write(more, 2, cancel));
    assert(delivered == expected);

    delivered.clear();
    AudioOutput reset;
    reset.queue = expected;
    limits = {0};
    reset.on_audio_write(music, 2048, 20);
    reset.tts_end();
    assert(reset.queue.empty() && !reset.tts_pending_bytes_);
    reset.queue = expected;
    reset.load_tts_locked(2048);
    reset.tts_begin();
    assert(reset.queue.empty() && !reset.tts_pending_bytes_);

    delivered.clear();
    AudioOutput mixed;
    mixed.volume_.store(80);
    mixed.queue = expected;
    int16_t music_pcm[2048];
    std::fill_n(music_pcm, 2048, 1000);
    limits = {960};
    while (delivered.size() < expected.size())
        mixed.on_audio_write(reinterpret_cast<uint8_t*>(music_pcm),
                             std::min(size_t{2048}, expected.size() - delivered.size()), 20);
    for (size_t i = 0; i < delivered.size() / 2; ++i) {
        int16_t actual, spoken;
        memcpy(&actual, delivered.data() + i * 2, 2);
        memcpy(&spoken, expected.data() + i * 2, 2);
        assert(actual == spoken + 400);
    }
    std::cout << "Audio output partial/zero writes, fallback, drain, cancellation, reset and ducking passed\n";
}
"""
with tempfile.TemporaryDirectory(prefix="johan2-audio-test-") as directory:
    binary = Path(directory) / "audio_output"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-I", str(root / "main"),
         "-x", "c++", "-", "-o", str(binary)],
        input=fixture + methods + checks, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
