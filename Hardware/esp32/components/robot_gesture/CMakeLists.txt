idf_component_register(
    SRCS "robot_gesture.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver freertos robot_ui
)
