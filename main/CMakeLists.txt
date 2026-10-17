idf_component_register(SRCS "main.cpp" "network.cpp" "audio_output.cpp" "assist_client.cpp" "tts_player.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_i2s esp_timer mdns esp_driver_rmt esp_websocket_client esp_http_client espressif__esp_audio_codec bblanchon__arduinojson sendspin-cpp)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
