idf_component_register(
    SRCS "robot_audio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer spiffs
)
