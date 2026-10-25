"""Exercise actual TTS cancellation and HTTP events with blocked local TCP sockets."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/tts_player.cpp").read_text()
methods = source[source.index("void TtsPlayer::cancel()"):
                 source.index("void TtsPlayer::task(")]
fixture = r"""
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(error) assert((error) == ESP_OK)
using esp_err_t = int;
constexpr int ESP_OK = 0, HTTP_EVENT_ON_CONNECTED = 1, HTTP_EVENT_DISCONNECTED = 2;
static void tcp_pair(int pair[2]) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    pair[1] = socket(AF_INET, SOCK_STREAM, 0);
    assert(pair[1] >= 0);
    assert(connect(pair[1], reinterpret_cast<sockaddr*>(&address), length) == 0);
    pair[0] = accept(listener, nullptr, nullptr);
    assert(pair[0] >= 0);
    close(listener);
}
struct Client { int fd, timeout = 2000; };
static int esp_http_client_get_socket(Client* client) { return client->fd; }
static int esp_http_client_set_timeout_ms(Client* client, int timeout) {
    client->timeout = timeout;
    return ESP_OK;
}
struct esp_http_client_event_t { int event_id; void* user_data; Client* client; };
struct TtsPlayer {
    void cancel();
    static esp_err_t http_event(esp_http_client_event_t*);
    std::atomic<bool> busy_{true}, cancel_{false};
    std::mutex http_mutex_;
    int http_socket_{-1};
};
"""
checks = r"""
int main() {
    TtsPlayer player;
    for (int iteration = 0; iteration < 100; ++iteration) {
        int pair[2];
        tcp_pair(pair);
        Client client{pair[0]};
        esp_http_client_event_t event{HTTP_EVENT_ON_CONNECTED, &player, &client};
        player.cancel_.store(false);
        assert(TtsPlayer::http_event(&event) == ESP_OK);
        assert(client.timeout == 30000);
        std::promise<void> entering_read;
        auto reader = std::async(std::launch::async, [&] {
            entering_read.set_value();
            char byte;
            return recv(pair[0], &byte, 1, 0);
        });
        entering_read.get_future().wait();
        player.cancel();
        assert(reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        assert(reader.get() == 0);
        event.event_id = HTTP_EVENT_DISCONNECTED;
        TtsPlayer::http_event(&event);
        assert(player.http_socket_ == -1 && client.timeout == 2000);
        close(pair[0]);
        close(pair[1]);

        tcp_pair(pair);
        player.cancel(); // The old descriptor must no longer be published.
        char sent = 'x', received = 0;
        assert(send(pair[1], &sent, 1, 0) == 1);
        assert(recv(pair[0], &received, 1, 0) == 1 && received == sent);
        close(pair[0]);
        close(pair[1]);
    }
    int pair[2];
    tcp_pair(pair);
    Client client{pair[0]};
    esp_http_client_event_t event{HTTP_EVENT_ON_CONNECTED, &player, &client};
    player.cancel_.store(false);
    player.cancel(); // Cancellation before connection publication is not lost.
    TtsPlayer::http_event(&event);
    char byte;
    assert(recv(pair[0], &byte, 1, 0) == 0);
    event.event_id = HTTP_EVENT_DISCONNECTED;
    TtsPlayer::http_event(&event);
    close(pair[0]);
    close(pair[1]);
    player.busy_.store(false);
    player.cancel_.store(false);
    player.cancel();
    assert(!player.cancel_.load());
    std::cout << "TTS blocked-read cancellation, late connection and descriptor-lifetime tests passed\n";
}
"""
with tempfile.TemporaryDirectory(prefix="johan2-tts-cancel-test-") as directory:
    binary = Path(directory) / "tts_cancel"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-fsanitize=address,undefined", "-x", "c++", "-", "-o", str(binary)],
        input=fixture + methods + checks, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
