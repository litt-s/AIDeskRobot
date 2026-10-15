idf_component_register(
    SRCS "robot_wakeup.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__esp-sr esp_timer freertos nvs_flash
)
