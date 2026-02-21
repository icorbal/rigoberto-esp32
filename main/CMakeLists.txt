idf_component_register(
    SRCS "avatar_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        nvs_flash
        json
        esp_codec_dev
        esp-sr
)
