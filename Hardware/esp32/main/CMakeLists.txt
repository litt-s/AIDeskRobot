idf_component_register(
    SRCS "app_main.cpp" "robot_app.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        robot_audio
        robot_cloud
        robot_network
        robot_wakeup
        robot_ui
        robot_motion
        robot_gesture
        espressif__esp-sr
        driver
        esp_timer
        heap
        esp_hw_support
        json
        nvs_flash
)
