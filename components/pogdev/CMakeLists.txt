idf_component_register(
    SRCS "pogdev_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__mdns esp_netif
)
