#include "tts_http.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

struct Response {
    int status;
    std::string body;
};
struct FakeHttpClient {
    std::vector<Response> responses;
    size_t index = 0;
    int opens = 0, closes = 0, clears = 0;
    std::string cached{};
    esp_err_t open_error = ESP_OK, redirect_error = ESP_OK, close_error = ESP_OK, clear_error = ESP_OK;
    bool header_error = false;
    std::atomic<bool>* cancel_after_redirect = nullptr;
};

esp_err_t esp_http_client_open(FakeHttpClient* client, int) {
    ++client->opens;
    return client->open_error;
}
int64_t esp_http_client_fetch_headers(FakeHttpClient* client) {
    if (client->header_error) return -1;
    client->cached += client->responses.at(client->index).body;
    return client->responses.at(client->index).body.size();
}
int esp_http_client_get_status_code(FakeHttpClient* client) {
    return client->responses.at(client->index).status;
}
esp_err_t esp_http_client_set_redirection(FakeHttpClient* client) {
    if (client->redirect_error != ESP_OK) return client->redirect_error;
    ++client->index;
    if (client->cancel_after_redirect) client->cancel_after_redirect->store(true);
    return ESP_OK;
}
esp_err_t esp_http_client_close(FakeHttpClient* client) {
    ++client->closes;
    return client->close_error; // ESP-IDF leaves the fetched response body cached.
}
esp_err_t esp_http_client_clear_response_buffer(FakeHttpClient* client) {
    ++client->clears;
    if (client->clear_error == ESP_OK) client->cached.clear();
    return client->clear_error;
}

int main() {
    std::atomic<bool> cancel{false};
    for (int redirect : {301, 302, 303, 307, 308}) {
        FakeHttpClient client{{{redirect, "302: Found"}, {200, "ID3audio"}}};
        assert(open_tts_response(&client, cancel) == ESP_OK);
        assert(client.cached == "ID3audio"); // Same-origin redirect text must never reach the decoder.
        assert(client.opens == 2 && client.closes == 1 && client.clears == 1);
    }
    FakeHttpClient direct{{{200, "RIFFaudio"}}};
    assert(open_tts_response(&direct, cancel) == ESP_OK);
    assert(direct.opens == 1 && direct.closes == 0 && direct.clears == 0);
    assert(direct.cached == "RIFFaudio");
    FakeHttpClient chain{{{302, "one"}, {307, "two"}, {308, "three"}, {200, "ID3audio"}}};
    assert(open_tts_response(&chain, cancel) == ESP_OK && chain.cached == "ID3audio");
    assert(chain.opens == 4 && chain.clears == 3);
    FakeHttpClient loop{{{302, "one"}, {302, "two"}, {302, "three"}, {302, "four"}}};
    assert(open_tts_response(&loop, cancel) == ESP_ERR_HTTP_MAX_REDIRECT);
    assert(loop.opens == 4 && loop.clears == 3);
    FakeHttpClient not_found{{{404, "Not found"}}};
    assert(open_tts_response(&not_found, cancel) == ESP_OK);
    assert(esp_http_client_get_status_code(&not_found) == 404); // Caller rejects non-audio status.
    for (int failure = 0; failure < 5; ++failure) {
        FakeHttpClient client{{{302, "redirect"}, {200, "ID3audio"}}};
        if (failure == 0) client.open_error = -10;
        if (failure == 1) client.header_error = true;
        if (failure == 2) client.redirect_error = -12;
        if (failure == 3) client.close_error = -13;
        if (failure == 4) client.clear_error = -14;
        assert(open_tts_response(&client, cancel) == (failure == 1 ? ESP_FAIL : -10 - failure));
        assert(client.opens == 1);
    }
    cancel.store(true);
    FakeHttpClient cancelled{{{200, "audio"}}};
    assert(open_tts_response(&cancelled, cancel) == ESP_FAIL && cancelled.opens == 0);
    cancel.store(false);
    FakeHttpClient during_redirect{{{302, "redirect"}, {200, "audio"}}};
    during_redirect.cancel_after_redirect = &cancel;
    assert(open_tts_response(&during_redirect, cancel) == ESP_FAIL);
    assert(during_redirect.opens == 1 && during_redirect.clears == 1);
    std::cout << "TTS HTTP redirect tests passed\n";
}
