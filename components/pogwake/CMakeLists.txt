set(WAKE_SOURCES "pogwake.c")
set(WAKE_DEPS pogmic pogvoice audio-resampler esp_timer nvs_flash)
# IDF resolves component requirements before sdkconfig is available.
if(IDF_TARGET STREQUAL "esp32")
    list(APPEND WAKE_DEPS espressif__esp-tflite-micro esphome__esp-micro-speech-features)
endif()
if(CONFIG_POG_WAKEWORD)
    list(APPEND WAKE_SOURCES "pogwake_engine.cpp" "wake_models.S")
endif()
idf_component_register(SRCS ${WAKE_SOURCES} INCLUDE_DIRS "include" REQUIRES ${WAKE_DEPS})
if(CONFIG_POG_WAKEWORD)
    set(WAKE_PACK_DIR "${CMAKE_BINARY_DIR}/embedded-wake")
    execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/pack_models.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/models" "${WAKE_PACK_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_LITE_STATIC_MEMORY ESP_NN
        "POG_WAKE_JARVIS=\"${WAKE_PACK_DIR}/hey_jarvis.z\""
        "POG_WAKE_NABU=\"${WAKE_PACK_DIR}/okay_nabu.z\""
        "POG_WAKE_ALEXA=\"${WAKE_PACK_DIR}/alexa.z\"")
    set_source_files_properties("wake_models.S" PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_jarvis.tflite;${CMAKE_CURRENT_SOURCE_DIR}/models/okay_nabu.tflite;${CMAKE_CURRENT_SOURCE_DIR}/models/alexa.tflite")
endif()
