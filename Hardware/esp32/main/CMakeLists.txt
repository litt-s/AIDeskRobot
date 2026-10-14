idf_component_register(
    SRCS "app_main.cpp" "robot_app.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        aiui_sdk
        aiui_port
        robot_audio
        robot_cloud
        robot_network
        robot_wakeup
        robot_ui
        robot_motion
        robot_gesture
        espressif__esp-sr
        espressif__esp_audio_simple_player
        espressif__gmf_core
        driver
        esp_timer
        heap
        esp_hw_support
        json
        nvs_flash
)
