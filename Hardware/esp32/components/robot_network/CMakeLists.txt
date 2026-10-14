idf_component_register(
    SRCS "robot_network.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_wifi esp_event esp_netif nvs_flash esp_http_server
        esp_http_client esp-tls esp_hw_support json lwip mbedtls
)
