#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_EXPRESSION_NEUTRAL,
    ROBOT_EXPRESSION_HAPPY,
    ROBOT_EXPRESSION_SAD,
    ROBOT_EXPRESSION_ANGRY,
    ROBOT_EXPRESSION_SURPRISED,
    ROBOT_EXPRESSION_THINKING,
    ROBOT_EXPRESSION_CONFUSED,
    ROBOT_EXPRESSION_AWKWARD,
    ROBOT_EXPRESSION_CRY,
    ROBOT_EXPRESSION_LAUGHING,
    ROBOT_EXPRESSION_FUNNY,
    ROBOT_EXPRESSION_LOVING,
    ROBOT_EXPRESSION_EMBARRASSED,
    ROBOT_EXPRESSION_SHOCKED,
    ROBOT_EXPRESSION_COOL,
    ROBOT_EXPRESSION_RELAXED,
    ROBOT_EXPRESSION_DELICIOUS,
    ROBOT_EXPRESSION_KISSY,
    ROBOT_EXPRESSION_CONFIDENT,
    ROBOT_EXPRESSION_SILLY,
    ROBOT_EXPRESSION_SLEEP,
    ROBOT_EXPRESSION_COUNT,
} robot_expression_t;

/* Animation names mirror the original esp32-s3n16r8-emoji
 * EmojiController.  The implementation is adapted to the project's direct
 * SSD1306 framebuffer instead of depending on LVGL. */
typedef enum {
    ROBOT_ANIMATION_BLINK,
    ROBOT_ANIMATION_HAPPY,
    ROBOT_ANIMATION_SAD,
    ROBOT_ANIMATION_ANGER,
    ROBOT_ANIMATION_SURPRISE,
    ROBOT_ANIMATION_WAKEUP,
    ROBOT_ANIMATION_SLEEP,
    ROBOT_ANIMATION_LOOK_LEFT,
    ROBOT_ANIMATION_LOOK_RIGHT,
    ROBOT_ANIMATION_LOOK_UP,
    ROBOT_ANIMATION_LOOK_DOWN,
    ROBOT_ANIMATION_HEAD_NOD,
    ROBOT_ANIMATION_HEAD_SHAKE,
    ROBOT_ANIMATION_HEAD_ROLL,
    ROBOT_ANIMATION_CONFUSED,
    ROBOT_ANIMATION_AWKWARD,
    ROBOT_ANIMATION_CRY,
    ROBOT_ANIMATION_LAUGHING,
    ROBOT_ANIMATION_FUNNY,
    ROBOT_ANIMATION_LOVING,
    ROBOT_ANIMATION_EMBARRASSED,
    ROBOT_ANIMATION_SHOCKED,
    ROBOT_ANIMATION_THINKING,
    ROBOT_ANIMATION_COOL,
    ROBOT_ANIMATION_RELAXED,
    ROBOT_ANIMATION_DELICIOUS,
    ROBOT_ANIMATION_KISSY,
    ROBOT_ANIMATION_CONFIDENT,
    ROBOT_ANIMATION_SILLY,
    ROBOT_ANIMATION_RANDOM,
    ROBOT_ANIMATION_STOP,
} robot_animation_t;

esp_err_t robot_ui_init(void);
esp_err_t robot_ui_show_expression(robot_expression_t expression);
esp_err_t robot_ui_play_animation(robot_animation_t animation);
/* Start a tracked animation from the calling task, then wait for that exact
 * animation to finish. Used by robot_motion to keep eyes and servos paired. */
esp_err_t robot_ui_begin_animation(robot_animation_t animation);
esp_err_t robot_ui_wait_animation(uint32_t timeout_ms);
void robot_ui_set_idle_animation(bool enabled);
bool robot_ui_idle_animation_enabled(void);
/* Serialize access to the physical I2C bus shared by the OLED and the
 * PAJ7620 gesture sensor. */
bool robot_ui_i2c_take(uint32_t timeout_ms);
void robot_ui_i2c_give(void);
esp_err_t robot_ui_show_provisioning(const char *ssid, const char *url);
void robot_ui_deinit(void);

#ifdef __cplusplus
}
#endif
