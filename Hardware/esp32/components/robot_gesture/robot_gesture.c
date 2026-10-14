#include "robot_gesture.h"

#include <stdbool.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "robot_ui.h"

#define PAJ_ADDRESS 0x73
#define GESTURE_EXIT_BIT BIT0
#define GESTURE_COOLDOWN_MS 900
#define GESTURE_REARM_QUIET_SAMPLES 4
#define GESTURE_STARTUP_GUARD_MS 1200
#define GESTURE_SEQUENCE_TIMEOUT_MS 4000
#define GESTURE_SEQUENCE_MIN_COUNT 3

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device;
    robot_gesture_fn callback;
    void *callback_ctx;
    EventGroupHandle_t events;
    TaskHandle_t task;
    volatile bool running;
    robot_gesture_t last_sequence_gesture;
    TickType_t last_sequence_tick;
    TickType_t last_accept_tick;
    TickType_t start_tick;
    uint8_t sequence_count;
    uint8_t quiet_samples;
    bool armed;
} gesture_state_t;

static const char *TAG = "robot_gesture";
/*
 * PAJ7620U2 vendor initialization table.  The previous abbreviated setup
 * only re-enabled interrupts and relied on power-on register contents; that
 * is not reliable after brownouts or a warm ESP32 reset.
 */
static const uint8_t s_init_sequence[][2] = {
    // Bank 0 基础配置
    {0xEF, 0x00},
    {0x32, 0x29}, {0x33, 0x01}, {0x34, 0x00}, {0x35, 0x01},
    {0x36, 0x00}, {0x37, 0x07}, {0x38, 0x17}, {0x39, 0x06},
    {0x3A, 0x12}, {0x3F, 0x00}, {0x40, 0x02}, {0x41, 0xFF},
    {0x42, 0x01}, {0x46, 0x2D}, {0x47, 0x0F}, {0x48, 0x3C},
    {0x49, 0x00}, {0x4A, 0x1E}, {0x4B, 0x00}, {0x4C, 0x20},
    {0x4D, 0x00}, {0x4E, 0x1A}, {0x4F, 0x14}, {0x50, 0x00},
    {0x51, 0x10}, {0x52, 0x00}, {0x5C, 0x02}, {0x5D, 0x00},
    {0x5E, 0x10}, {0x5F, 0x3F}, {0x60, 0x27}, {0x61, 0x28},
    {0x62, 0x00}, {0x63, 0x03}, {0x64, 0xF7}, {0x65, 0x03},
    {0x66, 0xD9}, {0x67, 0x03}, {0x68, 0x01}, {0x69, 0xC8},
    {0x6A, 0x40}, {0x6D, 0x04}, {0x6E, 0x00}, {0x6F, 0x00},
    {0x70, 0x80}, {0x71, 0x00}, {0x72, 0x00}, {0x73, 0x00},
    {0x74, 0xF0}, {0x75, 0x00}, {0x80, 0x42}, {0x81, 0x44},
    {0x82, 0x04}, {0x83, 0x20}, {0x84, 0x20}, {0x85, 0x00},
    {0x86, 0x10}, {0x87, 0x00}, {0x88, 0x05}, {0x89, 0x18},
    {0x8A, 0x10}, {0x8B, 0x01}, {0x8C, 0x37}, {0x8D, 0x00},
    {0x8E, 0xF0}, {0x8F, 0x81}, {0x90, 0x06}, {0x91, 0x06},
    {0x92, 0x1E}, {0x93, 0x0D}, {0x94, 0x0A}, {0x95, 0x0A},
    {0x96, 0x0C}, {0x97, 0x05}, {0x98, 0x0A}, {0x99, 0x41},
    {0x9A, 0x14}, {0x9B, 0x0A}, {0x9C, 0x3F}, {0x9D, 0x33},
    {0x9E, 0xAE}, {0x9F, 0xF9}, {0xA0, 0x48}, {0xA1, 0x13},
    {0xA2, 0x10}, {0xA3, 0x08}, {0xA4, 0x30}, {0xA5, 0x19},
    {0xA6, 0x10}, {0xA7, 0x08}, {0xA8, 0x24}, {0xA9, 0x04},
    {0xAA, 0x1E}, {0xAB, 0x1E}, {0xCC, 0x19}, {0xCD, 0x0B},
    {0xCE, 0x13}, {0xCF, 0x64}, {0xD0, 0x21}, {0xD1, 0x0F},
    {0xD2, 0x88}, {0xE0, 0x01}, {0xE1, 0x04}, {0xE2, 0x41},
    {0xE3, 0xD6}, {0xE4, 0x00}, {0xE5, 0x0C}, {0xE6, 0x0A},
    {0xE7, 0x00}, {0xE8, 0x00}, {0xE9, 0x00}, {0xEE, 0x07},
    {0xEF, 0x01}, {0x00, 0x1E}, {0x01, 0x1E}, {0x02, 0x0F},
    {0x03, 0x10}, {0x04, 0x02}, {0x05, 0x00}, {0x06, 0xB0},
    {0x07, 0x04}, {0x08, 0x0D}, {0x09, 0x0E}, {0x0A, 0x9C},
    {0x0B, 0x04}, {0x0C, 0x05}, {0x0D, 0x0F}, {0x0E, 0x02},
    {0x0F, 0x12}, {0x10, 0x02}, {0x11, 0x02}, {0x12, 0x00},
    {0x13, 0x01}, {0x14, 0x05}, {0x15, 0x07}, {0x16, 0x05},
    {0x17, 0x07}, {0x18, 0x01}, {0x19, 0x04}, {0x1A, 0x05},
    {0x1B, 0x0C}, {0x1C, 0x2A}, {0x1D, 0x01}, {0x1E, 0x00},
    {0x21, 0x00}, {0x22, 0x00}, {0x23, 0x00}, {0x25, 0x01},
    {0x26, 0x00}, {0x27, 0x39}, {0x28, 0x7F}, {0x29, 0x08},
    {0x30, 0x03}, {0x31, 0x00}, {0x32, 0x1A}, {0x33, 0x1A},
    {0x34, 0x07}, {0x35, 0x07}, {0x36, 0x01}, {0x37, 0xFF},
    {0x38, 0x36}, {0x39, 0x07}, {0x3A, 0x00}, {0x3E, 0xFF},
    {0x3F, 0x00}, {0x40, 0x77}, {0x41, 0x40}, {0x42, 0x00},
    {0x43, 0x30}, {0x44, 0xA0}, {0x45, 0x5C}, {0x46, 0x00},
    {0x47, 0x00}, {0x48, 0x58}, {0x4A, 0x1E}, {0x4B, 0x1E},
    {0x4C, 0x00}, {0x4D, 0x00}, {0x4E, 0xA0}, {0x4F, 0x80},
    {0x50, 0x00}, {0x51, 0x00}, {0x52, 0x00}, {0x53, 0x00},
    {0x54, 0x00}, {0x57, 0x80}, {0x59, 0x10}, {0x5A, 0x08},
    {0x5B, 0x94}, {0x5C, 0xE8}, {0x5D, 0x08}, {0x5E, 0x3D},
    {0x5F, 0x99}, {0x60, 0x45}, {0x61, 0x40}, {0x63, 0x2D},
    {0x64, 0x02}, {0x65, 0x96}, {0x66, 0x00}, {0x67, 0x97},
    {0x68, 0x01}, {0x69, 0xCD}, {0x6A, 0x01}, {0x6B, 0xB0},
    {0x6C, 0x04}, {0x6D, 0x2C}, {0x6E, 0x01}, {0x6F, 0x32},
    {0x71, 0x00}, {0x72, 0x01}, {0x73, 0x35}, {0x74, 0x00},
    {0x75, 0x33}, {0x76, 0x31}, {0x77, 0x01}, {0x7C, 0x84},
    {0x7D, 0x03}, {0x7E, 0x01}
};
static gesture_state_t s_gesture;

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    if (!robot_ui_i2c_take(150)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(s_gesture.device, payload,
                                        sizeof(payload),
                                        pdMS_TO_TICKS(100));
    robot_ui_i2c_give();
    return err;
}

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    if (!robot_ui_i2c_take(150)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit_receive(
        s_gesture.device, &reg, 1, value, 1, pdMS_TO_TICKS(100));
    robot_ui_i2c_give();
    return err;
}

static esp_err_t sensor_initialize(void)
{
    ESP_LOGI(TAG, "loading PAJ7620U2 vendor register table (%u entries)",
             (unsigned)(sizeof(s_init_sequence) / sizeof(s_init_sequence[0])));
    for (size_t i = 0;
         i < sizeof(s_init_sequence) / sizeof(s_init_sequence[0]); ++i) {
        esp_err_t err = write_register(s_init_sequence[i][0],
                                       s_init_sequence[i][1]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PAJ init failed at reg 0x%02x: %s",
                     s_init_sequence[i][0], esp_err_to_name(err));
            return err;
        }
        if (s_init_sequence[i][0] == 0xef) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if ((i % 16) == 15) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return ESP_OK;
}

static esp_err_t sensor_enable(void)
{
    /* Modules retain the vendor gesture engine in ROM. These writes select
       bank 0, enable all nine interrupt flags, and clear stale detections. */
    static const uint8_t sequence[][2] = {
        {0xef, 0x00}, {0x32, 0x29}, {0x40, 0x02},
        {0x41, 0xff}, {0x42, 0x01}, {0x43, 0x00}, {0x44, 0x00},
        {0x46, 0x2d}, {0x47, 0x0f}, {0x48, 0x3c},
    };
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        esp_err_t err = write_register(sequence[i][0], sequence[i][1]);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

static bool decode(uint8_t primary, uint8_t secondary, robot_gesture_t *gesture)
{
    /*
     * The sensor PCB is mounted 90 degrees relative to the OLED on
     * DeskEmoji.  Convert PAJ7620 raw directions into directions as seen by
     * the user; this is the same orientation mapping as the original board.
     */
    if (primary & 0x08) {
        *gesture = ROBOT_GESTURE_RIGHT;
    } else if (primary & 0x04) {
        *gesture = ROBOT_GESTURE_LEFT;
    } else if (primary & 0x01) {
        *gesture = ROBOT_GESTURE_UP;
    } else if (primary & 0x02) {
        *gesture = ROBOT_GESTURE_DOWN;
    } else if (primary & 0x10) {
        *gesture = ROBOT_GESTURE_FORWARD;
    } else if (primary & 0x20) {
        *gesture = ROBOT_GESTURE_BACKWARD;
    } else if (primary & 0x40) {
        *gesture = ROBOT_GESTURE_CLOCKWISE;
    } else if (primary & 0x80) {
        *gesture = ROBOT_GESTURE_COUNTERCLOCKWISE;
    } else if (secondary & 0x01) {
        *gesture = ROBOT_GESTURE_WAVE;
    } else {
        return false;
    }
    return true;
}

static bool flags_are_unambiguous(uint8_t primary, uint8_t secondary)
{
    bool multiple_primary = primary != 0 &&
                            (primary & (uint8_t)(primary - 1)) != 0;
    bool primary_and_wave = primary != 0 && (secondary & 0x01) != 0;
    return !multiple_primary && !primary_and_wave;
}

static const char *gesture_name(robot_gesture_t gesture)
{
    switch (gesture) {
        case ROBOT_GESTURE_UP: return "up";
        case ROBOT_GESTURE_DOWN: return "down";
        case ROBOT_GESTURE_LEFT: return "left";
        case ROBOT_GESTURE_RIGHT: return "right";
        case ROBOT_GESTURE_FORWARD: return "forward";
        case ROBOT_GESTURE_BACKWARD: return "backward";
        case ROBOT_GESTURE_CLOCKWISE: return "clockwise";
        case ROBOT_GESTURE_COUNTERCLOCKWISE: return "counterclockwise";
        case ROBOT_GESTURE_WAVE: return "wave";
        case ROBOT_GESTURE_NOD: return "nod";
        case ROBOT_GESTURE_SHAKE: return "shake";
        default: return "unknown";
    }
}

static bool detect_direction_sequence(robot_gesture_t gesture,
                                      robot_gesture_t *combined)
{
    bool horizontal = gesture == ROBOT_GESTURE_LEFT ||
                      gesture == ROBOT_GESTURE_RIGHT;
    bool vertical = gesture == ROBOT_GESTURE_UP ||
                    gesture == ROBOT_GESTURE_DOWN;
    if (!horizontal && !vertical) {
        s_gesture.sequence_count = 0;
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    bool timed_out = s_gesture.sequence_count == 0 ||
        pdTICKS_TO_MS(now - s_gesture.last_sequence_tick) >
            GESTURE_SEQUENCE_TIMEOUT_MS;
    bool last_horizontal =
        s_gesture.last_sequence_gesture == ROBOT_GESTURE_LEFT ||
        s_gesture.last_sequence_gesture == ROBOT_GESTURE_RIGHT;
    bool same_axis = (horizontal && last_horizontal) ||
                     (vertical && !last_horizontal);
    bool alternating = same_axis &&
                       gesture != s_gesture.last_sequence_gesture;

    if (timed_out || !alternating) {
        s_gesture.sequence_count = 1;
    } else {
        ++s_gesture.sequence_count;
    }
    s_gesture.last_sequence_gesture = gesture;
    s_gesture.last_sequence_tick = now;

    if (s_gesture.sequence_count < GESTURE_SEQUENCE_MIN_COUNT) {
        return false;
    }
    *combined = horizontal ? ROBOT_GESTURE_SHAKE : ROBOT_GESTURE_NOD;
    s_gesture.sequence_count = 0;
    return true;
}

static void gesture_task(void *arg)
{
    (void)arg;
    unsigned consecutive_errors = 0;
    unsigned recovery_failures = 0;
    s_gesture.start_tick = xTaskGetTickCount();
    s_gesture.last_accept_tick = s_gesture.start_tick;
    s_gesture.quiet_samples = 0;
    s_gesture.armed = false;
    while (s_gesture.running) {
        uint8_t primary = 0;
        uint8_t secondary = 0;
        esp_err_t primary_err = read_register(0x43, &primary);
        esp_err_t secondary_err = primary_err == ESP_OK
                                      ? read_register(0x44, &secondary)
                                      : primary_err;
        if (primary_err == ESP_OK && secondary_err == ESP_OK) {
            consecutive_errors = 0;
            recovery_failures = 0;
            TickType_t now = xTaskGetTickCount();
            if (primary == 0 && (secondary & 0x01) == 0) {
                if (s_gesture.quiet_samples < GESTURE_REARM_QUIET_SAMPLES) {
                    ++s_gesture.quiet_samples;
                }
                if (s_gesture.quiet_samples >= GESTURE_REARM_QUIET_SAMPLES &&
                    pdTICKS_TO_MS(now - s_gesture.last_accept_tick) >=
                        GESTURE_COOLDOWN_MS) {
                    s_gesture.armed = true;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            /* Always clear latched flags, including rejected/noisy samples. */
            (void)write_register(0x43, 0x00);
            (void)write_register(0x44, 0x00);
            s_gesture.quiet_samples = 0;

            bool startup_guard =
                pdTICKS_TO_MS(now - s_gesture.start_tick) <
                    GESTURE_STARTUP_GUARD_MS;
            if (!s_gesture.armed || startup_guard ||
                !flags_are_unambiguous(primary, secondary)) {
                ESP_LOGD(TAG,
                         "gesture filtered: raw=%02x/%02x armed=%d startup=%d",
                         primary, secondary, s_gesture.armed, startup_guard);
                s_gesture.armed = false;
                s_gesture.last_accept_tick = now;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            s_gesture.armed = false;
            s_gesture.last_accept_tick = now;
            robot_gesture_t gesture;
            if (decode(primary, secondary, &gesture) && s_gesture.callback) {
                ESP_LOGI(TAG, "gesture detected: %s raw=%02x/%02x",
                         gesture_name(gesture), primary, secondary);
                s_gesture.callback(s_gesture.callback_ctx, gesture);
                robot_gesture_t combined;
                if (detect_direction_sequence(gesture, &combined)) {
                    ESP_LOGI(TAG, "gesture sequence detected: %s",
                             gesture_name(combined));
                    s_gesture.callback(s_gesture.callback_ctx, combined);
                }
            }
        } else if (++consecutive_errors >= 3) {
            ESP_LOGW(TAG,
                     "gesture I2C failed repeatedly; re-enabling sensor");
            esp_err_t recovery_err = sensor_enable();
            consecutive_errors = 0;
            if (recovery_err != ESP_OK) {
                ++recovery_failures;
                ESP_LOGW(TAG,
                         "gesture sensor recovery failed %u/3: %s",
                         recovery_failures, esp_err_to_name(recovery_err));
                if (recovery_failures >= 3) {
                    /*
                     * A missing/faulty PAJ7620 must not monopolize the shared
                     * I2C bus and starve the OLED.  Keep voice/UI operation
                     * healthy and retry the gesture sensor after reboot.
                     */
                    ESP_LOGE(TAG,
                             "gesture sensor disabled to protect shared I2C");
                    s_gesture.running = false;
                    break;
                }
            } else {
                recovery_failures = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    xEventGroupSetBits(s_gesture.events, GESTURE_EXIT_BIT);
    vTaskDelete(NULL);
}

esp_err_t robot_gesture_init(robot_gesture_fn callback, void *ctx)
{
    if (!callback || s_gesture.device) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_gesture, 0, sizeof(s_gesture));
    s_gesture.callback = callback;
    s_gesture.callback_ctx = ctx;
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_0, &s_gesture.bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shared I2C bus is unavailable");
        return err;
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PAJ_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_gesture.bus, &device_config,
                                    &s_gesture.device);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    err = sensor_initialize();
    if (err == ESP_OK) {
        err = sensor_enable();
    }
    if (err != ESP_OK) {
        robot_gesture_deinit();
        return err;
    }
    ESP_LOGI(TAG, "PAJ7620U2 ready: swipe/near/far/rotate/wave enabled");
    s_gesture.events = xEventGroupCreate();
    if (!s_gesture.events) {
        robot_gesture_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t robot_gesture_start(void)
{
    if (!s_gesture.device || s_gesture.running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_gesture.running = true;
    xEventGroupClearBits(s_gesture.events, GESTURE_EXIT_BIT);
    if (xTaskCreate(gesture_task, "gesture", 3072, NULL, 3,
                    &s_gesture.task) != pdPASS) {
        s_gesture.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void robot_gesture_stop(void)
{
    if (!s_gesture.running) {
        return;
    }
    s_gesture.running = false;
    xEventGroupWaitBits(s_gesture.events, GESTURE_EXIT_BIT, pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(500));
}

void robot_gesture_deinit(void)
{
    robot_gesture_stop();
    if (s_gesture.device) {
        i2c_master_bus_rm_device(s_gesture.device);
    }
    if (s_gesture.events) {
        vEventGroupDelete(s_gesture.events);
    }
    memset(&s_gesture, 0, sizeof(s_gesture));
}
