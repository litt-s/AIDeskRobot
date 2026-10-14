#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/unistd.h>
#include <assert.h>

#include "aiui_wrappers_os.h"
#include "aiui_api.h"
#include "aiui_log.h"

#if (ENABLE_PORTAUDIO)
#include "audio_recorder/audio_recorder.h"
#include "audio_player/audio_player.h"
#endif

#define LOG_TAG "AIUI_DEMO"

#ifndef NULLABLE_STR
#define NULLABLE_STR(x)  (NULL == (x) ? "null" : (x))
#endif // !NULLABLE_STR

// 测试识别的音频
#define TEST_AUDIO_FILE_PATH "../audio/1c16k16b.pcm"
#define TTS_FILE_PATH "./tts.pcm"
#define RECORD_FILE_PATH "./record.pcm"

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

#if (ENABLE_PORTAUDIO)
    AUDIO_RECORDER_HANDLE obj_recorder;
    AUDIO_PLAYER_HANDLE obj_player;
    FILE *fp_recorded = NULL;
#endif
} aiui_dev_context_t;

#if (ENABLE_PORTAUDIO)
static void process_captured_audio(const void *user_param, const void *audio, int len);
#endif

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

static int run_aiui();

// aiui 回调
static void process_aiui_event(const void *user_param, aiui_callback_event_t *event);

int main(int argc, char *argv[])
{
    printf("hello, argc=%d\n", argc);

    int ret = run_aiui();

    printf("\nbye bye... %d\n", ret);
    return 0;
}

static int run_aiui()
{
    int ret = -1;
    aiui_dev_context_t _dev_context;
    aiui_dev_context_t *context = &_dev_context;
    memset(context, 0, sizeof(aiui_dev_context_t));

    {
        char aiui_init_params[1024] = {0};
#if(!defined(AIUI_APP_ID)  || !defined(AIUI_APP_KEY) || !defined(AIUI_APP_SECRET))
        // 将 "./aiui.json" 文件内容填充到 aiui_init_params
        FILE *fp_aiui_json = fopen("./aiui.json", "rb");
        if (NULL == fp_aiui_json)
        {
            LOGE(LOG_TAG, "failed on open \"./aiui.json\" file, need app info for auth.");
            return -1;
        }
        fseek(fp_aiui_json, 0, SEEK_END);
        const int cfg_file_len = (int)ftell(fp_aiui_json);
        fseek(fp_aiui_json, 0, SEEK_SET);
        if (cfg_file_len >= (int)sizeof(aiui_init_params))
        {
            LOGE(LOG_TAG, "aiui.json is too big. can't hold to aiui_init_params");
            fclose(fp_aiui_json);
            return -1;
        }
        fread(aiui_init_params, cfg_file_len, 1, fp_aiui_json);
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
    }

    do
    {
        if (0 != audio_player_init(&context->obj_player))
        {
            LOGE(LOG_TAG, "failed on init player");
            break;
        }
        if (0 != audio_player_start(context->obj_player))
        {
            LOGE(LOG_TAG, "failed on start player");
            break;
        }
        if (NULL == (context->obj_recorder = audio_recorder_init(process_captured_audio ,context)))
        {
            LOGE(LOG_TAG, "failed on init recorder");
            break;
        }

        LOGI(LOG_TAG, "succeed init recorder/player");
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
        if (!context->status.connected)
        {
            LOGE(LOG_TAG, "connect server timeout, break the chat");
            break;
        }
        if (0!= audio_recorder_start(context->obj_recorder))
        {
            LOGE(LOG_TAG, "failed on start recorder, break the chat");
            break;
        }

        printf("\n\n press \"q\" to end of chat... \n\n");
        while (getchar() != 'q')
        {
            // nothing
        }
        LOGI(LOG_TAG, "break the chat...\n");
        context->status.end_of_send_audio = true;
        //  要发送的音频 已经全部发完了, 则发个结尾帧. 极速交互v3 也可以不发
        aiui_send_audio(context->obj, "", 0);
        // aiui_hal_sleep(40); // 真实使用情况不需要sleep
        LOGI(LOG_TAG, "end of send audio...");
    } while (0);
    if (context->obj_recorder)
    {
        audio_recorder_uninit(context->obj_recorder);
        context->obj_recorder = NULL;
    }
    if (context->obj)
    {
        aiui_uninit(context->obj);
        context->obj = NULL;
    }
    if (context->obj_player)
    {
        audio_player_stop(context->obj_player);
        audio_player_uninit(context->obj_player);
        context->obj_player = NULL;
    }
    if (context->log.fp_tts)
    {
        fclose(context->log.fp_tts);
        context->log.fp_tts = NULL;
    }
    if (context->fp_recorded)
    {
        fclose(context->fp_recorded);
        context->fp_recorded = NULL;
    }
    LOGI(LOG_TAG, "end of run aiui: %d", ret);
    return ret;
}

static void process_cloud_event(const aiui_dev_context_t* self, const aiui_callback_event_t* event)
{
    const char* key_value_str = (const char*)event->result.params.key_data;
    if (NULL == key_value_str || '\0' == key_value_str[0])
    {
        return;
    }
    if (0 != strncmp(key_value_str, "Vad=", 4))
    {
        LOGE(LOG_TAG, "unhandled cloud.event.type: %s", key_value_str);
        return;
    }

    if (0 == strncmp(key_value_str + 4, "Silence", 8))
    {
        LOGI(LOG_TAG, "detected Cloud Silence, now stop current session.");
        aiui_stop(self->obj); // <-- 对于云端推送设备长时间静音的情况, 只能设备端自己停止当前会话, 如需交互再开启新会话
        if (!self->status.end_of_send_audio)
        {
            aiui_start(self->obj); // 这里我们为了持续交互, 直接开启新会话
        }
    }
    else if (0 == strncmp(key_value_str + 4, "Bos", 4)) // Bos: 人声前端点
    {
        LOGI(LOG_TAG, "detected: Cloud Bos");
        if (self->obj_player)
        {
            if (0 == audio_player_stop(self->obj_player))
            {
                LOGI(LOG_TAG, "succeed stop player");
            }
            else
            {
                LOGE(LOG_TAG, "failed on stop player");
            }

            if (0 == audio_player_start(self->obj_player))
            {
                LOGI(LOG_TAG, "succeed start player");
            }
            else
            {
                LOGE(LOG_TAG, "failed on start player");
            }
        }
    }
    else if (0 == strncmp(key_value_str + 4, "Eos", 4)) //  Eos: 人声后端点
    {
        LOGI(LOG_TAG, "detected: Cloud Eos");
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
                LOGD(LOG_TAG, "tts: dts=%d, size=%zu, audio=%p, detail=\n%s\n", dts,
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
                if (self->obj_player)
                {
                    audio_player_write(self->obj_player,
                                       result->params.key_data,
                                       result->params.key_data_size - 1U);
                }
            }
            else
            {
                LOGW(LOG_TAG, "unhandled sub: %s", result->params.sub);
            }
        } // AIUI_EVENT_RESULT
            break;
        default:
            LOGW(LOG_TAG, "unhandled event.type=%d", event->type);
            break;
    }
}

#if (ENABLE_PORTAUDIO)

static void process_captured_audio(const void *user_param, const void *audio, int len)
{
    aiui_dev_context_t *context = (aiui_dev_context_t *)user_param;
    if (NULL == context)
    {
        return;
    }
    if (NULL == context->obj || !context->status.connected || context->status.end_of_send_audio)
    {
        return;
    }
    if (NULL == context->fp_recorded)
    {
        context->fp_recorded = fopen(RECORD_FILE_PATH, "wb");
        if (NULL == context->fp_recorded)
        {
            LOGE(LOG_TAG, "failed on open record file: %s", RECORD_FILE_PATH);
            return;
        }
    }
    fwrite(audio, 1, len, context->fp_recorded);
    if (1280 != len)
    {
        LOGE(LOG_TAG, "should be 1280 byte. but we got %d", len);
        return; // should be 1280 byte.
    }

    if (0 != aiui_send_audio(context->obj, (const char *)audio, len))
    {
        LOGE(LOG_TAG, "failed on aiui_send_audio");
    }
}

#endif

