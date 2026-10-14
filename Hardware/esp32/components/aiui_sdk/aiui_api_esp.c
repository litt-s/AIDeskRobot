#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include "aiui_wrappers_os.h"
#include "aiui_log.h"
#include "base64.h"
#include "cjson.h"
#include "aiui_queue.h"
#include "hmac_sha256.h"
#include "aiui_websocket_api.h"
#include "aiui_message_parse.h"
#include "aiui_api.h"

#define AIUI_TAG "aiui_api"
#include "cjson_helper.h"

static void *aiui_cjson_malloc(size_t size)
{
    return size > INT_MAX ? NULL : aiui_hal_malloc((int)size);
}

static void aiui_cjson_free(void *ptr)
{
    aiui_hal_free(ptr);
}

// impl of aiui_log.h
/* extern*/ int g_aiui_log_level = LOG_DEFAULT_LEVEL;

#define AIUI_WS_ORIGIN_HOST "wss://aiui.xf-yun.com"
#define AIUI_WS_HOST_NO_WSS "aiui.xf-yun.com"
#define AIUI_WS_PATH_FORMAT "/v2/aiui/ws?host=%s&date=%s&authorization=%s"
#define AIUI_WS_REQUEST_LINE "GET /v2/aiui/ws HTTP/1.1"

// aiui v3 地址
#if 0
// "wss://sos-4o.cn-huabei-1.xf-yun.com:443/v3/sos"    // 小新畅聊 sos 链路.
#define AIUI_V3_WS_ORIGIN_HOST "wss://sos-4o.cn-huabei-1.xf-yun.com"
#define AIUI_V3_WS_HOST_NO_WSS "sos-4o.cn-huabei-1.xf-yun.com"
#define AIUI_V3_WS_PATH_FORMAT "/v3/sos?host=%s&date=%s&authorization=%s"
#define AIUI_V3_WS_REQUEST_LINE "GET /v3/sos HTTP/1.1"
#else
// "wss:////aiui.xf-yun.com/v3/aiint/sos"  // aiint 极速交互链路。 协议文档: https://aiui-doc.xf-yun.com/project-1/doc-584/
#define AIUI_V3_WS_ORIGIN_HOST "wss://aiui.xf-yun.com"
#define AIUI_V3_WS_HOST_NO_WSS "aiui.xf-yun.com"
#define AIUI_V3_WS_PATH_FORMAT "/v3/aiint/sos?host=%s&date=%s&authorization=%s"
#define AIUI_V3_WS_REQUEST_LINE "GET /v3/aiint/sos HTTP/1.1"
#endif

// 主动合成的场景
#define SCENE_IFLY_TTS  "IFLYTEK.hts"

// 不支持的函数
#define NOT_SUPPORT_FUNCTION_IN_SCHEME_V3(aiui)                                          \
    do                                                                                   \
    {                                                                                    \
        if (AIUI_SCHEME_VERSION_3 != (aiui)->cfg.global.scheme_version)                  \
        {                                                                                \
            break;                                                                       \
        }                                                                                \
        LOGE(AIUI_TAG, "not support function(%s:%d) in scheme v3.", __func__, __LINE__); \
        return -1;                                                                       \
    } while (0)

/* AIUI 协议版本 */
typedef enum
{
    AIUI_SCHEME_VERSION_2 = 2,
    AIUI_SCHEME_VERSION_3 = 3,
} AIUI_SCHEME_VERSION;

/* websocket 会话连接状态 */
typedef enum
{
    AIUI_STATUS_INIT = 0,
    AIUI_STATUS_CONNECTING,
    AIUI_STATUS_CONNECTED,
    AIUI_STATUS_DISCONNECTED,
    AIUI_STATUS_UNINIT
} AIUI_SESSION_STATUS;

/* 发送的数据类型 */
typedef enum
{
    AIUI_DATA_START = 0,
    AIUI_DATA_TEXT,
    AIUI_DATA_AUDIO,
    AIUI_DATA_TTS,
    AIUI_DATA_STOP,

    AIUI_DATA_MAX
} AIUI_DATA_TYPE;

typedef struct aiui_data_s
{
    AIUI_DATA_TYPE type;
    char* data;
    int len;
} aiui_data_t;


typedef struct aiui_info_s
{
    /*  aiui 配置信息 */
    struct
    {
        struct
        {
            char appid[9];
            char key[33];
            char api_secret[33];
        } login;

        struct
        {
            AIUI_SCHEME_VERSION scheme_version; // aiui 协议版本号
            // 默认交互场景. 通常是 main
            char scene[33];
            // 设备唯一id. 小于等于32字节的字符串
            char uid[33];
        } global;

        struct
        {
            // tts 发音人名称
            char vcn[33];
        } tts;

    } cfg;

    /* 根据 cfg.global.scheme_version 来自动配置的 websocket 连接信息 */
    struct
    {
        const char *origin_host;
        const char *host_no_wss;
        const char *path_format;
        const char *request_line;
    } ws_config;

    AIUI_SESSION_STATUS session_status;
//    unsigned int heart_beat_id;
    AIUI_MESSAGE_PARSE_HANDLE message_parse_handle;
    AIUI_QUEUE_HANDLE queue_handle;
    AIUI_WEBSOCKET_HANDLE websocket_handle;

    void *sem;
    void *queue_sem;
    void *queue_mutex;
    volatile int accepting_data;

    void *user_params;
    aiui_event on_event;

    int session_times; // 会话次数, 用于拼接 stmid
    int data_status;  // 0-代表首帧、1-代表中间帧、2-代表尾帧、3-代表数据一帧发送完毕，文本请求固定为3
    void* tid;
    int scene; // 场景
} aiui_info_t;

static void _aiui_free_data(aiui_data_t *data)
{
    if (NULL == data)
    {
        return;
    }
    if (NULL != data->data)
    {
        aiui_hal_free(data->data);
    }
    aiui_hal_free(data);
}

static void _aiui_clear_queue_unlocked(aiui_info_t *aiui)
{
    aiui_data_t *data = (aiui_data_t *)aiui_queue_front(aiui->queue_handle);
    while (NULL != data)
    {
        aiui_queue_pop(aiui->queue_handle);
        _aiui_free_data(data);
        data = (aiui_data_t *)aiui_queue_front(aiui->queue_handle);
    }
}

static aiui_data_t *_aiui_take_queued_data(aiui_info_t *aiui)
{
    aiui_data_t *data = NULL;
    aiui_hal_mutex_lock(aiui->queue_mutex);
    data = (aiui_data_t *)aiui_queue_front(aiui->queue_handle);
    if (NULL != data)
    {
        aiui_queue_pop(aiui->queue_handle);
    }
    aiui_hal_mutex_unlock(aiui->queue_mutex);
    return data;
}

static int _aiui_enqueue_payload(aiui_info_t *aiui, aiui_data_t *data)
{
    int ret = -1;
    aiui_hal_mutex_lock(aiui->queue_mutex);
    if (aiui->accepting_data)
    {
        ret = aiui_queue_push(aiui->queue_handle, data);
    }
    aiui_hal_mutex_unlock(aiui->queue_mutex);
    if (0 == ret)
    {
        aiui_hal_sem_post(aiui->queue_sem);
    }
    else
    {
        _aiui_free_data(data);
    }
    return ret;
}

static int _aiui_replace_queue_with_control(aiui_info_t *aiui,
                                            aiui_data_t *control)
{
    int ret = -1;
    aiui_hal_mutex_lock(aiui->queue_mutex);
    aiui->accepting_data = (AIUI_DATA_START == control->type);
    _aiui_clear_queue_unlocked(aiui);
    ret = aiui_queue_push(aiui->queue_handle, control);
    aiui_hal_mutex_unlock(aiui->queue_mutex);
    if (0 == ret)
    {
        aiui_hal_sem_post(aiui->queue_sem);
    }
    else
    {
        _aiui_free_data(control);
    }
    return ret;
}

static void _aiui_set_accepting_data(aiui_info_t *aiui, int accepting)
{
    aiui_hal_mutex_lock(aiui->queue_mutex);
    aiui->accepting_data = accepting;
    aiui_hal_mutex_unlock(aiui->queue_mutex);
}

static void _aiui_throw_connection_status_event(aiui_info_t* aiui, AIUI_EVENT_TYPE connection_status)
{
    aiui_callback_event_t callback_event =
    {
        .type = connection_status,
        .general = {
            .code = 0,
            .arg1 = connection_status,
            .message_size = 0,
            .message = "",
        },
    };
    aiui->on_event(aiui->user_params, &callback_event);
}

// websocket 连接状态回调
static void _aiui_on_ws_connect_status_changed(void *user_param, aiui_websocket_connect_status_e status)
{
    aiui_info_t *aiui = (aiui_info_t *)user_param;
    if (NULL == user_param)
    {
        return;
    }
    LOGI(AIUI_TAG, "recv new connect status: %d", status);

    if (AIUI_WS_CONNECT_STATUS_CONNECTED == status)
    {
        LOGI(AIUI_TAG, "AIUI_WS_CONNECT_STATUS_CONNECTED, now post sem...");
        aiui->session_status = AIUI_STATUS_CONNECTED;
        _aiui_set_accepting_data(aiui, 1);
        aiui_hal_sem_post(aiui->sem);

        _aiui_throw_connection_status_event(aiui, AIUI_EVENT_CONNECTED_TO_SERVER);
    }
    else if (AIUI_WS_CONNECT_STATUS_DISCONNECTED == status)
    {
        aiui->session_status = AIUI_STATUS_DISCONNECTED;
        _aiui_set_accepting_data(aiui, 0);

        _aiui_throw_connection_status_event(aiui, AIUI_EVENT_SERVER_DISCONNECTED);
    }
    else
    {
        LOGE(AIUI_TAG, "unhandled connect status: %d", status);
    }
}

// websocket 消息回调
static void _aiui_on_ws_message(void *user_param, const char *msg, int len)
{
    LOGV(AIUI_TAG, "recv websocket msg: %d %s", len, msg);
    if (NULL == user_param || NULL == msg || len < 1)
    {
        return;
    }
    aiui_info_t *aiui = (aiui_info_t *)user_param;
    aiui_message_parse(aiui->message_parse_handle, msg, len);
}

// aiui_message_parse 解析数据回调
static void _aiui_on_got_the_parsed_result(aiui_event_result_t *result, void* user_param)
{
    aiui_info_t *self = (aiui_info_t *)user_param;
    if (0 != result->session.code)
    {
        aiui_callback_event_t err_event =
        {
            .type = AIUI_EVENT_ERROR,
            .general = {
                .code = result->session.code,
                .message_size = result->data_size,
                .message = result->data,
            },
        };
        self->on_event(self->user_params, &err_event);
        return;
    }

    aiui_callback_event_t event =
    {
        .type = AIUI_EVENT_RESULT,
        .result = *result,
    };
    self->on_event(self->user_params, &event);
}

static void _aiui_encode_url_component(const char *src, char *dst)
{
    int j = 0;
    for (size_t i = 0; i < strlen(src); i++)
    {
        if ((src[i] >= 'a' && src[i] <= 'z')
        || (src[i] >= 'A' && src[i] <= 'Z')
        || (src[i] >= '0' && src[i] <= '9')
        || (src[i] >= '\'' && src[i] <= '*')
        || src[i] == '!'
        || src[i] == '-'
        || src[i] == '.'
        || src[i] == '_'
        || src[i] == '~')
        {
            dst[j++] = src[i];
        }
        else
        {
            dst[j++] = '%';
            int n = sprintf(dst + j, "%X", src[i]);
            j += n;
        }
    }
}

static char *_aiui_path_format(aiui_info_t *aiui)
{
    time_t current_time = time(NULL);
    // 将time_t类型转换为struct tm类型
    struct tm time_info = {0,};
    gmtime_r(&current_time, &time_info);

    // 构造RFC1123格式的时间字符串
    char date_fmt[64] = {0};
    strftime(date_fmt, sizeof(date_fmt), "%a, %d %b %Y %H:%M:%S GMT", &time_info);
    char header[128] = {0};
    snprintf(header, sizeof(header), "host: %s\ndate: %s\n%s",
             aiui->ws_config.host_no_wss, date_fmt, aiui->ws_config.request_line);
    unsigned char signature_sha[32] = {0};
    hmac_sha256(aiui->cfg.login.api_secret, strlen(aiui->cfg.login.api_secret),
                header, strlen(header), signature_sha, sizeof(signature_sha));
    unsigned char signature[64] = {0};
    size_t write_len = 0;
    mbedtls_base64_encode(signature, sizeof(signature),
                          &write_len, signature_sha, sizeof(signature_sha));
    char authorization_origin[512] = {0};
    snprintf(authorization_origin, sizeof(authorization_origin),
             "api_key=\"%s\", algorithm=\"%s\", headers=\"%s\", signature=\"%s\"",
             aiui->cfg.login.key, "hmac-sha256", "host date request-line", (const char *)signature);
    size_t authorization_len = (strlen(authorization_origin) + 2) / 3 * 4 + 1;
    char *authorization = (char *)aiui_hal_malloc(authorization_len);
    memset(authorization, 0, authorization_len);
    mbedtls_base64_encode((unsigned char*)authorization, authorization_len,
                          &write_len, (const unsigned char*)authorization_origin,
                          strlen(authorization_origin));

    char *date_encode =  (char*)aiui_hal_malloc(strlen(date_fmt) * 3 + 1);
    memset(date_encode, 0, strlen(date_fmt) * 3 + 1);
    _aiui_encode_url_component(date_fmt, date_encode);
    size_t url_len = strlen(aiui->ws_config.path_format) + strlen(aiui->ws_config.host_no_wss)
                     + strlen(date_encode) + authorization_len + 1U;

    char *url = (char *)aiui_hal_malloc(url_len);
    memset(url, 0, url_len);
    snprintf(url, url_len, aiui->ws_config.path_format, aiui->ws_config.host_no_wss, date_encode, authorization);

    aiui_hal_free(authorization);
    aiui_hal_free(date_encode);
    return url;
}

static int aiui_connect(aiui_info_t *aiui)
{
    aiui_print_mem_info();
    if (NULL == aiui)
    {
        LOGE(AIUI_TAG, "websocket not init");
        return -1;
    }
    if (aiui->session_status == AIUI_STATUS_CONNECTING)
    {
        return 0;
    }

    int ret;
    char *path = _aiui_path_format(aiui);
    aiui->session_status = AIUI_STATUS_CONNECTING;
    aiui->session_times = 0;
    if (0 == (ret = aiui_websocket_connect(aiui->websocket_handle,
                                           path, aiui->ws_config.origin_host)))
    {
        LOGI(AIUI_TAG, "succeed to connect server");
        // below code move to _aiui_on_ws_connect_status_changed
//        aiui->session_status = AIUI_STATUS_CONNECTED;
//        aiui_hal_sem_post(aiui->sem);
    }
    else
    {
        LOGE(AIUI_TAG, "failed(%d) on connect server", ret);
    }
    aiui_hal_free(path);
    return ret;
}

static cJSON *_aiui_gen_tts_config_for_request_header(const char *vcn)
{
    cJSON *tts_parameter = cJSON_CreateObject();
    cJSON *tts_result_parameter = cJSON_CreateObject();
    cJSON_AddNumberToObject(tts_result_parameter, "bit_depth", 16);
    cJSON_AddNumberToObject(tts_result_parameter, "channels", 1);
    cJSON_AddStringToObject(tts_result_parameter, "encoding", "opus-wb");
    cJSON_AddNumberToObject(tts_result_parameter, "sample_rate", 16000);
    /*
     * Match the 7911 rapid-interaction demo: compressed Opus-WB with an
     * initial four-second lead, followed by paced 400 ms chunks.
     */
    cJSON_AddStringToObject(tts_result_parameter, "flow_ctrl_mode", "duration");
    cJSON_AddNumberToObject(tts_result_parameter, "flow_ctrl_size", 400);
    cJSON_AddNumberToObject(tts_result_parameter, "flow_ctrl_interval", 400);
    cJSON_AddNumberToObject(tts_result_parameter, "flow_ctrl_start", 4000);
    cJSON_AddItemToObject(tts_parameter, "tts", tts_result_parameter);
    //        cJSON_AddStringToObject(tts_parameter, "tts_res_type", "url");
#if 1
    cJSON_AddStringToObject(tts_parameter, "vcn", vcn);
#else
    cJSON_AddStringToObject(tts_parameter, "vcn", "x5_clone");
    cJSON_AddStringToObject(tts_parameter, "res_id", "777456123");
#endif
    return tts_parameter;
}

static char *_aiui_format_request_data(aiui_info_t *aiui, const char *buf, int len)
{
    size_t base64_len = 0;
    char *base64_data = NULL;
    if (len > 0)
    {
        base64_len = (len + 2) * 4 / 3  + 4;
        base64_data = (char *)aiui_hal_malloc(base64_len);
        if (NULL == base64_data)
        {
            LOGE(AIUI_TAG, "failed on allocate base64 audio, len=%zu",
                 base64_len);
            return NULL;
        }
        memset(base64_data, 0, base64_len);
        size_t write_len = 0;
        mbedtls_base64_encode((unsigned char*)base64_data, base64_len,
                              &write_len, (const unsigned char*)buf, len);
    }
    else
    {
        aiui->data_status = 2;
    }

    cJSON *root = cJSON_CreateObject();
    if (NULL == root)
    {
        if (NULL != base64_data)
        {
            aiui_hal_free(base64_data);
        }
        return NULL;
    }

    // 1. 添加 header 字段
    cJSON *header = cJSON_CreateObject();
    cJSON_AddStringToObject(header, "sn", aiui->cfg.global.uid);
    cJSON_AddStringToObject(header, "uid", aiui->cfg.global.uid);
    cJSON_AddStringToObject(header, "app_id", aiui->cfg.login.appid);
    const char *stmid_fmt =
        (3 == aiui->data_status) ? "text-%d" : "audio-%d";
    char stmid[32] = {0};
    snprintf(stmid, sizeof(stmid), stmid_fmt, aiui->session_times);
    cJSON_AddStringToObject(header, "stmid", stmid);
    int header_status = aiui->data_status;
    if (AIUI_SCHEME_VERSION_3 == aiui->cfg.global.scheme_version &&
        0 == aiui->scene)
    {
        /*
         * 7911 continuous_vad: only the first audio frame after WebSocket
         * connect uses header status=0; every later audio header uses 1.
         * payload.audio.status still carries the per-utterance 0/1/2 state.
         */
        header_status = (0 == aiui->session_times &&
                         0 == aiui->data_status) ? 0 : 1;
    }
    cJSON_AddNumberToObject(header, "status", header_status);
    cJSON_AddStringToObject(header, "scene", (0 == aiui->scene) ? aiui->cfg.global.scene : SCENE_IFLY_TTS);
    cJSON_AddStringToObject(header, "os_sys", "rtos");
    cJSON_AddStringToObject(header, "sdk_ver", aiui_get_version());
    if (AIUI_SCHEME_VERSION_3 == aiui->cfg.global.scheme_version)
    {
        /*
         * This is the RTOS WebSocket header field, not aiui.cfg's
         * prot_interact_mode. Audio is continuous; one-frame text/TTS
         * requests must use oneshot.
         */
        cJSON_AddStringToObject(
            header, "interact_mode",
            (1 == aiui->scene || 3 == aiui->data_status)
                ? "oneshot" : "continuous_vad");
    }
    cJSON_AddItemToObject(root, "header", header);

    // 2. 添加 parameter 字段
    cJSON *parameter = cJSON_CreateObject();
    // 交互
    cJSON *tts_parameter = NULL;
    if (AIUI_SCHEME_VERSION_3 == aiui->cfg.global.scheme_version)
    {
        tts_parameter = _aiui_gen_tts_config_for_request_header(aiui->cfg.tts.vcn);
    }
    if (0 == aiui->scene)
    {
        // parameter.iat
        const char* parameter_iat_str = NULL;
        if (AIUI_SCHEME_VERSION_3 == aiui->cfg.global.scheme_version)
        {
            /* Match the 7911 rapid-interaction request exactly. */
            parameter_iat_str = "{\"iat\":{\"compress\":\"raw\",\"encoding\":\"utf8\",\"format\":\"json\"},\"vgap\":40}";
        }
        else if (AIUI_SCHEME_VERSION_2 == aiui->cfg.global.scheme_version)
        {
            parameter_iat_str = "{\"iat\":{\"compress\":\"raw\",\"encoding\":\"utf8\",\"format\":\"json\"},\"svad\":\"1\",\"eos\":600,\"vgap\":400}";
        }
        cJSON_AddRawToObject(parameter, "iat", parameter_iat_str);

        // parameter.nlp
        // prompt为人设配置.
        // const char *parameter_nlp_str = "{\"new_session\":false,\"personal\":\"\",\"prompt\":\"你是一个叫小月的台湾女孩，说话机车, 声音好听, 习惯简洁表达, 爱用网络梗, 能够帮助人们解决生活中的各种问题。我的男朋友是一个程序员，梦想是开发一个喜欢哈哈大笑的女孩, 爱东说西爱吹牛，不合逻辑的也照吹，就要逗别人开心。\", \"nlp\":{\"compress\":\"raw\",\"encoding\":\"utf8\",\"format\":\"json\"}}";
        /*
         * header status=0 opens the continuous_vad interaction. Setting
         * new_session=true on every audio request resets the cloud dialogue
         * memory, so follow-up turns cannot refer to the previous answer.
         */
        cJSON *parameter_nlp = cJSON_CreateObject();
        cJSON *nlp_result = cJSON_CreateObject();
        if (NULL == parameter_nlp || NULL == nlp_result)
        {
            cJSON_Delete(parameter_nlp);
            cJSON_Delete(nlp_result);
            cJSON_Delete(root);
            if (NULL != base64_data)
            {
                aiui_hal_free(base64_data);
            }
            return NULL;
        }
        cJSON_AddBoolToObject(parameter_nlp, "new_session", 0);
        /*
         * NetEase music authorization binds to the same device identity in
         * both header.sn and parameter.nlp.sn. Keep it identical to the
         * MAC-derived global uid used above.
         */
        cJSON_AddStringToObject(parameter_nlp, "sn",
                               aiui->cfg.global.uid);
        cJSON_AddStringToObject(nlp_result, "compress", "raw");
        cJSON_AddStringToObject(nlp_result, "encoding", "utf8");
        cJSON_AddStringToObject(nlp_result, "format", "json");
        cJSON_AddItemToObject(parameter_nlp, "nlp", nlp_result);
        cJSON_AddItemToObject(parameter, "nlp", parameter_nlp);
    }
    else // tts
    {
        if (NULL == tts_parameter)
        {
            tts_parameter = _aiui_gen_tts_config_for_request_header(aiui->cfg.tts.vcn);
        }
    }
    if (NULL != tts_parameter)
    {
        // parameter.tts
        cJSON_AddItemToObject(parameter, "tts", tts_parameter);
    }
    // parameter
    cJSON_AddItemToObject(root, "parameter", parameter);

    // 3. 添加 payload 字段
    cJSON *payload = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (aiui->data_status == 3) // 3: 文本交互. 一把传完
    {
        cJSON_AddStringToObject(data, "encoding", "utf8");
        cJSON_AddStringToObject(data, "compress", "raw");
        cJSON_AddStringToObject(data, "format", "plain");
        cJSON_AddNumberToObject(data, "status", aiui->data_status);
        cJSON_AddStringToObject(data, "text", NULL == base64_data ? "" : base64_data);
        cJSON_AddItemToObject(payload, "text", data);
    }
    else // 0/1/2 音频状态.
    {
        const char *audio_encoding = "raw";// 支持 opus-wb/speex-wb/raw
        cJSON_AddStringToObject(data, "encoding", audio_encoding);
        cJSON_AddNumberToObject(data, "sample_rate", 16000);
        cJSON_AddNumberToObject(data, "channels", 1);
        cJSON_AddNumberToObject(data, "bit_depth", 16);
        cJSON_AddNumberToObject(data, "status", aiui->data_status);
        /* 40 ms PCM blocks reduce JSON/TLS overhead on ESP32-S3. */
        int frame_size = len;
        cJSON_AddNumberToObject(data, "frame_size", frame_size);
        cJSON_AddStringToObject(data, "audio", NULL == base64_data ? "" : base64_data);
        cJSON_AddItemToObject(payload, "audio", data);
    }
    // payload node
    cJSON_AddItemToObject(root, "payload", payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (NULL != base64_data)
    {
        aiui_hal_free(base64_data);
    }
    // 更新数据帧状态
    if (aiui->data_status == 0)
    {
        aiui->data_status = 1;
    }
    else if (aiui->data_status == 2 || aiui->data_status == 3)
    {
        aiui->data_status = 0;
        aiui->session_times++;
    }
    return json_str;
}

static void *_aiui_main_task(void *param)
{
    if (NULL == param)
    {
        return NULL;
    }
    size_t audio_packet_count = 0;
    aiui_info_t* aiui = (aiui_info_t *)param;
    while (aiui->session_status != AIUI_STATUS_UNINIT)
    {
        aiui_hal_sem_wait(aiui->queue_sem, -1);
        if (aiui->session_status == AIUI_STATUS_UNINIT)
        {
            break;
        }

        aiui_data_t* data = _aiui_take_queued_data(aiui);
        if (NULL == data)
        {
            continue;
        }
        const bool audio_eos =
            data->type == AIUI_DATA_AUDIO && data->len == 0;

        switch (data->type)
        {
            case AIUI_DATA_START:
            {
                aiui->data_status = 0;  // 新的会话开始 默认数据帧状态为0
                if (aiui->session_status == AIUI_STATUS_CONNECTED)
                {
                    LOGI(AIUI_TAG, "ok, we already connected server.");
                    break;
                }
                aiui_connect(aiui);
                /* 注释掉这块, 等待ws连接成功的状态回调, 连接成功状态里会修改 session_status
                if (AIUI_STATUS_CONNECTED != aiui->session_status)
                {
                    LOGI(AIUI_TAG, "ok, connect succeed, now you can send data.");
                    break;
                }
                */
                int rc = aiui_hal_sem_wait(aiui->sem, 5000);
                if (0 != rc || AIUI_STATUS_CONNECTED != aiui->session_status)
                {
                    LOGE(AIUI_TAG, "wait connected status timeout. %d", rc);
                    aiui->session_status = AIUI_STATUS_DISCONNECTED;

                    _aiui_throw_connection_status_event(aiui, AIUI_EVENT_CONNECTED_TIMEOUT);
                }
                else
                {
                    LOGI(AIUI_TAG, "connect succeed.");
                }
            }
            break;
            case AIUI_DATA_TEXT:
            case AIUI_DATA_AUDIO:
            case AIUI_DATA_TTS:
            {
                aiui->scene = (data->type == AIUI_DATA_TTS ? 1 : 0); // IFLYTEK.tts : 默认场景
                if (data->type == AIUI_DATA_TEXT || data->type == AIUI_DATA_TTS)
                {
                    aiui->data_status = 3;  // 文本交互或合成，数据帧状态固定为3
                }

                char *request_data = _aiui_format_request_data(aiui, data->data, data->len);
                if (NULL != data->data)
                {
                    aiui_hal_free(data->data);
                    data->data = NULL;
                }
                data->len = 0;
                if (NULL == request_data)
                {
                    LOGE(AIUI_TAG, "failed on assemble request_data");
                    break;
                }
                int request_len = (int)strlen(request_data);
                int send_ret = aiui_websocket_send_bin(
                    aiui->websocket_handle, request_data, request_len);
                /*
                 * aiui_websocket_send_bin() returns the payload length on
                 * success and -1 on failure. A positive value is not an
                 * error code.
                 */
                if (send_ret != request_len)
                {
                    LOGE(AIUI_TAG, "websocket send failed: type=%d ret=%d",
                         data->type, send_ret);
                    /*
                     * Match the 7911 transport policy: after any incomplete
                     * WebSocket frame, stop accepting PCM, discard queued
                     * audio and close the broken connection. Processing the
                     * queued STOP makes the receive task emit DISCONNECTED,
                     * allowing the application to reconnect automatically.
                     */
                    aiui_data_t *stop =
                        (aiui_data_t *)aiui_hal_malloc(sizeof(aiui_data_t));
                    if (NULL != stop)
                    {
                        stop->type = AIUI_DATA_STOP;
                        stop->data = NULL;
                        stop->len = 0;
                        _aiui_replace_queue_with_control(aiui, stop);
                    }
                }
                if (data->type == AIUI_DATA_AUDIO && send_ret == request_len)
                {
                    ++audio_packet_count;
                    if (audio_eos)
                    {
                        LOGI(AIUI_TAG,
                             "websocket audio EOS sent: packets=%zu",
                             audio_packet_count);
                    }
                    if (audio_packet_count == 1 ||
                        audio_packet_count % 25 == 0)
                    {
                        LOGI(AIUI_TAG, "websocket audio packets=%zu",
                             audio_packet_count);
                    }
                    if (audio_packet_count % 250 == 0)
                    {
                        aiui_hal_mutex_lock(aiui->queue_mutex);
                        int pending =
                            aiui_queue_size(aiui->queue_handle);
                        aiui_hal_mutex_unlock(aiui->queue_mutex);
                        LOGI(AIUI_TAG, "outbound queue pending=%d",
                             pending);
                        aiui_print_mem_info();
                    }
                }
                cJSON_free(request_data);
            }
            break;
            case AIUI_DATA_STOP:
            {
                LOGD(AIUI_TAG, "--> perform stop");
                if (aiui->session_status == AIUI_STATUS_CONNECTED ||
                    aiui->session_status == AIUI_STATUS_CONNECTING)
                {
                    aiui_websocket_disconnect(aiui->websocket_handle);
                }
                aiui->session_status = AIUI_STATUS_DISCONNECTED;
                aiui_print_mem_info();
                LOGD(AIUI_TAG, "<-- done of stop");
            }
            break;
            default:
                LOGE(AIUI_TAG, "unhandled AIUI_DATA_TYPE: %d", data->type);
                break;
        }
        aiui_hal_free(data);

    }
    LOGD(AIUI_TAG, "aiui main task end");
    return NULL;
}

static int _aiui_parse_cfg(aiui_info_t *aiui, const char *init_params)
{
    int ret = 0;
      /*
      {"login":{"appid":"<你的appid>","key":"<你的appkey>","api_secret":"<你的apisecret>"},
       "global":{"scene":"main","aiui_ver":"3", "uid":"12345678abcdefgh"},
       "tts":{"voice_name":"","data_encoding":"raw"}}
     */
    cJSON *cfg_root = cJSON_Parse(init_params);
    do
    {
        if (!cJSON_IsObject(cfg_root))
        {
            ret = -1;
            break;
        }

        cJSON* login = cJSON_GetObjectItem(cfg_root, "login");
        if (!cJSON_IsObject(login))
        {
            ret = -2;
            break;
        }
        cJSON* appid = cJSON_GetObjectItem(login, "appid");
        cJSON* key = cJSON_GetObjectItem(login, "key");
        cJSON* api_secret = cJSON_GetObjectItem(login, "api_secret");
        if (!cJSON_IsString(appid) || NULL == appid->valuestring || '\0' == appid->valuestring[0] ||
            !cJSON_IsString(key) || NULL == key->valuestring || '\0' == key->valuestring[0] ||
            !cJSON_IsString(api_secret) || NULL == api_secret->valuestring || '\0' == api_secret->valuestring[0])
        {
            ret = -3;
            break;
        }
        strncpy(aiui->cfg.login.appid, appid->valuestring,
                sizeof(aiui->cfg.login.appid) - 1);
        strncpy(aiui->cfg.login.key, key->valuestring,
                sizeof(aiui->cfg.login.key) - 1);
        strncpy(aiui->cfg.login.api_secret, api_secret->valuestring,
                sizeof(aiui->cfg.login.api_secret) - 1);

        cJSON* global = cJSON_GetObjectItem(cfg_root, "global");
        if (!cJSON_IsObject(global))
        {
            ret = -4;
            break;
        }
        cJSON* aiui_ver = cJSON_GetObjectItem(global, "aiui_ver");
        if (cJSON_IsString(aiui_ver) && NULL != aiui_ver->valuestring &&
            '\0' != aiui_ver->valuestring[0])
        {
            int aiui_ver_int = 0;
            EXTRACT_JSON_INT(aiui_ver_int, aiui_ver);
            aiui->cfg.global.scheme_version = (AIUI_SCHEME_VERSION)aiui_ver_int;
        }

        cJSON* scene = cJSON_GetObjectItem(global, "scene");
        if (cJSON_IsString(scene) && NULL != scene->valuestring &&
            '\0' != scene->valuestring[0])
        {
            strncpy(aiui->cfg.global.scene, scene->valuestring,
                    sizeof(aiui->cfg.global.scene) - 1);
        }
        cJSON* uid = cJSON_GetObjectItem(global, "uid");
        if (cJSON_IsString(uid) && NULL != uid->valuestring && '\0' != uid->valuestring[0])
        {
            strncpy(aiui->cfg.global.uid, uid->valuestring,
                    sizeof(aiui->cfg.global.uid) - 1);
        }
        if ('\0' == aiui->cfg.global.uid[0])
        {
            LOGE(AIUI_TAG, "should provide valid uid.");
            ret = -5;
            break;
        }

        cJSON* tts = cJSON_GetObjectItem(cfg_root, "tts");
        if (cJSON_IsObject(tts))
        {
            cJSON *vcn = cJSON_GetObjectItem(tts, "vcn");
            if (!cJSON_IsString(vcn))
            {
                vcn = cJSON_GetObjectItem(tts, "voice_name");//fallback use node "voice_name"
            }
            if (cJSON_IsString(vcn) && NULL != vcn->valuestring && '\0' != vcn->valuestring[0])
            {
                strncpy(aiui->cfg.tts.vcn, vcn->valuestring,
                        sizeof(aiui->cfg.tts.vcn) - 1);
            }
        }
    } while (0);
    if (cfg_root)
    {
        cJSON_Delete(cfg_root);
    }
    if (0 != ret)
    {
        return ret;
    }

    const bool need_setup_default_scene = ('\0' == aiui->cfg.global.scene[0]);
    const bool need_setup_default_vcn = ('\0' == aiui->cfg.tts.vcn[0]);
    switch (aiui->cfg.global.scheme_version)
    {
        case AIUI_SCHEME_VERSION_2:
        {
            aiui->ws_config.origin_host = AIUI_WS_ORIGIN_HOST;
            aiui->ws_config.host_no_wss = AIUI_WS_HOST_NO_WSS;
            aiui->ws_config.path_format = AIUI_WS_PATH_FORMAT;
            aiui->ws_config.request_line = AIUI_WS_REQUEST_LINE;
            if (need_setup_default_scene)
            {
                strncpy(aiui->cfg.global.scene, "main",
                        sizeof(aiui->cfg.global.scene) - 1);
            }
            if (need_setup_default_vcn)
            {
                strncpy(aiui->cfg.tts.vcn, "x2_xiaojuan",
                        sizeof(aiui->cfg.tts.vcn) - 1);
            }
        }
            break;
        case AIUI_SCHEME_VERSION_3:
        {
            aiui->ws_config.origin_host = AIUI_V3_WS_ORIGIN_HOST;
            aiui->ws_config.host_no_wss = AIUI_V3_WS_HOST_NO_WSS;
            aiui->ws_config.path_format = AIUI_V3_WS_PATH_FORMAT;
            aiui->ws_config.request_line = AIUI_V3_WS_REQUEST_LINE;
            if (need_setup_default_scene)
            {
                strncpy(aiui->cfg.global.scene, "sos_app",
                        sizeof(aiui->cfg.global.scene) - 1);
            }
            if (need_setup_default_vcn)
            {
                strncpy(aiui->cfg.tts.vcn, "x5_lingxiaoyue_flow",
                        sizeof(aiui->cfg.tts.vcn) - 1);
            }
        }
            break;
        default:
            ret = -6;
            break;
    }

    if (0 == ret)
    {
        LOGI(AIUI_TAG, "cfg loaded: ver=%d, scene=%s, uid=%s, vcn=%s",
            aiui->cfg.global.scheme_version, aiui->cfg.global.scene,
            aiui->cfg.global.uid, aiui->cfg.tts.vcn);
    }
    return ret;
}

AIUI_HANDLE aiui_init(const char *init_params, aiui_event event_cb, void *user_params)
{
    if (NULL == init_params || '\0' == init_params[0] || NULL == event_cb)
    {
        LOGE(AIUI_TAG, "aiui_event function can not be null");
        return NULL;
    }
    /*
     * The bundled cJSON otherwise uses libc malloc(), so every audio frame
     * fragments internal RAM even though AIUI protocol data is PSRAM-safe.
     */
    cJSON_Hooks json_hooks = {
        .malloc_fn = aiui_cjson_malloc,
        .free_fn = aiui_cjson_free,
    };
    cJSON_InitHooks(&json_hooks);
    int ret_code = -1;
    aiui_info_t *aiui = (aiui_info_t *)aiui_hal_malloc(sizeof(aiui_info_t));
    if (NULL == aiui)
    {
        return NULL;
    }
    memset(aiui, 0, sizeof(aiui_info_t));
    aiui->cfg.global.scheme_version = AIUI_SCHEME_VERSION_3;
    LOGV(AIUI_TAG, "init_params: %s", (NULL == init_params) ? "null" : init_params);

    if (0 != (ret_code = _aiui_parse_cfg(aiui, init_params)))
    {
        LOGE(AIUI_TAG, "failed(%d) on parse cfg", ret_code);
        aiui_hal_free(aiui);
        return NULL;
    }

    aiui_websocket_cb_t ws_callbacks = {_aiui_on_ws_connect_status_changed, _aiui_on_ws_message};
    aiui->on_event = event_cb;
    aiui->user_params = user_params;
    aiui->session_status = AIUI_STATUS_INIT;
    aiui->sem = aiui_hal_sem_create();
    aiui->queue_sem = aiui_hal_sem_create();
    aiui->queue_mutex = aiui_hal_mutex_create();
    aiui->queue_handle = aiui_create_queue();
    aiui->accepting_data = 0;
    aiui->websocket_handle = aiui_websocket_create(ws_callbacks, (void *)aiui);
    aiui_msg_parse_cb parse_cb = {_aiui_on_got_the_parsed_result};
    aiui->message_parse_handle = aiui_message_parse_init(parse_cb, (void *)aiui);
    aiui->tid = NULL;
    aiui_thread_param_t thread_param = {0};
    thread_param.name = "aiui_main_task";
    thread_param.deatch = 0;
    thread_param.priority = 5;
    thread_param.stack_size = 2048;
    aiui_hal_thread_create(&aiui->tid, &thread_param, _aiui_main_task, aiui);
    return (AIUI_HANDLE)aiui;
}

void aiui_uninit(AIUI_HANDLE handle)
{
    if (NULL == handle)
    {
        return;
    }
    aiui_info_t *aiui = (aiui_info_t *)handle;
    aiui->session_status = AIUI_STATUS_UNINIT;
    _aiui_set_accepting_data(aiui, 0);
    aiui_hal_sem_post(aiui->sem);
    aiui_hal_sem_post(aiui->queue_sem);
    aiui_hal_thread_destroy(aiui->tid);
    aiui_websocket_destroy(aiui->websocket_handle);
    aiui_hal_mutex_lock(aiui->queue_mutex);
    _aiui_clear_queue_unlocked(aiui);
    aiui_hal_mutex_unlock(aiui->queue_mutex);

    aiui_destroy_queue(aiui->queue_handle);
    aiui_hal_mutex_destroy(aiui->queue_mutex);
    aiui_hal_sem_destroy(aiui->sem);
    aiui_hal_sem_destroy(aiui->queue_sem);
    aiui_message_parse_uninit(aiui->message_parse_handle);
    aiui_hal_free(handle);
    handle = NULL;

    aiui_print_mem_info();
    LOGD(AIUI_TAG, "aiui uninit done");
}

static int _start_stop(AIUI_HANDLE handle, int typeof_start_or_stop)
{
    if (NULL == handle)
    {
        return -1;
    }
    aiui_info_t *aiui = (aiui_info_t *)handle;
    aiui_data_t* data =(aiui_data_t*)aiui_hal_malloc(sizeof(aiui_data_t));
    data->type = (AIUI_DATA_TYPE)typeof_start_or_stop;
    data->data = NULL;
    data->len = 0;
    return _aiui_replace_queue_with_control(aiui, data);
}

int aiui_start(AIUI_HANDLE handle)
{
    return _start_stop(handle, AIUI_DATA_START);
}

int aiui_stop(AIUI_HANDLE handle)
{
    return _start_stop(handle, AIUI_DATA_STOP);
}

int aiui_send_text(AIUI_HANDLE handle, const char *buf, const int len)
{
    if (NULL == handle || NULL == buf || len <= 0)
    {
        return -1;
    }
    aiui_info_t *aiui = (aiui_info_t *)handle;
    NOT_SUPPORT_FUNCTION_IN_SCHEME_V3(aiui);

    aiui_data_t* data =(aiui_data_t*)aiui_hal_malloc(sizeof(aiui_data_t));
    data->type = AIUI_DATA_TEXT;
    data->data = (char*)aiui_hal_malloc(len);
    memcpy(data->data, buf, len);
    data->len = len;
    return _aiui_enqueue_payload(aiui, data);
}

int aiui_send_audio(AIUI_HANDLE handle, const char *buf, const int len)
{
    if (NULL == handle)
    {
        return -1;
    }
    if (len < 0)
    {
        LOGE(AIUI_TAG, "send audio length(%d) is invalid", len);
        return -1;
    }
    aiui_info_t *aiui = (aiui_info_t *)handle;
    aiui_data_t* data =(aiui_data_t*)aiui_hal_malloc(sizeof(aiui_data_t));
    if (NULL == data)
    {
        LOGE(AIUI_TAG, "failed on allocate audio descriptor");
        return -1;
    }
    data->type = AIUI_DATA_AUDIO;
    data->data = NULL;
    if (len > 0)
    {
        data->data = (char*)aiui_hal_malloc(len);
        if (NULL == data->data)
        {
            LOGE(AIUI_TAG, "failed on allocate audio frame, len=%d", len);
            aiui_hal_free(data);
            return -1;
        }
        memcpy(data->data, buf, len);
    }
    data->len = len;
    return _aiui_enqueue_payload(aiui, data);
}

int aiui_send_tts(AIUI_HANDLE handle, const char *buf, const int len)
{
    if (NULL == handle || NULL == buf || len <= 0)
    {
        return -1;
    }
    aiui_info_t *aiui = (aiui_info_t *)handle;

    aiui_data_t* data =(aiui_data_t*)aiui_hal_malloc(sizeof(aiui_data_t));
    data->type = AIUI_DATA_TTS;
    data->data = (char*)aiui_hal_malloc(len);
    memcpy(data->data, buf, len);
    data->len = len;
    return _aiui_enqueue_payload(aiui, data);
}

const char *aiui_get_version()
{
    return "3.0.0_open-source";
}
