#pragma once
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_HTTP_MAX_REDIRECT = -2;
struct FakeHttpClient;
using esp_http_client_handle_t = FakeHttpClient*;

esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_clear_response_buffer(esp_http_client_handle_t);
