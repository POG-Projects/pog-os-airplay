idf_component_register(SRCS "pogvoice.c" "pogvoice_protocol.c" "pogvoice_resample.c" "pogvoice_feedback.c" "pogvoice_worker.c"
    INCLUDE_DIRS "include"
    REQUIRES json nvs_flash esp_http_client esp_timer esp_driver_i2s
    esp_websocket_client esp_audio_codec mbedtls pogmic)
