set( SRC_FILES
    "main.c"
    "pogdev_app.c"
    "pogmic_app.c"
    "alac_magic_cookie.c"
    "settings.c"
    "audio/audio_receiver.c"
    "audio/audio_stream.c"
    "audio/audio_stream_realtime.c"
    "audio/audio_stream_buffered.c"
    "audio/audio_decoder.c"
    "audio/audio_buffer.c"
    "audio/audio_timing.c"
    "audio/audio_crypto.c"
    "audio/audio_resample.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_rsa.c"
    "rtsp/rtsp_events.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/base64.c"
    "hap/hap.c"
    "hap/hap_pair_verify.c"
    "hap/hap_pair_setup.c"
    "hap/hap_crypto.c"
    "hap/srp.c"
    "hap/tlv8.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ptp_clock.c"
    "network/ntp_clock.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/web_index.S"
    "network/ota.c"
    "network/dns_server.c"
    "network/log_stream.c"
    "network/ha_mqtt.c"
    "led.c"
    "led_matrix.c"
    "led_argb.c"
    "led_argb_policy.c"
    "effect_sync.c"
    "audio/eq_events.c"
    "audio/eq_dsp.c"
    "audio/audio_limiter.c"
    "buttons.c"
    "playback_control.c"
    "dacp_client.c"
    "amp_ctrl.c"
)

# Select audio output implementation at compile time
if(CONFIG_AUDIO_OUTPUT_SPDIF)
    list(APPEND SRC_FILES "audio/audio_output_spdif.c")
elseif(CONFIG_AUDIO_OUTPUT_USB)
    list(APPEND SRC_FILES "audio/audio_output_usb.c")
else()
    list(APPEND SRC_FILES "audio/audio_output.c")
endif()

# Bluetooth A2DP sink (only available on ESP32 with Classic BT)
if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND SRC_FILES "audio/a2dp_sink.c")
endif()

# BLE onboarding (NimBLE — ESP32-S3 targets; mutually exclusive with the
# Bluedroid A2DP build above, which owns the radio on classic ESP32)
if(CONFIG_BT_NIMBLE_ENABLED)
    list(APPEND SRC_FILES "network/ble_onboarding.c" "network/onboarding.c")
endif()

# W5500 SPI Ethernet
if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND SRC_FILES "network/ethernet.c")
endif()

set( INC_DIRS
    "." 
    "audio" 
    "rtsp" 
    "plist" 
    "hap" 
    "network"
)

set( DEPS
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_ringbuf
    esp_driver_i2s
    esp_driver_rmt
    espressif__led_strip
    esp_timer
    esp_http_server
    esp_http_client
    esp_https_ota
    app_update
    json
    mbedtls
    esp_driver_ledc
    boards
    dac
    dac_tas58xx
    display
    audio-resampler
    esp_eth
    spiffs_storage
    mqtt
    esp_app_format
    pogdev
    pogmic
    pogvoice
    pogwake
)

# TODO: why these fall over in CI and fine locally...
# Bluetooth component only needed when A2DP is enabled
#if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND DEPS bt)
#endif()

# Ethernet component only needed when W5500 is enabled
#if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
#endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)

# Embed the control panel in the application image so normal OTA updates also
# refresh the UI. A tiny .incbin wrapper avoids PlatformIO duplicating the
# generated ESP-IDF binary-data source path.
set(EMBEDDED_WEB_DIR "${CMAKE_BINARY_DIR}/embedded-web")
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/compress_web.py"
    "${CMAKE_SOURCE_DIR}/data/www" "${EMBEDDED_WEB_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/data/www/index.html"
    "${CMAKE_SOURCE_DIR}/data/www/logs.html")
set_source_files_properties(
    "network/web_index.S"
    PROPERTIES
        COMPILE_DEFINITIONS
            "WEB_INDEX_HTML_PATH=\"${EMBEDDED_WEB_DIR}/index.html.gz\";WEB_LOGS_HTML_PATH=\"${EMBEDDED_WEB_DIR}/logs.html.gz\""
        OBJECT_DEPENDS
            "${CMAKE_SOURCE_DIR}/data/www/index.html;${CMAKE_SOURCE_DIR}/data/www/logs.html"
)

# Enable function wrapping for abort() to intercept panics and mute GPIO
if(CONFIG_BOARD_SQUEEZEAMP_COMMON)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=abort")
endif()
