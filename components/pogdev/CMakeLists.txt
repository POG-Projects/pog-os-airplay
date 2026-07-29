idf_component_register(
    SRCS "pogdev_discovery.c" "pogdev_enrol.c" "pogdev_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__mdns esp_netif esp_http_client nvs_flash json esp_hw_support mqtt esp_wifi
)
