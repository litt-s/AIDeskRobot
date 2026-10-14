#include "robot_motion.h"

#include <stdbool.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Servo pins are configurable, but a stale sdkconfig.h would leave the macro
 * undefined and break the build, so fall back to the board default. */
#ifndef CONFIG_ROBOT_SERVO_X_PIN
#define CONFIG_ROBOT_SERVO_X_PIN 11
#endif
#ifndef CONFIG_ROBOT_SERVO_Y_PIN
#define CONFIG_ROBOT_SERVO_Y_PIN 12
#endif

#define ACTION_QUEUE_DEPTH 16
#define MOTION_EXIT_BIT BIT0
#define SERVO_CENTER_X 90
#define SERVO_CENTER_Y 90
#define SERVO_MIN_X 50
#define SERVO_MAX_X 130
#define SERVO_MIN_Y 70
#define SERVO_MAX_Y 110
#define SERVO_STEP_DELAY_MS 10
#define RANDOM_ANIMATION_INTERVAL_MS 10000

typedef struct {
    QueueHandle_t queue;
    EventGroupHandle_t events;
    TaskHandle_t task;
    int servo_x;
    int servo_y;
    bool running;
} motion_state_t;

static const char *TAG = "robot_motion";
static motion_state_t s_motion;

static uint32_t angle_to_duty(int angle)
{
    if (angle < 0) {
        angle = 0;
    } else if (angle > 180) {
        angle = 180;
    }
    uint32_t pulse_us = 500U + ((uint32_t)angle * 2000U) / 180U;
    return (pulse_us * ((1U << 14) - 1U)) / 20000U;
}

static void set_servo(ledc_channel_t channel, int angle)
{
#if CONFIG_ROBOT_ENABLE_SERVO
    uint32_t duty = angle_to_duty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
#else
    (void)channel;
    (void)angle;
#endif
}

static int clamp_angle(int angle, int minimum, int maximum)
{
    if (angle < minimum) {
        return minimum;
    }
    if (angle > maximum) {
        return maximum;
    }
    return angle;
}

static void move_servos(int target_x, int target_y, int step_delay_ms)
{
    target_x = clamp_angle(target_x, SERVO_MIN_X, SERVO_MAX_X);
    target_y = clamp_angle(target_y, SERVO_MIN_Y, SERVO_MAX_Y);
    while (s_motion.running &&
           (s_motion.servo_x != target_x || s_motion.servo_y != target_y)) {
        if (s_motion.servo_x < target_x) {
            ++s_motion.servo_x;
        } else if (s_motion.servo_x > target_x) {
            --s_motion.servo_x;
        }
        if (s_motion.servo_y < target_y) {
            ++s_motion.servo_y;
        } else if (s_motion.servo_y > target_y) {
            --s_motion.servo_y;
        }
        set_servo(LEDC_CHANNEL_0, s_motion.servo_x);
        set_servo(LEDC_CHANNEL_1, s_motion.servo_y);
        vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
    }
}

static void center(void)
{
    move_servos(SERVO_CENTER_X, SERVO_CENTER_Y, SERVO_STEP_DELAY_MS);
}

static bool begin_ui_animation(robot_animation_t animation)
{
    esp_err_t err = robot_ui_begin_animation(animation);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to start paired UI animation %d: %s",
                 (int)animation, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void wait_ui_animation(bool started, uint32_t timeout_ms)
{
    if (!started) {
        return;
    }
    esp_err_t err = robot_ui_wait_animation(timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "paired UI animation timed out: %s",
                 esp_err_to_name(err));
    }
}

static void move_relative(int offset_x, int offset_y, int step_delay_ms)
{
    move_servos(s_motion.servo_x + offset_x,
                s_motion.servo_y + offset_y, step_delay_ms);
}

static void head_nod(int delay_ms)
{
    for (int i = 0; i < 3; ++i) {
        move_servos(SERVO_CENTER_X, SERVO_MAX_Y, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        move_servos(SERVO_CENTER_X, SERVO_MIN_Y, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    center();
}

static void head_shake(int delay_ms)
{
    for (int i = 0; i < 2; ++i) {
        move_servos(SERVO_MIN_X + 20, SERVO_CENTER_Y, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        move_servos(SERVO_MAX_X - 20, SERVO_CENTER_Y, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        move_servos(SERVO_MIN_X + 20, SERVO_CENTER_Y, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    center();
}

static void head_roll(void)
{
    center();
    move_relative(0, 17, SERVO_STEP_DELAY_MS);
    move_relative(40, -12, SERVO_STEP_DELAY_MS);
    move_relative(-40, -12, SERVO_STEP_DELAY_MS);
    move_relative(-40, 12, SERVO_STEP_DELAY_MS);
    move_relative(40, 12, SERVO_STEP_DELAY_MS);
    move_relative(-40, -12, SERVO_STEP_DELAY_MS);
    move_relative(40, -12, SERVO_STEP_DELAY_MS);
    move_relative(40, 12, SERVO_STEP_DELAY_MS);
    move_relative(-40, 12, SERVO_STEP_DELAY_MS);
    center();
}

static void execute_expression_motion(robot_expression_t expression)
{
    switch (expression) {
        case ROBOT_EXPRESSION_HAPPY:
            move_relative(0, -15, 10);
            vTaskDelay(pdMS_TO_TICKS(700));
            break;
        case ROBOT_EXPRESSION_SAD:
            move_relative(0, 20, 1);
            move_relative(0, -20, 1);
            break;
        case ROBOT_EXPRESSION_ANGRY:
            head_shake(150);
            return;
        case ROBOT_EXPRESSION_SURPRISED:
            move_relative(0, -12, 10);
            break;
        case ROBOT_EXPRESSION_THINKING:
            move_relative(-10, 5, 10);
            break;
        case ROBOT_EXPRESSION_AWKWARD:
            move_relative(10, -5, 1);
            break;
        case ROBOT_EXPRESSION_LAUGHING:
            move_relative(0, -15, 10);
            break;
        case ROBOT_EXPRESSION_FUNNY:
            move_relative(10, 5, 10);
            break;
        case ROBOT_EXPRESSION_LOVING:
            head_roll();
            return;
        case ROBOT_EXPRESSION_EMBARRASSED:
            move_relative(0, 10, 10);
            break;
        case ROBOT_EXPRESSION_SHOCKED:
            move_relative(0, -15, 10);
            break;
        case ROBOT_EXPRESSION_COOL:
            move_relative(0, -5, 10);
            head_nod(150);
            return;
        case ROBOT_EXPRESSION_RELAXED:
            move_relative(0, 5, 10);
            break;
        case ROBOT_EXPRESSION_DELICIOUS:
            head_nod(150);
            move_relative(5, 0, 10);
            move_relative(-5, 0, 10);
            move_relative(5, 0, 10);
            break;
        case ROBOT_EXPRESSION_KISSY:
            move_relative(0, -10, 10);
            break;
        case ROBOT_EXPRESSION_CONFIDENT:
            move_relative(0, -10, 10);
            head_nod(150);
            return;
        case ROBOT_EXPRESSION_SILLY:
            move_relative(10, 0, 10);
            head_shake(150);
            return;
        default:
            return;
    }
    vTaskDelay(pdMS_TO_TICKS(700));
    center();
}

static void execute(const robot_action_t *action);

static void execute_random(void)
{
    uint32_t value = esp_random() % 100;
    robot_action_t action = {0};
    if (value < 60) {
        action.type = ROBOT_ACTION_BLINK;
    } else if (value < 78) {
        action.type = ROBOT_ACTION_LOOK_LEFT;
    } else if (value < 96) {
        action.type = ROBOT_ACTION_LOOK_RIGHT;
    } else if (value < 97) {
        action.type = ROBOT_ACTION_ROLL;
    } else {
        action.type = ROBOT_ACTION_EXPRESSION;
        action.expression = ROBOT_EXPRESSION_CONFUSED;
    }
    ESP_LOGI(TAG, "original random animation: value=%lu action=%d",
             (unsigned long)value, (int)action.type);
    execute(&action);
}

static robot_animation_t animation_for_expression(robot_expression_t expression)
{
    switch (expression) {
        case ROBOT_EXPRESSION_HAPPY: return ROBOT_ANIMATION_HAPPY;
        case ROBOT_EXPRESSION_SAD: return ROBOT_ANIMATION_SAD;
        case ROBOT_EXPRESSION_ANGRY: return ROBOT_ANIMATION_ANGER;
        case ROBOT_EXPRESSION_SURPRISED: return ROBOT_ANIMATION_SURPRISE;
        case ROBOT_EXPRESSION_THINKING: return ROBOT_ANIMATION_THINKING;
        case ROBOT_EXPRESSION_CONFUSED: return ROBOT_ANIMATION_CONFUSED;
        case ROBOT_EXPRESSION_AWKWARD: return ROBOT_ANIMATION_AWKWARD;
        case ROBOT_EXPRESSION_CRY: return ROBOT_ANIMATION_CRY;
        case ROBOT_EXPRESSION_LAUGHING: return ROBOT_ANIMATION_LAUGHING;
        case ROBOT_EXPRESSION_FUNNY: return ROBOT_ANIMATION_FUNNY;
        case ROBOT_EXPRESSION_LOVING: return ROBOT_ANIMATION_LOVING;
        case ROBOT_EXPRESSION_EMBARRASSED:
            return ROBOT_ANIMATION_EMBARRASSED;
        case ROBOT_EXPRESSION_SHOCKED: return ROBOT_ANIMATION_SHOCKED;
        case ROBOT_EXPRESSION_COOL: return ROBOT_ANIMATION_COOL;
        case ROBOT_EXPRESSION_RELAXED: return ROBOT_ANIMATION_RELAXED;
        case ROBOT_EXPRESSION_DELICIOUS: return ROBOT_ANIMATION_DELICIOUS;
        case ROBOT_EXPRESSION_KISSY: return ROBOT_ANIMATION_KISSY;
        case ROBOT_EXPRESSION_CONFIDENT: return ROBOT_ANIMATION_CONFIDENT;
        case ROBOT_EXPRESSION_SILLY: return ROBOT_ANIMATION_SILLY;
        case ROBOT_EXPRESSION_SLEEP: return ROBOT_ANIMATION_SLEEP;
        case ROBOT_EXPRESSION_NEUTRAL:
        default: return ROBOT_ANIMATION_BLINK;
    }
}

static void execute(const robot_action_t *action)
{
    switch (action->type) {
        case ROBOT_ACTION_CENTER:
            center();
            break;
        case ROBOT_ACTION_LOOK_LEFT:
            move_servos(SERVO_MIN_X, s_motion.servo_y,
                        SERVO_STEP_DELAY_MS);
            wait_ui_animation(
                begin_ui_animation(ROBOT_ANIMATION_LOOK_LEFT), 2000);
            center();
            break;
        case ROBOT_ACTION_LOOK_RIGHT:
            move_servos(SERVO_MAX_X, s_motion.servo_y,
                        SERVO_STEP_DELAY_MS);
            wait_ui_animation(
                begin_ui_animation(ROBOT_ANIMATION_LOOK_RIGHT), 2000);
            center();
            break;
        case ROBOT_ACTION_LOOK_UP:
            robot_ui_play_animation(ROBOT_ANIMATION_LOOK_UP);
            move_servos(s_motion.servo_x, SERVO_MIN_Y,
                        SERVO_STEP_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(350));
            center();
            break;
        case ROBOT_ACTION_LOOK_DOWN:
            robot_ui_play_animation(ROBOT_ANIMATION_LOOK_DOWN);
            move_servos(s_motion.servo_x, SERVO_MAX_Y,
                        SERVO_STEP_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(350));
            center();
            break;
        case ROBOT_ACTION_NOD:
        {
            bool ui_started = begin_ui_animation(ROBOT_ANIMATION_HEAD_NOD);
            head_nod(150);
            wait_ui_animation(ui_started, 2500);
            break;
        }
        case ROBOT_ACTION_SHAKE:
        {
            bool ui_started = begin_ui_animation(ROBOT_ANIMATION_HEAD_SHAKE);
            head_shake(150);
            wait_ui_animation(ui_started, 2500);
            break;
        }
        case ROBOT_ACTION_ROLL:
        {
            bool ui_started = begin_ui_animation(ROBOT_ANIMATION_HEAD_ROLL);
            head_roll();
            wait_ui_animation(ui_started, 2500);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
        case ROBOT_ACTION_DANCE:
            robot_ui_play_animation(ROBOT_ANIMATION_FUNNY);
            for (int i = 0; i < 3; ++i) {
                move_servos(55, 75, 3);
                move_servos(125, 105, 3);
                move_servos(55, 105, 3);
                move_servos(125, 75, 3);
            }
            center();
            break;
        case ROBOT_ACTION_BLINK:
        {
            bool ui_started = begin_ui_animation(ROBOT_ANIMATION_BLINK);
            bool moved_head = false;
            if ((esp_random() % 100) < 30) {
                int direction = (int)(esp_random() % 6);
                static const int8_t offsets[6][2] = {
                    {-25, -25}, {25, 25}, {0, 25},
                    {0, -25}, {-25, 25}, {25, -25},
                };
                move_relative(offsets[direction][0],
                              offsets[direction][1], SERVO_STEP_DELAY_MS);
                moved_head = true;
            }
            wait_ui_animation(ui_started, 1500);
            if (moved_head) {
                center();
            }
            break;
        }
        case ROBOT_ACTION_WAKEUP:
            robot_ui_play_animation(ROBOT_ANIMATION_WAKEUP);
            center();
            break;
        case ROBOT_ACTION_SLEEP:
            robot_ui_play_animation(ROBOT_ANIMATION_SLEEP);
            center();
            move_relative(0, 25, SERVO_STEP_DELAY_MS);
            break;
        case ROBOT_ACTION_RANDOM:
            execute_random();
            break;
        case ROBOT_ACTION_EXPRESSION:
            if (action->expression == ROBOT_EXPRESSION_NEUTRAL) {
                robot_ui_show_expression(ROBOT_EXPRESSION_NEUTRAL);
            } else {
                bool ui_started = begin_ui_animation(
                    animation_for_expression(action->expression));
                execute_expression_motion(action->expression);
                wait_ui_animation(ui_started, 3000);
            }
            break;
        case ROBOT_ACTION_SHOW_PROVISIONING:
            robot_ui_show_provisioning(action->text_a, action->text_b);
            break;
        case ROBOT_ACTION_VOLUME_DELTA:
            ESP_LOGI(TAG, "volume delta=%d", action->value);
            break;
        case ROBOT_ACTION_STOP:
            center();
            s_motion.running = false;
            break;
        default:
            break;
    }
}

static void motion_task(void *arg)
{
    (void)arg;
    TickType_t last_random_time = xTaskGetTickCount();
    while (s_motion.running) {
        robot_action_t action;
        if (xQueueReceive(s_motion.queue, &action, pdMS_TO_TICKS(100)) == pdTRUE) {
            execute(&action);
        }
        TickType_t now = xTaskGetTickCount();
        if (!robot_ui_idle_animation_enabled()) {
            last_random_time = now;
        } else if ((now - last_random_time) >=
                   pdMS_TO_TICKS(RANDOM_ANIMATION_INTERVAL_MS)) {
            last_random_time = now;
            execute_random();
        }
    }
    xEventGroupSetBits(s_motion.events, MOTION_EXIT_BIT);
    vTaskDelete(NULL);
}

esp_err_t robot_action_queue_init(void)
{
    if (s_motion.running) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_motion, 0, sizeof(s_motion));
    s_motion.servo_x = SERVO_CENTER_X;
    s_motion.servo_y = SERVO_CENTER_Y;
    s_motion.queue = xQueueCreate(ACTION_QUEUE_DEPTH, sizeof(robot_action_t));
    s_motion.events = xEventGroupCreate();
    if (!s_motion.queue || !s_motion.events) {
        robot_action_queue_deinit();
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_ROBOT_ENABLE_SERVO
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_timer_config(&timer));
    ledc_channel_config_t horizontal = {
        .gpio_num = CONFIG_ROBOT_SERVO_X_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = angle_to_duty(90),
        .hpoint = 0,
    };
    ledc_channel_config_t vertical = horizontal;
    vertical.gpio_num = CONFIG_ROBOT_SERVO_Y_PIN;
    vertical.channel = LEDC_CHANNEL_1;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_channel_config(&horizontal));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_channel_config(&vertical));
    ESP_LOGI(TAG, "servo PWM 50Hz: horizontal GPIO%d, vertical GPIO%d",
             CONFIG_ROBOT_SERVO_X_PIN, CONFIG_ROBOT_SERVO_Y_PIN);
#if CONFIG_ROBOT_SERVO_SELFTEST
    /*
     * Bring-up aid: sweep each channel once, before Wi-Fi starts, so a silent
     * servo can be told apart from a firmware or connection problem.
     */
    ESP_LOGI(TAG, "servo self-test: horizontal channel");
    for (int i = 0; i < 2; ++i) {
        for (int angle = SERVO_MIN_X; angle <= SERVO_MAX_X; angle += 5) {
            set_servo(LEDC_CHANNEL_0, angle);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        for (int angle = SERVO_MAX_X; angle >= SERVO_MIN_X; angle -= 5) {
            set_servo(LEDC_CHANNEL_0, angle);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
    ESP_LOGI(TAG, "servo self-test: vertical channel");
    for (int i = 0; i < 2; ++i) {
        for (int angle = SERVO_MIN_Y; angle <= SERVO_MAX_Y; angle += 5) {
            set_servo(LEDC_CHANNEL_1, angle);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        for (int angle = SERVO_MAX_Y; angle >= SERVO_MIN_Y; angle -= 5) {
            set_servo(LEDC_CHANNEL_1, angle);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
    set_servo(LEDC_CHANNEL_0, SERVO_CENTER_X);
    set_servo(LEDC_CHANNEL_1, SERVO_CENTER_Y);
    ESP_LOGI(TAG, "servo self-test: done");
#endif
#endif
    s_motion.running = true;
    if (xTaskCreate(motion_task, "robot_actions", 4096, NULL, 4,
                    &s_motion.task) != pdPASS) {
        robot_action_queue_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t robot_action_queue_submit(const robot_action_t *action, uint32_t timeout_ms)
{
    if (!action) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_motion.running || !s_motion.queue) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t timeout = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return xQueueSend(s_motion.queue, action, timeout) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

void robot_action_queue_deinit(void)
{
    if (s_motion.running) {
        s_motion.running = false;
        if (s_motion.queue) {
            xQueueReset(s_motion.queue);
        }
        if (s_motion.events) {
            xEventGroupWaitBits(s_motion.events, MOTION_EXIT_BIT, pdFALSE,
                                pdTRUE, pdMS_TO_TICKS(3000));
        }
    }
#if CONFIG_ROBOT_ENABLE_SERVO
    set_servo(LEDC_CHANNEL_0, SERVO_CENTER_X);
    set_servo(LEDC_CHANNEL_1, SERVO_CENTER_Y);
    vTaskDelay(pdMS_TO_TICKS(50));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
#endif
    if (s_motion.queue) {
        vQueueDelete(s_motion.queue);
    }
    if (s_motion.events) {
        vEventGroupDelete(s_motion.events);
    }
    memset(&s_motion, 0, sizeof(s_motion));
}
