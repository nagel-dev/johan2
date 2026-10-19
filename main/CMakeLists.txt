idf_component_register(SRCS "main.cpp" "network.cpp" "audio_output.cpp" "assist_client.cpp" "tts_player.cpp" "mic_diagnostics.cpp" "mic_aec.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash esp_driver_i2s esp_timer mdns esp_driver_rmt esp_websocket_client esp_http_client espressif__esp-sr espressif__esp_audio_codec bblanchon__arduinojson sendspin-cpp)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# The real-time FIR/FFT/AEC adapter must keep up with 16 ms microphone DMA.
# Leave the rest of the firmware at the selected project debug optimization.
set_source_files_properties(audio_output.cpp mic_aec.cpp PROPERTIES COMPILE_OPTIONS "-O2")
