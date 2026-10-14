#include "robot_wakeup.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"

#define DETECT_TASK_EXIT_BIT BIT0
#define WAKE_COMMAND_ID 1
#define WAKE_COMMAND_PINYIN "xiao fei xiao fei"
#define WAKE_DETECT_DURATION_MS 3000
#define WAKE_DETECT_THRESHOLD 0.20f
#define WAKE_FRAME_QUEUE_DEPTH 8

typedef struct {
    srmodel_list_t *models;
    const esp_mn_iface_t *multinet;
    model_iface_data_t *model_data;
    robot_wakeup_fn callback;
    void *callback_ctx;
    int16_t *feed_buffer;
    int16_t *detect_buffer;
    size_t feed_size;
    size_t buffered;
    QueueHandle_t frame_queue;
    EventGroupHandle_t events;
    TaskHandle_t task;
    volatile bool armed;
    volatile bool destroying;
} wakeup_state_t;

static const char *TAG = "robot_wakeup";
static wakeup_state_t s_wakeup;

static void detect_task(void *arg)
{
    (void)arg;
    while (!s_wakeup.destroying) {
        if (!s_wakeup.armed) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (xQueueReceive(s_wakeup.frame_queue, s_wakeup.detect_buffer,
                          pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!s_wakeup.armed) {
            continue;
        }

        esp_mn_state_t state =
            s_wakeup.multinet->detect(s_wakeup.model_data,
                                      s_wakeup.detect_buffer);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *results =
                s_wakeup.multinet->get_results(s_wakeup.model_data);
            bool wake_detected = false;
            if (results) {
                for (int i = 0; i < results->num; ++i) {
                    if (results->command_id[i] == WAKE_COMMAND_ID) {
                        ESP_LOGI(TAG,
                                 "offline wake word detected: 小飞小飞 "
                                 "(prob=%.3f)",
                                 (double)results->prob[i]);
                        wake_detected = true;
                        break;
                    }
                }
            }
            s_wakeup.multinet->clean(s_wakeup.model_data);
            if (wake_detected) {
                s_wakeup.armed = false;
                xQueueReset(s_wakeup.frame_queue);
                if (s_wakeup.callback) {
                    s_wakeup.callback(s_wakeup.callback_ctx);
                }
            }
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            s_wakeup.multinet->clean(s_wakeup.model_data);
        }
    }
    xEventGroupSetBits(s_wakeup.events, DETECT_TASK_EXIT_BIT);
    vTaskDelete(NULL);
}

esp_err_t robot_wakeup_init(robot_wakeup_fn callback, void *ctx)
{
    if (!callback || s_wakeup.model_data) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_wakeup, 0, sizeof(s_wakeup));
    s_wakeup.callback = callback;
    s_wakeup.callback_ctx = ctx;
    s_wakeup.models = esp_srmodel_init("model");
    if (!s_wakeup.models || s_wakeup.models->num <= 0) {
        robot_wakeup_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    char *model_name =
        esp_srmodel_filter(s_wakeup.models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!model_name) {
        model_name = esp_srmodel_filter(s_wakeup.models, ESP_MN_PREFIX, NULL);
    }
    if (!model_name) {
        robot_wakeup_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "using custom wake model: %s", model_name);

    s_wakeup.multinet = esp_mn_handle_from_name(model_name);
    if (!s_wakeup.multinet) {
        robot_wakeup_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    s_wakeup.model_data =
        s_wakeup.multinet->create(model_name, WAKE_DETECT_DURATION_MS);
    if (!s_wakeup.model_data) {
        robot_wakeup_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_wakeup.multinet->set_det_threshold(s_wakeup.model_data,
                                         WAKE_DETECT_THRESHOLD);

    esp_err_t err =
        esp_mn_commands_alloc(s_wakeup.multinet, s_wakeup.model_data);
    if (err == ESP_OK) {
        err = esp_mn_commands_add(WAKE_COMMAND_ID, WAKE_COMMAND_PINYIN);
    }
    if (err != ESP_OK || esp_mn_commands_update() != NULL) {
        ESP_LOGE(TAG, "unable to activate wake phrase: %s",
                 WAKE_COMMAND_PINYIN);
        robot_wakeup_deinit();
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    s_wakeup.multinet->print_active_speech_commands(s_wakeup.model_data);

    s_wakeup.feed_size =
        (size_t)s_wakeup.multinet->get_samp_chunksize(s_wakeup.model_data);
    if (s_wakeup.feed_size == 0) {
        robot_wakeup_deinit();
        return ESP_ERR_INVALID_SIZE;
    }
    s_wakeup.feed_buffer =
        malloc(s_wakeup.feed_size * sizeof(*s_wakeup.feed_buffer));
    s_wakeup.detect_buffer =
        malloc(s_wakeup.feed_size * sizeof(*s_wakeup.detect_buffer));
    s_wakeup.frame_queue =
        xQueueCreate(WAKE_FRAME_QUEUE_DEPTH,
                     s_wakeup.feed_size * sizeof(*s_wakeup.feed_buffer));
    s_wakeup.events = xEventGroupCreate();
    if (!s_wakeup.feed_buffer || !s_wakeup.detect_buffer ||
        !s_wakeup.frame_queue || !s_wakeup.events) {
        robot_wakeup_deinit();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(detect_task, "wake_detect", 6144, NULL, 4,
                                &s_wakeup.task, 1) != pdPASS) {
        robot_wakeup_deinit();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "custom wake phrase ready: 小飞小飞");
    return ESP_OK;
}

esp_err_t robot_wakeup_start(void)
{
    if (!s_wakeup.model_data || !s_wakeup.frame_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    s_wakeup.armed = false;
    s_wakeup.buffered = 0;
    xQueueReset(s_wakeup.frame_queue);
    s_wakeup.multinet->clean(s_wakeup.model_data);
    s_wakeup.armed = true;
    return ESP_OK;
}

void robot_wakeup_stop(void)
{
    s_wakeup.armed = false;
    s_wakeup.buffered = 0;
    if (s_wakeup.frame_queue) {
        xQueueReset(s_wakeup.frame_queue);
    }
}

void robot_wakeup_deinit(void)
{
    s_wakeup.armed = false;
    s_wakeup.destroying = true;
    if (s_wakeup.events && s_wakeup.task) {
        xEventGroupWaitBits(s_wakeup.events, DETECT_TASK_EXIT_BIT, pdFALSE,
                            pdTRUE, pdMS_TO_TICKS(500));
    }
    if (s_wakeup.model_data) {
        esp_mn_commands_free();
    }
    if (s_wakeup.multinet && s_wakeup.model_data) {
        s_wakeup.multinet->destroy(s_wakeup.model_data);
    }
    if (s_wakeup.models) {
        esp_srmodel_deinit(s_wakeup.models);
    }
    free(s_wakeup.feed_buffer);
    free(s_wakeup.detect_buffer);
    if (s_wakeup.frame_queue) {
        vQueueDelete(s_wakeup.frame_queue);
    }
    if (s_wakeup.events) {
        vEventGroupDelete(s_wakeup.events);
    }
    memset(&s_wakeup, 0, sizeof(s_wakeup));
}

esp_err_t robot_wakeup_feed_pcm(const int16_t *samples,
                                size_t sample_count,
                                uint64_t capture_ms)
{
    (void)capture_ms;
    if (!samples || !sample_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_wakeup.model_data || !s_wakeup.armed) {
        return ESP_ERR_INVALID_STATE;
    }
    while (sample_count) {
        size_t available = s_wakeup.feed_size - s_wakeup.buffered;
        size_t copy_count = sample_count < available ? sample_count : available;
        memcpy(s_wakeup.feed_buffer + s_wakeup.buffered, samples,
               copy_count * sizeof(*samples));
        s_wakeup.buffered += copy_count;
        samples += copy_count;
        sample_count -= copy_count;
        if (s_wakeup.buffered == s_wakeup.feed_size) {
            if (xQueueSend(s_wakeup.frame_queue, s_wakeup.feed_buffer, 0) !=
                pdTRUE) {
                s_wakeup.buffered = 0;
                return ESP_ERR_TIMEOUT;
            }
            s_wakeup.buffered = 0;
        }
    }
    return ESP_OK;
}
