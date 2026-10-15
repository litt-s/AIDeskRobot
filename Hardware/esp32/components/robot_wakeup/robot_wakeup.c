#include "robot_wakeup.h"

#include <stdbool.h>
#include <stdio.h>
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
#include "nvs.h"
#include "nvs_flash.h"

#define DETECT_TASK_EXIT_BIT BIT0
#define WAKE_COMMAND_ID 1
/*
 * MultiNet7 takes the phrase as written: hanzi goes through the model's own
 * grapheme-to-phoneme step, and the pinyin form below is what this firmware has
 * always shipped with.  Either way it is only the *default* - NVS (set from the
 * console or the setup page) can replace it at runtime.
 */
#define WAKE_PHRASE_DEFAULT "xiao fei xiao fei"
#define WAKE_PHRASE_MAX 64
#define WAKE_NVS_NAMESPACE "wake_cfg"
#define WAKE_NVS_KEY "phrase"
#define WAKE_NVS_LABEL_KEY "label"
/* 用户写的词只是给人看/给人听的，长度按 UTF-8 字节算够用就行。 */
#define WAKE_LABEL_MAX 40
#define WAKE_DETECT_DURATION_MS 3000
#define WAKE_DETECT_THRESHOLD 0.20f
#define WAKE_FRAME_QUEUE_DEPTH 8

typedef struct {
    srmodel_list_t *models;
    const esp_mn_iface_t *multinet;
    model_iface_data_t *model_data;
    robot_wakeup_fn callback;
    void *callback_ctx;
    /* Current phrase, as handed to MultiNet.  Empty only before init. */
    char phrase[WAKE_PHRASE_MAX];
    /* What the user called it (中文)，用于提示音；空表示没设过。 */
    char label[WAKE_LABEL_MAX];
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
                                 "offline wake word detected: %s "
                                 "(prob=%.3f)",
                                 s_wakeup.phrase,
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

/* ---------- wake phrase ---------- */

static void wake_load_phrase(char *out, size_t size)
{
    out[0] = '\0';
    nvs_handle_t nvs = 0;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t length = size;
    if (nvs_get_str(nvs, WAKE_NVS_KEY, out, &length) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(nvs);
}

static void wake_store_label(const char *label)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, WAKE_NVS_LABEL_KEY, label);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void wake_load_label(char *out, size_t size)
{
    out[0] = '\0';
    nvs_handle_t nvs = 0;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t length = size;
    if (nvs_get_str(nvs, WAKE_NVS_LABEL_KEY, out, &length) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(nvs);
}

static void wake_store_phrase(const char *phrase)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "unable to store the wake phrase: NVS unavailable");
        return;
    }
    esp_err_t err = nvs_set_str(nvs, WAKE_NVS_KEY, phrase);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to store the wake phrase: %s",
                 esp_err_to_name(err));
    }
    nvs_close(nvs);
}

/*
 * Install one phrase as the only command.  MultiNet does not fail on a phrase it
 * cannot parse - it reports it back - so this error list is what decides whether
 * the phrase is usable at all.
 */
static esp_err_t wake_apply_phrase(const char *phrase)
{
    if (esp_mn_commands_clear() != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_mn_commands_add(WAKE_COMMAND_ID, phrase);
    if (err != ESP_OK) {
        return err;
    }
    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors != NULL && errors->num > 0) {
        ESP_LOGW(TAG, "the model cannot use this phrase: %s",
                 (errors->phrases != NULL && errors->phrases[0] != NULL &&
                  errors->phrases[0]->string != NULL)
                     ? errors->phrases[0]->string
                     : phrase);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
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
        /* Silence here used to be the whole story: the wake word simply never
         * fired, while the app still logged "offline wake ready".  Say it. */
        ESP_LOGE(TAG,
                 "no wake model in the \"model\" partition - was "
                 "build/srmodels/srmodels.bin flashed?");
        robot_wakeup_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    char *model_name =
        esp_srmodel_filter(s_wakeup.models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!model_name) {
        model_name = esp_srmodel_filter(s_wakeup.models, ESP_MN_PREFIX, NULL);
    }
    if (!model_name) {
        ESP_LOGE(TAG,
                 "the model partition holds no MultiNet model (%s*), so the "
                 "wake word cannot be recognised",
                 ESP_MN_PREFIX);
        robot_wakeup_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "using custom wake model: %s", model_name);

    s_wakeup.multinet = esp_mn_handle_from_name(model_name);
    if (!s_wakeup.multinet) {
        ESP_LOGE(TAG, "no MultiNet handle for model %s", model_name);
        robot_wakeup_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    s_wakeup.model_data =
        s_wakeup.multinet->create(model_name, WAKE_DETECT_DURATION_MS);
    if (!s_wakeup.model_data) {
        ESP_LOGE(TAG, "unable to allocate wake model %s (out of PSRAM?)",
                 model_name);
        robot_wakeup_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_wakeup.multinet->set_det_threshold(s_wakeup.model_data,
                                         WAKE_DETECT_THRESHOLD);

    esp_err_t err =
        esp_mn_commands_alloc(s_wakeup.multinet, s_wakeup.model_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unable to prepare the wake phrase list: %s",
                 esp_err_to_name(err));
        robot_wakeup_deinit();
        return err;
    }
    /* A stored phrase wins; the built-in one is the fallback both for a fresh
     * device and for a stored phrase the current model cannot parse. */
    char stored[WAKE_PHRASE_MAX];
    wake_load_phrase(stored, sizeof(stored));
    const char *phrase = stored[0] != '\0' ? stored : WAKE_PHRASE_DEFAULT;
    if (wake_apply_phrase(phrase) != ESP_OK) {
        ESP_LOGW(TAG, "stored wake phrase rejected (%.32s); using the default",
                 phrase);
        phrase = WAKE_PHRASE_DEFAULT;
        if (wake_apply_phrase(phrase) != ESP_OK) {
            ESP_LOGE(TAG, "unable to activate wake phrase: %s", phrase);
            robot_wakeup_deinit();
            return ESP_ERR_INVALID_ARG;
        }
    }
    strlcpy(s_wakeup.phrase, phrase, sizeof(s_wakeup.phrase));
    wake_load_label(s_wakeup.label, sizeof(s_wakeup.label));
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
    ESP_LOGI(TAG, "custom wake phrase ready: %s", s_wakeup.phrase);
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

esp_err_t robot_wakeup_set_phrase(const char *phrase, char *error,
                                  size_t error_size)
{
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    if (phrase == NULL || phrase[0] == '\0') {
        if (error) {
            snprintf(error, error_size, "唤醒词不能为空");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(phrase) >= WAKE_PHRASE_MAX) {
        if (error) {
            snprintf(error, error_size, "唤醒词太长（最多 %d 字节）",
                     WAKE_PHRASE_MAX - 1);
        }
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * The Chinese MultiNet model wants pinyin syllables; hanzi are rejected by
     * the model itself ("MN_COMMAND: invalid command"), which is a dead end for
     * the user unless we say what to write instead.
     */
    for (const char *c = phrase; *c != '\0'; ++c) {
        if ((unsigned char)*c >= 0x80) {
            if (error) {
                snprintf(error, error_size,
                         "唤醒词要用拼音，例如 ni hao xiao zhi");
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_wakeup.model_data == NULL) {
        if (error) {
            snprintf(error, error_size, "唤醒引擎未就绪");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(phrase, s_wakeup.phrase) == 0) {
        return ESP_OK;
    }

    char previous[WAKE_PHRASE_MAX];
    strlcpy(previous, s_wakeup.phrase, sizeof(previous));
    const bool was_armed = s_wakeup.armed;
    /* Disarm while the command list is rebuilt: a detection racing the update
     * would call back into the app with a phrase that is on its way out. */
    s_wakeup.armed = false;

    esp_err_t err = wake_apply_phrase(phrase);
    if (err == ESP_OK) {
        strlcpy(s_wakeup.phrase, phrase, sizeof(s_wakeup.phrase));
        wake_store_phrase(phrase);
        ESP_LOGI(TAG, "wake phrase set to: %s", s_wakeup.phrase);
    } else {
        /* Never leave the robot deaf: put the previous phrase back. */
        if (wake_apply_phrase(previous) != ESP_OK) {
            ESP_LOGE(TAG, "unable to restore the previous wake phrase: %s",
                     previous);
        }
        if (error) {
            snprintf(error, error_size, "模型不认识「%.40s」", phrase);
        }
    }

    s_wakeup.buffered = 0;
    if (s_wakeup.frame_queue) {
        xQueueReset(s_wakeup.frame_queue);
    }
    if (s_wakeup.model_data) {
        s_wakeup.multinet->clean(s_wakeup.model_data);
    }
    s_wakeup.armed = was_armed;
    return err;
}

esp_err_t robot_wakeup_set_label(const char *label)
{
    if (label == NULL || strlen(label) >= WAKE_LABEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_wakeup.label, label, sizeof(s_wakeup.label));
    wake_store_label(s_wakeup.label);
    ESP_LOGI(TAG, "wake label set to: %s", s_wakeup.label);
    return ESP_OK;
}

const char *robot_wakeup_label(void)
{
    return s_wakeup.label;
}

const char *robot_wakeup_phrase(void)
{
    return s_wakeup.phrase[0] != '\0' ? s_wakeup.phrase : WAKE_PHRASE_DEFAULT;
}

#ifdef __cplusplus
}
#endif
