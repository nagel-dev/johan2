"""Exercise actual TTS request allocation, initialization and cleanup paths."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/tts_player.cpp").read_text()
play = source[source.index("TtsPlayer::Result TtsPlayer::play()"):]
fixture = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_AUDIO_ERR_OK = 0;
constexpr int ESP_AUDIO_SIMPLE_DEC_TYPE_WAV = 1, ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 = 2;
struct esp_http_client_event_t {};
struct esp_http_client_config_t {
    const char* url;
    int timeout_ms, buffer_size;
    esp_err_t (*event_handler)(esp_http_client_event_t*);
    void* user_data;
};
struct esp_audio_simple_dec_cfg_t { int dec_type; };
struct AudioOutput {
    bool tts_drain(int) { return true; }
    void tts_end() {}
};
static bool fail_allocation = false, fail_client = false, fail_fetch = false, bad_format = false;
static size_t outstanding = 0;
static int reads = 0, closes = 0, cleanups = 0;
static void* tracked_malloc(size_t size) {
    if (fail_allocation) return nullptr;
    void* result = std::malloc(size);
    if (result) outstanding += size;
    return result;
}
static void tracked_free(void* pointer) {
    if (pointer) outstanding -= 8192;
    std::free(pointer);
}
static int64_t esp_timer_get_time() { return 1000000; }
static void* esp_http_client_init(esp_http_client_config_t* config) {
    assert(config->timeout_ms == 2000 && config->event_handler && config->user_data);
    reads = 0;
    return fail_client ? nullptr : reinterpret_cast<void*>(1);
}
static int open_tts_response(void*, const std::atomic<bool>& cancel) {
    return fail_fetch || cancel.load() ? ESP_FAIL : ESP_OK;
}
static int esp_http_client_get_status_code(void*) { return 200; }
static int esp_http_client_read(void*, char* data, size_t capacity) {
    assert(capacity >= 8);
    if (reads++) return 0;
    memcpy(data, bad_format ? "Nope...." : "RIFF....", 8);
    return 8;
}
static bool esp_http_client_is_complete_data_received(void*) { return true; }
static int esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*, void** decoder) {
    *decoder = reinterpret_cast<void*>(2);
    return ESP_OK;
}
static void esp_audio_simple_dec_close(void*) {}
static void esp_http_client_close(void*) { ++closes; }
static void esp_http_client_cleanup(void*) { ++cleanups; }
struct TtsPlayer {
    enum Result { None, Ok, Unsupported, Failed, CueDone };
    Result play();
    static esp_err_t http_event(esp_http_client_event_t*) { return ESP_OK; }
    bool decode(uint8_t*, size_t) { started_ = true; return true; }
    bool flush() { return true; }
    std::atomic<bool> cancel_{false};
    std::string url_;
    bool info_ready_ = false, have_prev_ = false, started_ = false;
    uint32_t rate_ = 0, channels_ = 0, frac_ = 0;
    size_t carry_len_ = 0, out_len_ = 0, decoded_capacity_ = 0;
    uint8_t* decoded_ = nullptr;
    void* decoder_ = nullptr;
    AudioOutput output;
    AudioOutput* audio_ = &output;
};
#define malloc tracked_malloc
#define free tracked_free
"""
checks = r"""
#undef malloc
#undef free
int main() {
    TtsPlayer player;
    fail_allocation = true;
    assert(player.play() == TtsPlayer::Unsupported);
    assert(!player.decoded_ && outstanding == 0);
    fail_allocation = false;
    for (int i = 0; i < 1000; ++i) {
        fail_client = true;
        assert(player.play() == TtsPlayer::Unsupported);
        assert(!player.decoded_ && outstanding == 0);
        fail_client = false;
        assert(player.play() == TtsPlayer::Ok);
        assert(!player.decoded_ && !player.decoder_ && outstanding == 0);
    }
    fail_fetch = true;
    assert(player.play() == TtsPlayer::Unsupported && outstanding == 0);
    fail_fetch = false;
    bad_format = true;
    assert(player.play() == TtsPlayer::Unsupported && outstanding == 0);
    bad_format = false;
    player.cancel_.store(true);
    assert(player.play() == TtsPlayer::Ok && outstanding == 0);
    assert(!player.cancel_.load());
    assert(closes == 1003 && cleanups == closes);
    std::cout << "TTS allocation failure, HTTP initialization, recovery and cleanup tests passed\n";
}
"""
with tempfile.TemporaryDirectory(prefix="johan2-tts-player-test-") as directory:
    binary = Path(directory) / "tts_player"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
         "-fsanitize=address,undefined", "-x", "c++", "-", "-o", str(binary)],
        input=fixture + play + checks, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
