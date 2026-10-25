"""Exercise the firmware poll function with deterministic Wi-Fi event interleavings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/network.cpp").read_text()
poll = source[source.index("void Network::poll()"):]
fixture = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using esp_err_t = int;
constexpr int ESP_OK = 0;
static int64_t now = 1000000;
static int attempts = 0, connect_result = ESP_OK;
static std::function<void()> during_connect;
static int64_t esp_timer_get_time() { return now; }
static int esp_wifi_connect() {
    ++attempts;
    if (during_connect) during_connect();
    return connect_result;
}
struct wifi_ap_record_t {};
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t*) { return ESP_OK; }
struct EventAtomic {
    bool value = false;
    std::function<void()> after_load;
    bool load() {
        const bool previous = value;
        if (after_load) {
            auto event = std::move(after_load);
            after_load = {};
            event();
        }
        return previous;
    }
    void store(bool next) { value = next; }
};
struct Network {
    void poll();
    EventAtomic connected_;
    std::atomic<bool> retry_{false};
    int64_t retry_at_{0}, link_log_at_{0};
    uint32_t retry_delay_ms_{1000};
};
"""
checks = r"""
int main() {
    Network network;
    network.connected_.store(true);
    network.connected_.after_load = [&] {
        network.connected_.store(false);
        network.retry_.store(true);
    };
    network.poll();
    assert(network.retry_.load());
    network.poll();
    assert(attempts == 1 && !network.retry_.load());
    network.poll();
    assert(attempts == 1); // Do not replace an in-flight association attempt.

    network.retry_.store(true);
    now += 1000000;
    connect_result = -1;
    network.poll();
    assert(attempts == 2 && network.retry_.load());
    network.poll();
    assert(attempts == 2); // Preserve backoff after a synchronous error.
    now += 2000000;
    connect_result = ESP_OK;
    during_connect = [&] { network.retry_.store(true); };
    network.poll();
    assert(attempts == 3 && network.retry_.load());
    during_connect = {};
    network.connected_.store(true);
    network.poll();
    assert(network.retry_delay_ms_ == 1000 && network.retry_at_ == 0);
    network.connected_.store(false);
    network.poll();
    assert(attempts == 4); // A newer disconnect remains actionable.
    std::cout << "Wi-Fi retry interleaving and backoff tests passed\n";
}
"""
with tempfile.TemporaryDirectory(prefix="johan2-network-test-") as directory:
    binary = Path(directory) / "network_retry"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
         "-fsanitize=address,undefined", "-x", "c++", "-", "-o", str(binary)],
        input=fixture + poll + checks, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
