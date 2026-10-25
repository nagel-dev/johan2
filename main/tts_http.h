#pragma once
#include <atomic>
#include "esp_http_client.h"

inline esp_err_t open_tts_response(esp_http_client_handle_t client, const std::atomic<bool>& cancel) {
    for (unsigned redirects = 0; redirects <= 3; ++redirects) {
        if (cancel.load()) return ESP_FAIL;
        esp_err_t error = esp_http_client_open(client, 0);
        if (error != ESP_OK) return error;
        if (esp_http_client_fetch_headers(client) < 0) return ESP_FAIL;
        const int status = esp_http_client_get_status_code(client);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) return ESP_OK;
        if (redirects == 3) return ESP_ERR_HTTP_MAX_REDIRECT;
        error = esp_http_client_set_redirection(client);
        if (error != ESP_OK) return error;
        error = esp_http_client_close(client);
        if (error != ESP_OK) return error;
        // Closing does not discard body bytes cached while fetching headers.
        error = esp_http_client_clear_response_buffer(client);
        if (error != ESP_OK) return error;
    }
    return ESP_FAIL;
}
