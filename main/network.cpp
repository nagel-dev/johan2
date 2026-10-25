#include "network.h"
#include "runtime_metrics.h"
#include <cstring>
#include <algorithm>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char* TAG = "network";

void Network::event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto& self = *static_cast<Network*>(arg);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        self.retry_.store(true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        runtime_metrics.wifi_disconnects.fetch_add(1);
        self.connected_.store(false);
        self.retry_.store(true);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reason=%u",
                 static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        self.connected_.store(true);
        const auto* info = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&info->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        self.connected_.store(false);
    }
}

bool Network::start() {
    char ssid[33]{};
    char password[65]{};
    nvs_handle_t settings;
    ESP_ERROR_CHECK(nvs_open("player_config", NVS_READWRITE, &settings));
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    // Empty menuconfig settings permit credentials provisioned in NVS to be reused.
    const esp_err_t ssid_err = nvs_get_str(settings, "wifi_ssid", ssid, &ssid_size);
    const esp_err_t password_err = nvs_get_str(settings, "wifi_pass", password, &password_size);
    if (ssid_err != ESP_OK && ssid_err != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(ssid_err);
    if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(password_err);
    if (strlen(CONFIG_JOHAN2_WIFI_SSID)) {
        strlcpy(ssid, CONFIG_JOHAN2_WIFI_SSID, sizeof(ssid));
        strlcpy(password, CONFIG_JOHAN2_WIFI_PASSWORD, sizeof(password));
        ESP_ERROR_CHECK(nvs_set_str(settings, "wifi_ssid", ssid));
        ESP_ERROR_CHECK(nvs_set_str(settings, "wifi_pass", password));
        ESP_ERROR_CHECK(nvs_commit(settings));
    }
    nvs_close(settings);
    if (!ssid[0]) {
        ESP_LOGE(TAG, "Set Wi-Fi in menuconfig -> Johan2 Player Configuration, rebuild and flash");
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    auto* sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta, "johan2-aux"));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event, this));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t config{};
    static_assert(sizeof(CONFIG_JOHAN2_WIFI_SSID) - 1 <= sizeof(config.sta.ssid), "SSID too long");
    static_assert(sizeof(CONFIG_JOHAN2_WIFI_PASSWORD) - 1 <= sizeof(config.sta.password), "Password too long");
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    return true;
}

void Network::poll() {
    const int64_t now = esp_timer_get_time();
    if (connected_.load()) {
        retry_delay_ms_ = 1000;
        retry_at_ = 0;
        if (now >= link_log_at_) {
            link_log_at_ = now + 10'000'000LL;
            wifi_ap_record_t ap{};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi link: rssi=%d dBm channel=%u bssid=..:%02x:%02x",
                         ap.rssi, ap.primary, ap.bssid[4], ap.bssid[5]);
            }
        }
        return;
    }
    if (now >= retry_at_ && retry_.exchange(false)) {
        retry_at_ = now + retry_delay_ms_ * 1000LL;
        retry_delay_ms_ = std::min(retry_delay_ms_ * 2, uint32_t{60000});
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Connect: %s", esp_err_to_name(err));
            retry_.store(true);
        }
    }
}
