idf_component_register(
    SRCS "pogmic.c" "pogmic_pcm.c" "pogmic_output.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_driver_gpio esp_timer
)
