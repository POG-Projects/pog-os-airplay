# AirPlay 2 Receiver for ESP32
cmake_minimum_required(VERSION 3.16)

# Read firmware version from version.txt
file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

# Add components directory
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(IDF_VERSION_MAJOR LESS 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR LESS 5))
    message(WARNING
        "ESP-IDF ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH} detected. "
        "This project is developed and tested against ESP-IDF >= 5.5. "
        "Older versions may require workarounds (e.g. for hosted-WiFi Kconfig symbols on ESP32-P4). "
        "Please upgrade with: cd $ENV{IDF_PATH} && git fetch && git checkout v5.5.2 && ./install.sh")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airplay2-receiver VERSION ${PROJECT_VER})

include(cmake/fix_usb_device_uac_platformio.cmake)

# Fit the optional local detector into the existing 4 MB dual-OTA layout.
# ESP-NN keeps its optimized kernels; size-optimize TFLM's dispatch/helpers
# instead of its component's default -O3. Do not modify managed sources.
if(CONFIG_POG_WAKEWORD)
    idf_component_get_property(pog_tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
    get_target_property(pog_tflm_options ${pog_tflm_lib} COMPILE_OPTIONS)
    string(REPLACE "-O3" "-Os" pog_tflm_options "${pog_tflm_options}")
    set_target_properties(${pog_tflm_lib} PROPERTIES COMPILE_OPTIONS "${pog_tflm_options}")
endif()

# Build a SPIFFS image from data/ and flash it alongside the firmware.
# Partition label "storage" matches partitions.csv. Used by:
#   - components/dac_tas57xx/ (HybridFlow DAC firmware)
#   - components/display/     (ST7789 background image)
# Flash firmware + SPIFFS together with: idf.py build flash
if(CONFIG_BOARD_WROVER_E)
    # The 4 MB board has no TFT. Keep every web resource, omitting only the
    # optional display background that overflows its 188 KiB SPIFFS partition.
    set(POG_SPIFFS_DATA "${CMAKE_BINARY_DIR}/compact-data")
    execute_process(
        COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/compact_filesystem.py"
                "${CMAKE_SOURCE_DIR}/data" "${POG_SPIFFS_DATA}"
        RESULT_VARIABLE POG_SPIFFS_RESULT
    )
    if(NOT POG_SPIFFS_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not stage the WROVER-E web filesystem")
    endif()
else()
    set(POG_SPIFFS_DATA "${CMAKE_SOURCE_DIR}/data")
endif()
spiffs_create_partition_image(storage "${POG_SPIFFS_DATA}" FLASH_IN_PROJECT)
