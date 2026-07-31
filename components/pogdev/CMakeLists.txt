# esp_app_format : pogdev_fw_version() lit la version dans le descripteur de
# l'image. Ce composant ne fait pas partie des dépendances implicites de l'IDF,
# il doit donc être demandé, sinon esp_app_desc.h reste introuvable.
idf_component_register(
    SRCS "pogdev_discovery.c" "pogdev_enrol.c" "pogdev_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__mdns esp_netif esp_http_client nvs_flash json esp_hw_support mqtt esp_wifi esp_app_format
)
