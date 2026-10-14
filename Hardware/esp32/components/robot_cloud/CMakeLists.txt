idf_component_register(
    SRCS "robot_cloud.c" "robot_voice.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_client
        esp-tls
        esp_app_format
        json
        esp_timer
        mbedtls
        heap
        robot_audio
        robot_motion
        robot_ui
)
