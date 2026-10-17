#include <cstring>
#include <algorithm>
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "network.h"
#include "audio_output.h"
#include "assist_client.h"

static const char* TAG = "johan2";

class Persistence : public sendspin::SendspinPersistenceProvider {
public:
    void init() { ESP_ERROR_CHECK(nvs_open("sendspin", NVS_READWRITE, &handle_)); }
    bool save_last_server_hash(uint32_t hash) override {
        return nvs_set_u32(handle_, "last_hash", hash) == ESP_OK && nvs_commit(handle_) == ESP_OK;
    }
    std::optional<uint32_t> load_last_server_hash() override {
        uint32_t hash;
        if (nvs_get_u32(handle_, "last_hash", &hash) != ESP_OK) return std::nullopt;
        return hash;
    }
    bool save_static_delay(uint16_t delay) override {
        return nvs_set_u16(handle_, "delay_ms", delay) == ESP_OK && nvs_commit(handle_) == ESP_OK;
    }
    std::optional<uint16_t> load_static_delay() override {
        uint16_t delay;
        if (nvs_get_u16(handle_, "delay_ms", &delay) != ESP_OK) return std::nullopt;
        return delay;
    }
private:
    nvs_handle_t handle_{};
};

extern "C" void app_main() {
    // Preserve existing identities: unexpected NVS errors require explicit recovery.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "PSRAM available: %u bytes", static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
    static Network network;
    if (!network.start()) return;
    static Persistence persistence;
    persistence.init();
    sendspin::SendspinClientConfig config;
    // Legacy protocol identifies the player by a stable, per-board MAC-derived ID.
    uint8_t device_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(device_mac, ESP_MAC_WIFI_STA));
    char device_id[32];
    snprintf(device_id, sizeof(device_id), "johan2-%02x%02x%02x%02x%02x%02x",
             device_mac[0], device_mac[1], device_mac[2], device_mac[3], device_mac[4], device_mac[5]);
    config.client_id = device_id;
    config.name = CONFIG_JOHAN2_PLAYER_NAME;
    config.product_name = "ESP32-S3 PCM5102A AUX Player";
    config.software_version = "0.2.2";
    static sendspin::SendspinClient client(std::move(config));
    sendspin::PlayerRoleConfig player_config;
    player_config.audio_formats = {
        {sendspin::SendspinCodecFormat::FLAC, 2, 48000, 16},
        {sendspin::SendspinCodecFormat::PCM, 2, 48000, 16},
    };
    auto& player = client.add_player(std::move(player_config));
    static AudioOutput audio;
    static AssistClient assist;
    assist.start(&audio);
    audio.init(player, &assist);
    player.set_listener(&audio);
    player.update_volume(CONFIG_JOHAN2_INITIAL_VOLUME);
    client.set_network_provider(&network);
    client.set_persistence_provider(&persistence);
    if (!client.start()) {
        ESP_LOGE(TAG, "Sendspin failed to start");
        return;
    }
    ESP_ERROR_CHECK(mdns_init());
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "johan2-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(CONFIG_JOHAN2_PLAYER_NAME));
    mdns_txt_item_t txt[] = {{"path", "/sendspin"}, {"name", CONFIG_JOHAN2_PLAYER_NAME}};
    ESP_ERROR_CHECK(mdns_service_add(nullptr, "_sendspin", "_tcp", 8928, txt, 2));
    ESP_LOGI(TAG, "Firmware 0.2.2; Sendspin v0.8.0 legacy transport (unencrypted, no pairing)");
    ESP_LOGI(TAG, "Player ready: %s; listening on TCP 8928", CONFIG_JOHAN2_PLAYER_NAME);
    int64_t next_connect_us = 0;
    while (true) {
        network.poll();
        audio.poll();
        client.loop();
        const int64_t now = esp_timer_get_time();
        if (network.is_network_ready() && !client.is_connected() &&
            strlen(CONFIG_JOHAN2_SERVER_URL) && now >= next_connect_us) {
            client.connect_to(CONFIG_JOHAN2_SERVER_URL);
            // Exceeds the library's TCP/upgrade/DNS connect bounds; do not
            // replace a still-pending connection on every main-loop tick.
            next_connect_us = now + 60000000;
        }
        vTaskDelay(std::max(pdMS_TO_TICKS(5), TickType_t{1}));
    }
}
