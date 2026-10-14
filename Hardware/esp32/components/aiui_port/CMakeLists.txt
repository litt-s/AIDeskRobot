idf_component_register(
    SRCS
        "aiui_port.c"
        "aiui_os_freertos.c"
        "aiui_socket_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "${AIUI_VENDOR_ROOT}/include/aiui"
    PRIV_REQUIRES
        freertos
        esp_timer
        heap
        esp-tls
        mbedtls
        lwip
)
