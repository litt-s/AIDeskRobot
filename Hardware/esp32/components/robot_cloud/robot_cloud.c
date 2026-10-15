#include "robot_cloud.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "robot_audio.h"
#include "robot_motion.h"
#include "robot_ui.h"
#if CONFIG_ROBOT_CLOUD_VOICE
#include "robot_voice.h"
#endif

#define TAG "robot_cloud"
#define URL_MAX 256
#define BODY_MAX 2048
#define RESPONSE_MAX 4096
/* HTTPS handshakes need far more stack than a plain socket; the project has
 * already been bitten by an undersized task stack in the voice path. */
#define TASK_STACK 10240
#define TASK_PRIORITY 4
#define FAILURES_BEFORE_REHELLO 3
#define BACKOFF_STEP_MS 5000
#define BACKOFF_MAX_MS 30000

typedef struct {
    const char *expression;
    robot_expression_t value;
} expression_entry_t;

/* The console sends the names used by the web console; several of them differ
 * from the firmware enum spelling, so both spellings are accepted. */
static const expression_entry_t k_expressions[] = {
    {"neutral", ROBOT_EXPRESSION_NEUTRAL},
    {"happy", ROBOT_EXPRESSION_HAPPY},
    {"sad", ROBOT_EXPRESSION_SAD},
    {"angry", ROBOT_EXPRESSION_ANGRY},
    {"surprised", ROBOT_EXPRESSION_SURPRISED},
    {"thinking", ROBOT_EXPRESSION_THINKING},
    {"confused", ROBOT_EXPRESSION_CONFUSED},
    {"awkward", ROBOT_EXPRESSION_AWKWARD},
    {"embarrassed", ROBOT_EXPRESSION_EMBARRASSED},
    {"cry", ROBOT_EXPRESSION_CRY},
    {"crying", ROBOT_EXPRESSION_CRY},
    {"laughing", ROBOT_EXPRESSION_LAUGHING},
    {"funny", ROBOT_EXPRESSION_FUNNY},
    {"loving", ROBOT_EXPRESSION_LOVING},
    {"shocked", ROBOT_EXPRESSION_SHOCKED},
    {"cool", ROBOT_EXPRESSION_COOL},
    {"relaxed", ROBOT_EXPRESSION_RELAXED},
    {"delicious", ROBOT_EXPRESSION_DELICIOUS},
    {"kissy", ROBOT_EXPRESSION_KISSY},
    {"confident", ROBOT_EXPRESSION_CONFIDENT},
    {"silly", ROBOT_EXPRESSION_SILLY},
    {"sleep", ROBOT_EXPRESSION_SLEEP},
    {"sleepy", ROBOT_EXPRESSION_SLEEP},
};

typedef struct {
    const char *action;
    robot_action_type_t value;
} action_entry_t;

static const action_entry_t k_actions[] = {
    {"center", ROBOT_ACTION_CENTER},
    {"look_left", ROBOT_ACTION_LOOK_LEFT},
    {"look_right", ROBOT_ACTION_LOOK_RIGHT},
    {"look_up", ROBOT_ACTION_LOOK_UP},
    {"look_down", ROBOT_ACTION_LOOK_DOWN},
    {"nod", ROBOT_ACTION_NOD},
    {"shake", ROBOT_ACTION_SHAKE},
    {"roll", ROBOT_ACTION_ROLL},
    {"dance", ROBOT_ACTION_DANCE},
    {"blink", ROBOT_ACTION_BLINK},
    {"wakeup", ROBOT_ACTION_WAKEUP},
    {"sleep", ROBOT_ACTION_SLEEP},
};

typedef struct {
    bool running;
    bool online;
    robot_cloud_config_t config;
    TaskHandle_t task;
} cloud_state_t;

static cloud_state_t s_cloud;
/*
 * Storage for the normalised base URL.  robot_cloud keeps pointers, not
 * strings (see the note on robot_cloud_init), so the normalised value has to
 * live somewhere with static lifetime.
 */
static char s_base_url[160];

static bool lookup_expression(const char *name, robot_expression_t *out)
{
    for (size_t i = 0; i < sizeof(k_expressions) / sizeof(k_expressions[0]); ++i) {
        if (strcmp(k_expressions[i].expression, name) == 0) {
            *out = k_expressions[i].value;
            return true;
        }
    }
    return false;
}

static bool lookup_action(const char *name, robot_action_type_t *out)
{
    for (size_t i = 0; i < sizeof(k_actions) / sizeof(k_actions[0]); ++i) {
        if (strcmp(k_actions[i].action, name) == 0) {
            *out = k_actions[i].value;
            return true;
        }
    }
    return false;
}

static esp_err_t cloud_request(const char *method, const char *path, const char *body,
                               char *response, size_t response_size, int *status_out)
{
    if (s_cloud.config.base_url == NULL || s_cloud.config.device_key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[URL_MAX];
    int written = snprintf(url, sizeof(url), "%s%s", s_cloud.config.base_url, path);
    if (written <= 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    char authorization[160];
    snprintf(authorization, sizeof(authorization), "Device %s", s_cloud.config.device_key);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = (int)s_cloud.config.request_timeout_ms,
        .keep_alive_enable = true,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    int body_length = body != NULL ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(client, body_length);
    if (err == ESP_OK && body_length > 0) {
        int wrote = esp_http_client_write(client, body, body_length);
        if (wrote != body_length) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request to %s failed: %s", path, esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (response != NULL && response_size > 1) {
        int read = esp_http_client_read(client, response, (int)response_size - 1);
        response[read > 0 ? read : 0] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_out != NULL) {
        *status_out = status;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "%s returned HTTP %d", path, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t apply_command(const char *type, const char *payload, char *result, size_t result_size)
{
    cJSON *root = payload != NULL && payload[0] != '\0' ? cJSON_Parse(payload) : cJSON_CreateObject();
    if (root == NULL) {
        snprintf(result, result_size, "payload 不是合法 JSON");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    const cJSON *name = cJSON_GetObjectItem(root, "name");

    if (strcmp(type, "emotion") == 0) {
        robot_expression_t expression;
        if (!cJSON_IsString(name) || !lookup_expression(name->valuestring, &expression)) {
            snprintf(result, result_size, "未知表情");
            err = ESP_ERR_INVALID_ARG;
        } else if (robot_ui_show_expression(expression) != ESP_OK) {
            snprintf(result, result_size, "显示表情失败");
            err = ESP_FAIL;
        } else {
            snprintf(result, result_size, "已显示 %s", name->valuestring);
        }
    } else if (strcmp(type, "action") == 0) {
        robot_action_type_t action;
        if (!cJSON_IsString(name) || !lookup_action(name->valuestring, &action)) {
            snprintf(result, result_size, "未知动作");
            err = ESP_ERR_INVALID_ARG;
        } else {
            robot_action_t request = {0};
            request.type = action;
            if (robot_action_queue_submit(&request, 1000) != ESP_OK) {
                snprintf(result, result_size, "动作队列繁忙");
                err = ESP_FAIL;
            } else {
                snprintf(result, result_size, "已执行 %s", name->valuestring);
            }
        }
    } else if (strcmp(type, "volume") == 0) {
        const cJSON *value = cJSON_GetObjectItem(root, "value");
        if (!cJSON_IsNumber(value) || value->valueint < 0 || value->valueint > 100) {
            snprintf(result, result_size, "音量取值无效");
            err = ESP_ERR_INVALID_ARG;
        } else if (robot_audio_set_volume((uint8_t)value->valueint) != ESP_OK) {
            snprintf(result, result_size, "设置音量失败");
            err = ESP_FAIL;
        } else {
            snprintf(result, result_size, "音量已设为 %d", value->valueint);
        }
    } else if (strcmp(type, "speak") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        if (!cJSON_IsString(text)) {
            snprintf(result, result_size, "缺少播报文本");
            err = ESP_ERR_INVALID_ARG;
        } else {
            esp_err_t spoken = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_ROBOT_CLOUD_VOICE
            /*
             * The cloud voice channel is the only way to speak: the server
             * synthesises the sentence and streams the PCM back.  There is no
             * second route to fall back on, so a failure is reported as the
             * command result instead of being retried on another stack.
             */
            if (!robot_voice_busy()) {
                spoken = robot_voice_say(text->valuestring);
                if (spoken != ESP_OK) {
                    snprintf(result, result_size, "%.100s", robot_voice_last_result());
                }
            } else {
                spoken = ESP_ERR_INVALID_STATE;
            }
#endif
            if (spoken == ESP_OK) {
                snprintf(result, result_size, "已播报");
            } else if (spoken == ESP_ERR_INVALID_STATE) {
                snprintf(result, result_size, "机器人正在对话中");
            } else {
                snprintf(result, result_size, "设备未配置语音服务，暂时无法播报");
                err = ESP_ERR_NOT_SUPPORTED;
            }
        }
    } else if (strcmp(type, "listen") == 0) {
#if CONFIG_ROBOT_CLOUD_VOICE
        esp_err_t triggered = robot_voice_trigger();
        if (triggered == ESP_OK) {
            /* The turn runs in its own task; the ack only reports that it
             * started, because the answer itself arrives as audio. */
            snprintf(result, result_size, "已开始收音，请对着机器人说话");
        } else if (robot_voice_busy()) {
            snprintf(result, result_size, "机器人正在对话中");
            err = ESP_ERR_INVALID_STATE;
        } else {
            snprintf(result, result_size, "语音通道未就绪");
            err = ESP_ERR_INVALID_STATE;
        }
#else
        snprintf(result, result_size, "本固件未启用语音对话");
        err = ESP_ERR_NOT_SUPPORTED;
#endif
    } else if (strcmp(type, "reboot") == 0) {
        snprintf(result, result_size, "正在重启");
        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    } else {
        snprintf(result, result_size, "不支持的指令类型");
        err = ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t cloud_ack(const char *command_id, bool ok, const char *result)
{
    char path[URL_MAX];
    snprintf(path, sizeof(path), "/api/device/commands/%s/ack", command_id);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "status", ok ? "done" : "failed");
    if (result != NULL) {
        cJSON_AddStringToObject(body, "result", result);
    }
    char *serialized = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cloud_request("POST", path, serialized, NULL, 0, NULL);
    cJSON_free(serialized);
    return err;
}

static esp_err_t cloud_poll_commands(void)
{
    char *response = malloc(RESPONSE_MAX);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cloud_request("GET", "/api/device/commands", NULL, response, RESPONSE_MAX, NULL);
    if (err != ESP_OK) {
        free(response);
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *commands = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(commands)) {
        const cJSON *command = NULL;
        cJSON_ArrayForEach(command, commands) {
            const cJSON *id = cJSON_GetObjectItem(command, "id");
            const cJSON *type = cJSON_GetObjectItem(command, "type");
            const cJSON *payload = cJSON_GetObjectItem(command, "payload");
            if (!cJSON_IsString(id) || !cJSON_IsString(type)) {
                continue;
            }
            const char *payload_text = cJSON_IsString(payload) ? payload->valuestring : "{}";
            char result[160] = {0};
            esp_err_t applied = apply_command(type->valuestring, payload_text, result, sizeof(result));
            ESP_LOGI(TAG, "command %s (%s): %s", type->valuestring, id->valuestring, result);
            if (cloud_ack(id->valuestring, applied == ESP_OK, result) != ESP_OK) {
                ESP_LOGW(TAG, "failed to acknowledge %s", id->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cloud_hello(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "firmware", description != NULL ? description->version : "unknown");
    char *serialized = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cloud_request("POST", "/api/device/hello", serialized, NULL, 0, NULL);
    cJSON_free(serialized);
    return err;
}

static esp_err_t cloud_report_status(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "firmware", description != NULL ? description->version : "unknown");
    cJSON_AddNumberToObject(body, "volume", robot_audio_get_volume());
    char *serialized = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cloud_request("POST", "/api/device/status", serialized, NULL, 0, NULL);
    cJSON_free(serialized);
    return err;
}

static void cloud_task(void *arg)
{
    (void)arg;
    bool greeted = false;
    int failures = 0;
    int64_t last_status_us = 0;

    while (s_cloud.running) {
        if (!greeted) {
            if (cloud_hello() == ESP_OK) {
                greeted = true;
                failures = 0;
                s_cloud.online = true;
                ESP_LOGI(TAG, "device channel online");
            } else {
                s_cloud.online = false;
                ++failures;
                uint32_t backoff = BACKOFF_STEP_MS * (uint32_t)failures;
                if (backoff > BACKOFF_MAX_MS) {
                    backoff = BACKOFF_MAX_MS;
                }
                vTaskDelay(pdMS_TO_TICKS(backoff));
                continue;
            }
        }

        if (cloud_poll_commands() != ESP_OK) {
            ++failures;
            s_cloud.online = false;
            if (failures >= FAILURES_BEFORE_REHELLO) {
                greeted = false;
                ESP_LOGW(TAG, "device channel offline, will re-handshake");
            }
        } else {
            failures = 0;
            s_cloud.online = true;
        }

        int64_t now_us = esp_timer_get_time();
        if (s_cloud.online && now_us - last_status_us >= (int64_t)s_cloud.config.status_interval_ms * 1000) {
            last_status_us = now_us;
            cloud_report_status();
        }

        vTaskDelay(pdMS_TO_TICKS(s_cloud.config.poll_interval_ms));
    }

    s_cloud.online = false;
    s_cloud.task = NULL;
    vTaskDelete(NULL);
}

/* A host with no dots (or an IP literal) is a LAN address, where plain http is
 * the only thing that can work; anything else is assumed to be a real name and
 * therefore https.  Only reached when no scheme was given. */
static bool looks_like_lan_host(const char *url)
{
    bool digits_and_dots = true;
    bool has_dot = false;
    for (const char *cursor = url; *cursor && *cursor != ':' && *cursor != '/';
         ++cursor) {
        if (*cursor == '.') {
            has_dot = true;
        } else if (*cursor < '0' || *cursor > '9') {
            digits_and_dots = false;
        }
    }
    return digits_and_dots || !has_dot;
}

/*
 * Last line of defence for the console address.  A bare "192.168.137.1:8787"
 * is what people type, and it is what the setup page used to store before it
 * learned to normalise on save; esp_http_client refuses such a URL outright
 * ("Error parse url ..."), which leaves a robot that looks online but can never
 * reach the console.  Normalising here as well means an address already stored
 * in NVS heals itself instead of having to be retyped.  The setup page's
 * normalize_server() applies the same rule on write; keep the two in step.
 */
static const char *normalize_base_url(const char *url, char *output, size_t output_size)
{
    if (strstr(url, "://") != NULL) {
        snprintf(output, output_size, "%s", url);
    } else {
        snprintf(output, output_size, "%s%s",
                 looks_like_lan_host(url) ? "http://" : "https://", url);
    }
    return output;
}

esp_err_t robot_cloud_init(const robot_cloud_config_t *config)
{
    if (config == NULL || config->base_url == NULL || config->base_url[0] == '\0' ||
        config->device_key == NULL || config->device_key[0] == '\0') {
        ESP_LOGW(TAG, "device channel disabled: missing server URL or device key");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cloud.running) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_cloud, 0, sizeof(s_cloud));
    s_cloud.config = *config;
    s_cloud.config.base_url =
        normalize_base_url(config->base_url, s_base_url, sizeof(s_base_url));
    if (s_cloud.config.poll_interval_ms == 0) {
        s_cloud.config.poll_interval_ms = 3000;
    }
    if (s_cloud.config.status_interval_ms == 0) {
        s_cloud.config.status_interval_ms = 60000;
    }
    if (s_cloud.config.request_timeout_ms == 0) {
        s_cloud.config.request_timeout_ms = 8000;
    }

    size_t base_length = strlen(s_cloud.config.base_url);
    if (s_cloud.config.base_url[base_length - 1] == '/') {
        ESP_LOGW(TAG, "base URL should not end with a slash");
    }
    return ESP_OK;
}

esp_err_t robot_cloud_start(void)
{
    if (s_cloud.config.base_url == NULL || s_cloud.config.device_key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cloud.running) {
        return ESP_OK;
    }

    s_cloud.running = true;
    if (xTaskCreate(cloud_task, "robot_cloud", TASK_STACK, NULL, TASK_PRIORITY, &s_cloud.task) != pdPASS) {
        s_cloud.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void robot_cloud_stop(void)
{
    s_cloud.running = false;
    s_cloud.online = false;
}

void robot_cloud_deinit(void)
{
    s_cloud.running = false;
    s_cloud.online = false;
    s_cloud.config.base_url = NULL;
    s_cloud.config.device_key = NULL;
}

bool robot_cloud_online(void)
{
    return s_cloud.online;
}

const char *robot_cloud_base_url(void)
{
    return s_cloud.config.base_url;
}

const char *robot_cloud_device_key(void)
{
    return s_cloud.config.device_key;
}
