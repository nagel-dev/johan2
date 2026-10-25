"""Verify the actual client initialization blocks all attach TLS certificate trust."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
assist = (root / "main/assist_client.cpp").read_text()
tts = (root / "main/tts_player.cpp").read_text()
websocket = assist[assist.index("esp_websocket_client_config_t cfg{};"):
                   assist.index("socket_ = esp_websocket_client_init")]
post = assist[assist.index("esp_http_client_config_t config{};"):
              assist.index("if (auto client = esp_http_client_init")]
reply = tts[tts.index("esp_http_client_config_t config{};"):
            tts.index("auto client = esp_http_client_init")]
fixture = r"""
#include <cassert>
#include <iostream>
#include <string>
using esp_err_t = int;
static esp_err_t esp_crt_bundle_attach(void*) { return 0; }
constexpr const char* CONFIG_JOHAN2_HA_WS_URL = "wss://ha.example.test/api/websocket";
constexpr int HTTP_METHOD_POST = 1;
struct esp_websocket_client_config_t {
    const char* uri;
    const char* task_name;
    int task_stack, buffer_size, reconnect_timeout_ms, network_timeout_ms;
    bool enable_close_reconnect;
    esp_err_t (*crt_bundle_attach)(void*);
    bool skip_cert_common_name_check;
};
struct esp_http_client_event_t {};
struct esp_http_client_config_t {
    const char* url;
    int method, timeout_ms, buffer_size;
    esp_err_t (*event_handler)(esp_http_client_event_t*);
    void* user_data;
    esp_err_t (*crt_bundle_attach)(void*);
    bool skip_cert_common_name_check;
};
struct Owner {
    std::string url_ = "https://ha.example.test/reply.wav";
    static esp_err_t http_event(esp_http_client_event_t*) { return 0; }
    void websocket() {
"""
source = fixture + websocket + r"""
        assert(cfg.crt_bundle_attach == esp_crt_bundle_attach);
        assert(!cfg.skip_cert_common_name_check);
    }
    void post() {
        std::string url = "https://proxy.example.test/transcript";
""" + post + r"""
        assert(config.crt_bundle_attach == esp_crt_bundle_attach);
        assert(!config.skip_cert_common_name_check);
    }
    void reply() {
""" + reply + r"""
        assert(config.crt_bundle_attach == esp_crt_bundle_attach);
        assert(!config.skip_cert_common_name_check);
    }
};
int main() {
    Owner owner;
    owner.websocket();
    owner.post();
    owner.reply();
    std::cout << "HA WebSocket, TTS and HomeDash HTTP certificate trust tests passed\n";
}
"""
with tempfile.TemporaryDirectory(prefix="johan2-tls-config-test-") as directory:
    binary = Path(directory) / "tls_config"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-x", "c++", "-", "-o", str(binary)],
        input=source, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
