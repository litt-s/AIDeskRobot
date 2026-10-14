#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "robot_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_ACTION_CENTER,
    ROBOT_ACTION_LOOK_LEFT,
    ROBOT_ACTION_LOOK_RIGHT,
    ROBOT_ACTION_LOOK_UP,
    ROBOT_ACTION_LOOK_DOWN,
    ROBOT_ACTION_NOD,
    ROBOT_ACTION_SHAKE,
    ROBOT_ACTION_ROLL,
    ROBOT_ACTION_DANCE,
    ROBOT_ACTION_BLINK,
    ROBOT_ACTION_WAKEUP,
    ROBOT_ACTION_SLEEP,
    ROBOT_ACTION_RANDOM,
    ROBOT_ACTION_EXPRESSION,
    ROBOT_ACTION_VOLUME_DELTA,
    ROBOT_ACTION_SHOW_PROVISIONING,
    ROBOT_ACTION_STOP,
} robot_action_type_t;

typedef struct {
    robot_action_type_t type;
    robot_expression_t expression;
    int value;
    char text_a[33];
    char text_b[33];
} robot_action_t;

esp_err_t robot_action_queue_init(void);
esp_err_t robot_action_queue_submit(const robot_action_t *action, uint32_t timeout_ms);
void robot_action_queue_deinit(void);

#ifdef __cplusplus
}
#endif
