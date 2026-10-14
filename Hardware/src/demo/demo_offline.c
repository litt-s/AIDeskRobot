#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/unistd.h>
#include <assert.h>

#include "aiui_wrappers_os.h"
#include "aiui_api.h"
#include "aiui_log.h"

#define LOG_TAG "DEMO"

#ifndef NULLABLE_STR
#define NULLABLE_STR(x)  (NULL == (x) ? "null" : (x))
#endif // !NULLABLE_STR

// 测试识别的音频
#define TEST_AUDIO_FILE_PATH "../audio/1c16k16b.pcm"
#define TTS_FILE_PATH "./tts.pcm"

typedef struct
{
    AIUI_HANDLE obj;
    struct
    {
        bool connected;
        bool end_of_send_audio;
    } status;
    struct
    {
        FILE *fp_tts;
    } log;
} aiui_dev_context_t;


// AIUI交互协议版本
#ifndef AIUI_VER
#define AIUI_VER 3 // <-- 极速交互
// #define AIUI_VER 2
#endif

#if(!defined(AIUI_APP_ID)  || !defined(AIUI_APP_KEY) || !defined(AIUI_APP_SECRET))
#pragma message("should define AIUI_APP_ID/AIUI_APP_KEY/AIUI_APP_SECRET first! Or we will load key from aiui.json")
#endif

// 语音交互场景, 可以为空. 若为空字符串, v2 默认配为 "main"; v3 默认配为 "sos_app"
// 注意, 如果平台上的场景尚未正式发布, 调试情况下的场景后面应该带上 "_box", 例如: "sos_app_box"
#ifndef AIUI_SCENE
#define AIUI_SCENE ""
#endif

// 设备唯一id. 需保证每个设备唯一, 且字符串长度不能超过32字节.
// 通常可以采用设备SN号或者MAC地址
#ifndef AIUI_DEV_UID
#define AIUI_DEV_UID "12345678abcdefgh"
#endif

// 发音人信息,可以为空. 若为空字符串, v2 默认配为 "x2_xiaojuan"; v3 默认配为 "x5_lingxiaoyue_flow"
#ifndef AIUI_TTS_VOICE_NAME
#define AIUI_TTS_VOICE_NAME ""
#endif

static int run_aiui(int argc, char *argv[]);

// aiui 回调
static void process_aiui_event(const void *user_param, aiui_callback_event_t *event);

int main(int argc, char *argv[])
{
    printf("hello, argc=%d\n", argc);

    int ret = run_aiui(argc, argv);

    printf("\nbye bye... %d\n", ret);
    return ret;
}

static int run_aiui(int argc, char *argv[])
{
    int ret = -1;
    aiui_dev_context_t _dev_context = {NULL};
    aiui_dev_context_t *context = &_dev_context;
    memset(context, 0, sizeof(aiui_dev_context_t));

    const char *audio_path = TEST_AUDIO_FILE_PATH;
    const char *aiui_json_path = "./aiui.json";
    if (argc > 1)
    {
        if (strncasecmp("-h", argv[1], 2) == 0 || strncasecmp("--help", argv[1], 6) == 0)
        {
            printf("Usage: %s <audio_path> <aiui_json_path>\n", argv[0]);
            return 0;
        }

        audio_path = argv[1];
        if (argc > 2)
        {
            aiui_json_path = argv[2];
        }
    }
    LOGI(LOG_TAG, "aiui_json_path: \"%s\", audio_path: \"%s\"\n", aiui_json_path, audio_path);

    {
        char aiui_init_params[1024] = {0};
#if(!defined(AIUI_APP_ID)  || !defined(AIUI_APP_KEY) || !defined(AIUI_APP_SECRET))
        // 将 "./aiui.json" 文件内容填充到 aiui_init_params
        FILE *fp_aiui_json = fopen(aiui_json_path, "rb");
        if (NULL == fp_aiui_json)
        {
            LOGE(LOG_TAG, "failed on open \"%s\" file, need app info for auth.", aiui_json_path);
            return -1;
        }
        fseek(fp_aiui_json, 0, SEEK_END);
        int file_len = (int)ftell(fp_aiui_json);
        fseek(fp_aiui_json, 0, SEEK_SET);
        if (file_len >= (int)sizeof(aiui_init_params))
        {
            LOGE(LOG_TAG, "\"%s\" is too big. can't hold to aiui_init_params", aiui_json_path);
            fclose(fp_aiui_json);
            return -1;
        }
        fread(aiui_init_params, file_len, 1, fp_aiui_json);
        fclose(fp_aiui_json);
#else
        const char* aiui_init_params_fmt =
              "{\"login\":{\"appid\":\"%s\",\"key\":\"%s\",\"api_secret\":\"%s\"},"
              "\"global\":{\"scene\":\"%s\",\"aiui_ver\":\"%d\", \"uid\":\"%s\"},"
              "\"tts\":{\"voice_name\":\"%s\",\"data_encoding\":\"raw\"}}";
        snprintf(aiui_init_params, sizeof(aiui_init_params) - 1U, aiui_init_params_fmt,
            AIUI_APP_ID, AIUI_APP_KEY, AIUI_APP_SECRET,
            AIUI_SCENE, AIUI_VER, AIUI_DEV_UID,
            AIUI_TTS_VOICE_NAME);
#endif
        LOGI(LOG_TAG, "aiui_init_params: %s", aiui_init_params);
        if (NULL == (context->obj = aiui_init(aiui_init_params, &process_aiui_event, context)))
        {
            LOGE(LOG_TAG, "failed on init aiui");
            return -1;
        }
        LOGI(LOG_TAG, "succeed init aiui");
    }

    FILE *fp_audio = fopen(audio_path, "rb");
    do
    {
        // 推荐每次发送 1280 个字节, 也就是40ms的音频
        char audio_buffer[1280] = {0};
        if (NULL == fp_audio)
        {
            LOGE(LOG_TAG, "failed on open audio file: \"%s\"", audio_path);
            break;
        }
        fseek(fp_audio, 0L, SEEK_END);
        long total_audio_length = ftell(fp_audio);
        fseek(fp_audio, 0L, SEEK_SET);
        LOGD(LOG_TAG, "audio size = %ld", total_audio_length);
        if (total_audio_length < 0 || (size_t)total_audio_length < 2U * sizeof(audio_buffer))
        {
            LOGE(LOG_TAG, "audio file size(%ld) is too short", total_audio_length);
            break;
        }
        LOGI(LOG_TAG, "now send audio...");
        int wait_counter = 0;
        do
        {
            if (0 != (ret = aiui_start(context->obj)))
            {
                LOGE(LOG_TAG, "failed(%d) on start aiui", ret);
                break;
            }
            aiui_hal_sleep(100);
            if (++wait_counter > 30)
            {
                LOGE(LOG_TAG, "wait aiui connected timeout, check your network...");
                break;
            }
        } while (!context->status.connected);

        size_t audio_packet_count = 0;
        uint64_t last_send_time = 0, current_time = 0;
        aiui_hal_get_current_ms(&last_send_time);
        while (total_audio_length > 0)
        {
            if (!context->status.connected)
            {
                LOGE(LOG_TAG, "oops, not connected server...");
                break;
            }

            if (total_audio_length < sizeof(audio_buffer))
            {
                memset(audio_buffer, 0, sizeof(audio_buffer));
                fread(audio_buffer, total_audio_length, 1, fp_audio);
                total_audio_length = 0;
            }
            else
            {
                fread(audio_buffer, sizeof(audio_buffer), 1, fp_audio);
                total_audio_length -= sizeof(audio_buffer);
            }

#if 1
            ++audio_packet_count;
            aiui_hal_get_current_ms(&current_time);
            uint64_t cost_time = current_time - last_send_time;
            if (cost_time < 40U)
            {
                int to_sleep_ms = (int)(40U - cost_time);
                // LOGV(LOG_TAG, "sleep %dms for audio packet(%zu)", to_sleep_ms, audio_packet_count);
                aiui_hal_sleep(to_sleep_ms);
                aiui_hal_get_current_ms(&current_time);
            }
            else
            {
                LOGE(LOG_TAG, "send audio packt(%zu) cost too much: %lu, last=%lu, current=%lu",
                    audio_packet_count, cost_time, last_send_time, current_time);
            }
            last_send_time = current_time;
            // LOGV(LOG_TAG, "now try send audio packt(%zu) at: %lu", audio_packet_count, current_time);
#endif

// #define DEMO_TRACE_SEND_TIME
#ifdef DEMO_TRACE_SEND_TIME
            uint64_t trace_start_time, trace_end_time;
            aiui_hal_get_current_ms(&trace_start_time);
#endif
            if (0 != (ret = aiui_send_audio(context->obj, audio_buffer, sizeof(audio_buffer))))
            {
                LOGE(LOG_TAG, "failed(%d) on send audio", ret);
                break;
            }
#ifdef DEMO_TRACE_SEND_TIME
            aiui_hal_get_current_ms(&trace_end_time);
            uint64_t trace_cost_time = trace_end_time - trace_start_time;
            LOGV(LOG_TAG, "send audio packt(%zu) cost: %llu", audio_packet_count, trace_cost_time);
#endif
            // aiui_hal_sleep(40); // 音频量每次发送40ms, 这里模拟在线休眠一会. 真实使用录音不需要 sleep
        }
        LOGI(LOG_TAG, "end of send audio...");
        //  要发送的音频 已经全部发完了, 则发个结尾帧. 极速交互v3 也可以不发
        // aiui_send_audio(context->obj, "", 0);
        context->status.end_of_send_audio = true;
        aiui_hal_sleep(40); // 真实使用情况不需要sleep
    } while (0);

    if (fp_audio)
    {
        fclose(fp_audio);
    }
    if (context->log.fp_tts)
    {
        fclose(context->log.fp_tts);
        context->log.fp_tts = NULL;
    }
    if (context->obj)
    {
        aiui_uninit(context->obj);
    }
    LOGI(LOG_TAG, "end of run aiui: %d", ret);
    return ret;
}

static void process_cloud_event(const aiui_dev_context_t *self, const aiui_callback_event_t *event)
{
    const char *key_value_str = (const char *)event->result.params.key_data;
    if (NULL == key_value_str || '\0' == key_value_str[0])
    {
       return;
    }
    if (0 == strncmp(key_value_str, "Vad=", 4))
    {
        if (0 == strncmp(key_value_str + 4, "Silence", 8))
        {
            LOGI(LOG_TAG, "detected Silence, now stop current session.");
            /* aiui_stop(self->obj); // <-- 对于云端推送设备长时间静音的情况, 只能设备端自己停止当前会话, 如需交互再开启新会话
            if (!self->status.end_of_send_audio)
            {
                aiui_start(self->obj);// 这里我们为了持续交互, 直接开启新会话
            } */
        }
        else if (0 == strncmp(key_value_str + 4, "Bos", 4) ||
                 0 == strncmp(key_value_str + 4, "Eos", 4))
        {
            LOGI(LOG_TAG, "detected: %s", key_value_str); // Bos: 前端点; Eos: 后端点
        }
    }
    else
    {
        LOGW(LOG_TAG, "unhandled cloud.event: %s", key_value_str);
    }
}

static void process_aiui_event(const void *user_param, aiui_callback_event_t *event)
{
    aiui_dev_context_t *self = (aiui_dev_context_t *)user_param;
    switch (event->type)
    {
        case AIUI_EVENT_CONNECTED_TO_SERVER:
            self->status.connected = true;
            LOGI(LOG_TAG, "connected to server");
            break;
        case AIUI_EVENT_SERVER_DISCONNECTED:
            self->status.connected = false;
            LOGI(LOG_TAG, "server disconnected");
            break;
        case AIUI_EVENT_CONNECTED_TIMEOUT:
            self->status.connected = false;
            LOGI(LOG_TAG, "connected timeout");
            break;
        case AIUI_EVENT_ERROR:
        {
            LOGI(LOG_TAG, "error occurred. code=%d, detail=%s", event->general.code,
                NULLABLE_STR((const char *)event->general.message));
            if (0 != event->general.code)
            {
                aiui_stop(self->obj);// 这里通常是云端异常, 断开会话.
                if (!self->status.end_of_send_audio)
                {
                    aiui_start(self->obj);// 这里我们为了持续交互, 直接开启新会话
                }
                break;
            }
        }
            break;
        case AIUI_EVENT_RESULT:
        {
            aiui_event_result_t *result = &(event->result);
            assert(0 == result->session.code); // 云端错误会通过上面 AIUI_EVENT_ERROR 抛出

            const int dts = result->params.dts;
            if (0 == strncmp(result->params.sub, "event", 6))
            {
                LOGD(LOG_TAG, "event: dts=%d, stmid=%d, %s", dts,
                    result->session.stm_id, NULLABLE_STR((const char *)result->params.key_data));
                if (NULL == result->params.key_data ||
                    '\0' == ((const char *)result->params.key_data)[0])
                {
                    break;
                }
                process_cloud_event(self, event);
            }
            else if (0 == strncmp(result->params.sub, "iat", 4))
            {
                LOGD(LOG_TAG, "iat: dts=%d, %s\n  detail=%s\n", dts,
                    NULLABLE_STR((const char *)result->params.key_data),
                    NULLABLE_STR((const char *)result->data));
            }
            else if (0 == strncmp(result->params.sub, "cbm_tidy", 4))
            {
                LOGD(LOG_TAG, "cbm_tidy: dts=%d, %s", dts, NULLABLE_STR((const char *)result->params.key_data));
            }
            else if (0 == strncmp(result->params.sub, "cbm_semantic", 4))
            {
                LOGD(LOG_TAG, "cbm_semantic: dts=%d, %s", dts, NULLABLE_STR((const char *)result->params.key_data));
            }
            else if (0 == strncmp(result->params.sub, "nlp", 4))
            {
                LOGD(LOG_TAG, "nlp: dts=%d, %s", dts, NULLABLE_STR((const char *)result->params.key_data));
            }
            else if (0 == strncmp(result->params.sub, "tts", 4))
            {
                LOGD(LOG_TAG, "tts: dts=%d, size=%zu, audio=%p, detail=\n   %s\n", dts,
                    (result->params.key_data_size - 1U) /* 所有抛出的数据都补0了, 这里是音频二进制数据, 最后补的0不需要 */,
                    result->params.key_data,
                    NULLABLE_STR((const char *)result->data));
                if (NULL == self->log.fp_tts)
                {
                    self->log.fp_tts = fopen(TTS_FILE_PATH, "wb");
                }
                if (NULL == self->log.fp_tts)
                {
                    LOGE(LOG_TAG, "failed on open tts file: %s", TTS_FILE_PATH);
                    break;
                }
                fwrite(result->params.key_data, 1U, result->params.key_data_size - 1U, self->log.fp_tts);
            }
            else
            {
                LOGW(LOG_TAG, "unhandled sub: %s", result->params.sub);
            }
        }
            break;
        default:
            break;
    }
}
