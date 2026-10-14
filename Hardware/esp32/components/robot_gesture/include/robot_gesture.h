#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_GESTURE_UP,
    ROBOT_GESTURE_DOWN,
    ROBOT_GESTURE_LEFT,
    ROBOT_GESTURE_RIGHT,
    ROBOT_GESTURE_FORWARD,
    ROBOT_GESTURE_BACKWARD,
    ROBOT_GESTURE_CLOCKWISE,
    ROBOT_GESTURE_COUNTERCLOCKWISE,
    ROBOT_GESTURE_WAVE,
    /* Synthesized after three alternating directional gestures. */
    ROBOT_GESTURE_NOD,
    ROBOT_GESTURE_SHAKE,
} robot_gesture_t;

typedef void (*robot_gesture_fn)(void *ctx, robot_gesture_t gesture);

esp_err_t robot_gesture_init(robot_gesture_fn callback, void *ctx);
esp_err_t robot_gesture_start(void);
void robot_gesture_stop(void);
void robot_gesture_deinit(void);

#ifdef __cplusplus
}
#endif
