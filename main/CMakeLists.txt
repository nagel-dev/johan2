idf_component_register(SRCS "main.cpp" "network.cpp" "audio_output.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_i2s esp_timer mdns led_strip sendspin-cpp)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
