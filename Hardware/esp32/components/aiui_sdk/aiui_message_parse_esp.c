#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "cjson.h"
#include "base64.h"
#include "aiui_log.h"
#include "aiui_wrappers_os.h"
// #include "aiui_media_parse.h"
// #include "aiui_skill_parse.h"
#include "aiui_message_parse.h"

#define AIUI_TAG "aiui_msg"
#include "cjson_helper.h"

// 是否 拼接 nlp/iat 结果字符串
#define AIUI_CONFIG_SPLICING_STRINGS  1

typedef struct aiui_message_parse_s
{
    aiui_msg_parse_cb cb;
    void* cb_param;

    /* session 中拼接字符串 */
    struct
    {
        // 当前 iat.text 拼接的内容, 注意要释放
        char *iat_text;
        // 当前nlp.text的内容, 注意要释放
        char *nlp_text;
    } session_slicing_rlt;

    aiui_event_result_t cur_result;
} aiui_message_parse_t;


// ==============================================================================================


// ===================== AIUI_RESULT =====================

// 清理业务申请的内存
#define _CLEANUP_AIUI_CUR_RESULT_SUB(self)              \
  if (self->cur_result.params.key_data)                 \
  {                                                     \
      aiui_hal_free(self->cur_result.params.key_data);  \
      self->cur_result.params.key_data = NULL;          \
  }                                                     \
  self->cur_result.params.key_data_size = 0;            \
  if (self->cur_result.data)                            \
  {                                                     \
      aiui_hal_free(self->cur_result.data);             \
      self->cur_result.data = NULL;                     \
  }                                                     \
  self->cur_result.data_size = 0;                       \
  memset(&(self->cur_result.params), 0,                 \
         sizeof(aiui_event_result_param_t));

// 重置 result
#define _RESET_AIUI_CUR_RESULT(self)                           \
  _CLEANUP_AIUI_CUR_RESULT_SUB(self)                           \
  memset(&(self->cur_result), 0, sizeof(aiui_event_result_t)); \
  /* 设置默认的-1, 意味着没提取到code. 云端错误码一般是正4位数以上 */     \
  self->cur_result.session.code = -1;

// ===================== AIUI_RESULT =====================


// ===================== DECODE =====================

// 定义解码变量
#define _DEFINE_DECODE_VAR(NODE_NAME)   \
  char *decode_##NODE_NAME = NULL;      \
  size_t decode_##NODE_NAME##_size = 0;

// 解码
#define _DECODE_OR_BREAK(CJSON_OBJECT, NODE_NAME)                                       \
  cJSON* item_##NODE_NAME = cJSON_GetObjectItem(CJSON_OBJECT, #NODE_NAME);              \
  if (NULL == item_##NODE_NAME || !cJSON_IsString(item_##NODE_NAME))                    \
  {                                                                                     \
      break;                                                                            \
  }                                                                                     \
  if ('\0' == item_##NODE_NAME->valuestring[0])                                         \
  {                                                                                     \
      decode_##NODE_NAME = (char *)aiui_hal_malloc(1);                                  \
      decode_##NODE_NAME[0] = '\0';                                                     \
  }                                                                                     \
  else                                                                                  \
  {                                                                                     \
      const size_t encoded_strlen_##NODE_NAME = strlen(item_##NODE_NAME->valuestring);  \
      decode_##NODE_NAME = (char*)aiui_hal_malloc(encoded_strlen_##NODE_NAME + 1);      \
      memset(decode_##NODE_NAME, 0, encoded_strlen_##NODE_NAME + 1);                    \
      if (mbedtls_base64_decode((unsigned char*)decode_##NODE_NAME,                     \
                                encoded_strlen_##NODE_NAME + 1U,                        \
                                &(decode_##NODE_NAME##_size),                           \
                                (const unsigned char*)(item_##NODE_NAME->valuestring),  \
                                encoded_strlen_##NODE_NAME) != 0)                       \
      {                                                                                 \
          aiui_hal_free(decode_##NODE_NAME);                                            \
          decode_##NODE_NAME = NULL;                                                    \
          LOGE(AIUI_TAG, "failed on decode (%u encoded bytes)",                       \
               (unsigned)encoded_strlen_##NODE_NAME);                                  \
          break;                                                                        \
      }                                                                                 \
      /* LOGV(AIUI_TAG, "decode: size=zu, %s", decode_##NODE_NAME##_size, decode_##NODE_NAME);  */     \
  }

// 清理解码变量
#define _CLEAN_DECODE_VAR(NODE_NAME)     \
  if (NULL != decode_##NODE_NAME)        \
  {                                      \
      aiui_hal_free(decode_##NODE_NAME); \
      decode_##NODE_NAME = NULL;         \
  }                                      \
  decode_##NODE_NAME##_size = 0;

// ===================== DECODE =====================

// ===================== CPY2DATA =====================
// 拷贝到指定位置
#define _INTERNAL_CPY_TO_RESULT_MEMBER(DEST_PTR_MEMBER, DEST_MEMBER_SIZE,      \
                                       SRC_DATA, SRC_DATA_SIZE)                \
  do { if (NULL != (DEST_PTR_MEMBER))                                          \
       {                                                                       \
           aiui_hal_free((DEST_PTR_MEMBER));                                   \
           (DEST_PTR_MEMBER) = NULL;                                           \
           (DEST_MEMBER_SIZE) = 0;                                             \
       }                                                                       \
       (DEST_MEMBER_SIZE) = (SRC_DATA_SIZE) + 1U;                              \
       (DEST_PTR_MEMBER) = (char *)aiui_hal_malloc(DEST_MEMBER_SIZE);          \
       ((char *)(DEST_PTR_MEMBER))[(DEST_MEMBER_SIZE) - 1U] =  '\0';           \
       memcpy((char*)(DEST_PTR_MEMBER), (SRC_DATA), (DEST_MEMBER_SIZE) - 1U);  \
} while (0)

// 复制指定 内容 到 aiui_event_result_t.data 中
#define _CPY_TO_RESULT_DATA(self, SRC_DATA, SRC_DATA_SIZE)                \
  _INTERNAL_CPY_TO_RESULT_MEMBER(self->cur_result.data,                   \
                                 self->cur_result.data_size,              \
                                 (SRC_DATA), (SRC_DATA_SIZE))

// 复制指定 内容 到 aiui_event_result_t.params.key_data 中
#define _CPY_TO_RESULT_KEY_DATA(self, SRC_DATA, SRC_DATA_SIZE)            \
  _INTERNAL_CPY_TO_RESULT_MEMBER(self->cur_result.params.key_data,        \
                                 self->cur_result.params.key_data_size,   \
                                 (SRC_DATA), (SRC_DATA_SIZE))

// ===================== CPY2DATA =====================

// ==============================================================================================

AIUI_MESSAGE_PARSE_HANDLE aiui_message_parse_init(aiui_msg_parse_cb cb, void* user_param)
{
    aiui_message_parse_t* handle =
        (aiui_message_parse_t*)aiui_hal_malloc(sizeof(aiui_message_parse_t));
    if (NULL == handle)
    {
        return NULL;
    }
    memset(handle, 0, sizeof(aiui_message_parse_t));
    handle->cb = cb;
    handle->cb_param = user_param;
    return (AIUI_MESSAGE_PARSE_HANDLE)handle;
}

static int _aiui_parse_iat(aiui_message_parse_t* self, const cJSON *payload_obj, cJSON* iat_obj)
{
    /*
       {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 1,
            "status": 2,
            "text": "{\"text\":{\"sn\":1,\"ls\":false,\"bg\":0,\"rg\":null,\"ed\":0,\"voice\":[{\"sty\":\"\\u003c说话\\u003e\",\"lev\":\"\",\"description\":\"\"}],\"pgs\":\"\",\"rst\":\"\",\"sign\":\"\",\"ws\":[{\"bg\":0,\"cw\":[{\"sc\":0,\"w\":\"你好。\",\"ph\":\"\"}]}]}}"
        }
     */
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(text);
    cJSON *sub_text_obj = NULL;
    do
    {
        _DECODE_OR_BREAK(iat_obj, text);
        cJSON_ReplaceItemInObject(iat_obj, "text", cJSON_CreateString(decode_text));

        char *new_iat_json = cJSON_PrintUnformatted(iat_obj);
        _CPY_TO_RESULT_DATA(self, new_iat_json, strlen(new_iat_json));
        cJSON_free(new_iat_json);

        if (NULL == (sub_text_obj = cJSON_Parse(decode_text)))
        {
            break;
        }
        // 解析 iat 放到 key_data 中.
        cJSON *text_text_obj = NULL;
        if (NULL == (text_text_obj = cJSON_GetObjectItem(sub_text_obj, "text")))
        {
           break;
        }
        cJSON* ws = cJSON_GetObjectItem(text_text_obj, "ws");
        if (NULL == ws)
        {
            break;
        }

        const int ws_size = cJSON_GetArraySize(ws);
        for (int i = 0; i < ws_size; ++i)
        {
            cJSON* ws_i = cJSON_GetArrayItem(ws, i);
            cJSON* cw = NULL;
            if (NULL == ws_i || NULL == (cw = cJSON_GetObjectItem(ws_i, "cw")))
            {
                continue;
            }
            const int cw_size = cJSON_GetArraySize(cw);
            if (cw_size <= 0)
            {
                continue;
            }
            cJSON* cw_0 = cJSON_GetArrayItem(cw, 0);
            cJSON* w = NULL;
            if (NULL == cw_0 || NULL == (w = cJSON_GetObjectItem(cw_0, "w")))
            {
                continue;
            }
            if (NULL != w->valuestring && '\0' != w->valuestring[0])
            {
                self->session_slicing_rlt.iat_text = aiui_append_string(self->session_slicing_rlt.iat_text, w->valuestring);
            }
        }
        _CPY_TO_RESULT_KEY_DATA(self, (NULL == self->session_slicing_rlt.iat_text ? "" : self->session_slicing_rlt.iat_text),
            (NULL == self->session_slicing_rlt.iat_text ? 0 : strlen(self->session_slicing_rlt.iat_text)));
        if (NULL != self->session_slicing_rlt.iat_text
#if(defined(AIUI_CONFIG_SPLICING_STRINGS) && 0 != AIUI_CONFIG_SPLICING_STRINGS)
            && (2 == self->cur_result.params.dts || 3 == self->cur_result.params.dts))
#else
        )
#endif
        {
            aiui_hal_free(self->session_slicing_rlt.iat_text);
            self->session_slicing_rlt.iat_text = NULL;
        }
    } while (0);
    if (sub_text_obj)
    {
        cJSON_Delete(sub_text_obj);
    }
    _CLEAN_DECODE_VAR(text);

    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

/*
static int _aiui_parse_intent_split(aiui_message_parse_t* handle, const cJSON* intent_split)
{
    if (NULL == intent_split || NULL == handle)
        return 0;
    if (!cJSON_HasObjectItem(intent_split, "text"))
    {
        return 0;
    }
    cJSON* text = cJSON_GetObjectItem(intent_split, "text");
    if (!cJSON_IsString(text))
    {
        return -1;
    }
    if ('\0' == text->valuestring[0])
    {
        return 0;
    }
    int inlen = strlen(text->valuestring);

    char* output = (char*)aiui_hal_malloc(inlen + 1);
    memset(output, 0, inlen + 1);
    size_t out_len = 0;
    if (mbedtls_base64_decode((unsigned char*)output, inlen, &out_len,
                              (const unsigned char*)text->valuestring, inlen) != 0)
    {
        aiui_hal_free(output);
        return -1;
    }
    cJSON* result_decode = cJSON_Parse(output);
    if (NULL == result_decode || cJSON_IsInvalid(result_decode))
    {
        aiui_hal_free(output);
        return -1;
    }
    printf("output = %s\n", output);
    aiui_hal_free(output);
    cJSON* intent = cJSON_GetObjectItem(result_decode, "intent");
    if (cJSON_IsString(intent))
    {
        cJSON_Delete(result_decode);
        return -1;
    }
    int intent_size = cJSON_GetArraySize(intent);
    handle->cb.intent_split_num(intent_size, handle->cb_param);
    for (int i = 0; i < intent_size; i++)
    {
        cJSON* item = cJSON_GetArrayItem(intent, i);
        cJSON* index = cJSON_GetObjectItem(item, "index");
        cJSON* value = cJSON_GetObjectItem(item, "value");
        handle->cb.intent_split(index->valueint, value->valuestring, handle->cb_param);
    }
    cJSON_Delete(result_decode);
    return 0;
}
*/

static int _aiui_parse_cbm_meta(const cJSON* payload_obj, const char *sub_name, int *intent_index_p)
{
    int ret = -1;
    cJSON *cbm_meta_obj = NULL;
    if (NULL == (cbm_meta_obj = cJSON_GetObjectItem(payload_obj, "cbm_meta")))
    {
        return -1;
    }

    _DEFINE_DECODE_VAR(text);
    cJSON* decode_text_root = NULL;
    do
    {
        _DECODE_OR_BREAK(cbm_meta_obj, text);
        //cJSON_ReplaceItemInObject(cbm_meta_obj, "text", cJSON_CreateString(decode_text));

        if (NULL == (decode_text_root = cJSON_Parse(decode_text)))
        {
            LOGE(AIUI_TAG, "failed parse cbm_meta.text");
            break;
        }
        cJSON* cbm_item = NULL;
        if (NULL == (cbm_item = cJSON_GetObjectItem(decode_text_root, sub_name)))
        {
            LOGE(AIUI_TAG, "not found \"%s\" in cbm_meta", sub_name);
            break;
        }
        EXTRACT_JSON_INT_BY_KEY(*intent_index_p, cbm_item, intent);
        ret = 0;
    } while (0);
    if (NULL != decode_text_root)
    {
        cJSON_Delete(decode_text_root);
    }
    _CLEAN_DECODE_VAR(text);
    return ret;
}

static int _aiui_parse_parameter_intent(const cJSON *parameter_obj,
                                        const char *sub_name,
                                        int *intent_index_p)
{
    if (NULL == parameter_obj || NULL == sub_name || NULL == intent_index_p)
    {
        return -1;
    }

    cJSON *sub_obj = cJSON_GetObjectItem(parameter_obj, sub_name);
    cJSON *loc_obj = NULL;
    cJSON *intent_obj = NULL;
    if (NULL == sub_obj ||
        NULL == (loc_obj = cJSON_GetObjectItem(sub_obj, "loc")) ||
        NULL == (intent_obj = cJSON_GetObjectItem(loc_obj, "intent")) ||
        !cJSON_IsNumber(intent_obj))
    {
        return -1;
    }

    *intent_index_p = intent_obj->valueint;
    return 0;
}

static int _aiui_parse_cbm_semantic(aiui_message_parse_t* self, const cJSON *payload_obj, cJSON* cbm_semantic_obj)
{
    /*
    {
    "header": {
        "cid": null,
        "code": 0,
        "message": "success",
        "sid": "xgo000d17a3@dx19400b1aaf40001822-audio-3",
        "status": 1,
        "stmid": "audio-3"
    },
    "payload": {
        "cbm_meta": {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 0,
            "status": 3,
            "text": "{\"cbm_semantic\":{\"intent\":0}}"
        },
        "cbm_semantic": {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 0,
            "status": 3,
            "text": "{\"rc\":4,\"sid\":\"xgo000d17a3@dx19400b1aaf40001822-audio-3\",\"text\":\"你好。\",\"uuid\":\"xgo000d17a3@dx19400b1aaf40001822-audio-3\"}"
        }
    }
   }
   */
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(text);
    cJSON* decode_text_root = NULL;
    do
    {
        _DECODE_OR_BREAK(cbm_semantic_obj, text);
        cJSON_ReplaceItemInObject(cbm_semantic_obj, "text", cJSON_CreateString(decode_text));

        char *new_semantic_json = cJSON_PrintUnformatted(cbm_semantic_obj);
        _CPY_TO_RESULT_DATA(self, new_semantic_json, strlen(new_semantic_json));
        cJSON_free(new_semantic_json);

        if (NULL == (decode_text_root = cJSON_Parse(decode_text)))
        {
            LOGE(AIUI_TAG, "failed parse cbm_semantic.text");
            break;
        }
        int rc = -1;
        EXTRACT_JSON_INT_BY_KEY(rc, decode_text_root, rc);
        cJSON* item_obj = NULL;
        do // <-- parse answer in condition (0 == rc)
        {
            if (0 != rc)
            {
                break; // only parse answer in condition (0 == rc)
            }
            cJSON* answer_text_obj = NULL;
            if (NULL == (item_obj = cJSON_GetObjectItem(decode_text_root, "answer")) ||
                NULL == (answer_text_obj = cJSON_GetObjectItem(item_obj, "text")))
            {
                LOGW(AIUI_TAG, "no parse cbm_semantic.text.answer");
                break;
            }
            _CPY_TO_RESULT_KEY_DATA(self, answer_text_obj->valuestring,
                                    strlen(answer_text_obj->valuestring));
        } while (0);
        if (NULL != self->cur_result.params.key_data &&
            '\0' != ((char *)self->cur_result.params.key_data)[0])
        {
            break; // already got answer, break the chain.
        }

        // (0 != rc) OR no answer: extract cbm_semantic.text.text
        if (NULL == (item_obj = cJSON_GetObjectItem(decode_text_root, "text")))
        {
            LOGE(AIUI_TAG, "no parse cbm_semantic.text.text");
            break;
        }
        _CPY_TO_RESULT_KEY_DATA(self, item_obj->valuestring, strlen(item_obj->valuestring));
    } while (0);
    if (NULL != decode_text_root)
    {
        cJSON_Delete(decode_text_root);
    }
    _CLEAN_DECODE_VAR(text);

    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

static int _aiui_parse_tidy(aiui_message_parse_t* self,
                            const cJSON *payload_obj,
                            const cJSON *parameter_obj,
                            cJSON* tidy_obj)
{
    /*
    {
    "header": {
        "cid": null,
        "code": 0,
        "message": "success",
        "sid": "xgo000d17a3@dx19400b1aaf40001822-audio-3",
        "status": 1,
        "stmid": "audio-3"
    },
    "payload": {
        "cbm_meta": {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 0,
            "status": 2,
            "text": "{\"cbm_tidy\":{\"intent\":0}}"
        },
        "cbm_tidy": {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 0,
            "status": 2,
            "text": "{\"query\":\"你好。\",\"intent\":[{\"index\":0,\"value\":\"你好。\"}]}"
        }
     }
    }
     */
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(text);
    cJSON* decode_text_root = NULL;
    bool succeed_found_intent = false;
    do
    {
        _DECODE_OR_BREAK(tidy_obj, text);
        cJSON_ReplaceItemInObject(tidy_obj, "text", cJSON_CreateString(decode_text));

        char *new_tidy_json = cJSON_PrintUnformatted(tidy_obj);
        _CPY_TO_RESULT_DATA(self, new_tidy_json, strlen(new_tidy_json));
        cJSON_free(new_tidy_json);

        if (NULL == (decode_text_root = cJSON_Parse(decode_text)))
        {
            LOGE(AIUI_TAG, "failed parse cmb_tidy.text");
            break;
        }
        cJSON *intent_obj = NULL;
        if (NULL == (intent_obj = cJSON_GetObjectItem(decode_text_root, "intent")))
        {
            LOGE(AIUI_TAG, "failed parse cbm_tidy.text.intent");
            break;
        }

        int target_intent_index = -1;
        if (0 != _aiui_parse_parameter_intent(parameter_obj, "cbm_tidy",
                                              &target_intent_index) &&
            0 != _aiui_parse_cbm_meta(payload_obj, "cbm_tidy",
                                      &target_intent_index))
        {
            LOGE(AIUI_TAG, "failed on parse intent metadata for cbm_tidy");
            break;
        }
        if (target_intent_index < 0)
        {
            LOGE(AIUI_TAG, "invalid intent index for cbm_tidy: %d",
                 target_intent_index);
            break;
        }
        LOGV(AIUI_TAG, "cbm_tidy.index=%d", target_intent_index);
        const int intent_size = cJSON_GetArraySize(intent_obj);
        if (intent_size <= target_intent_index)
        {
            break;
        }
        cJSON *intent_item = NULL;
        for (int intent_index =0; intent_index < intent_size; ++intent_index)
        {
            int cur_intent_index = -1;
            if (NULL == (intent_item = cJSON_GetArrayItem(intent_obj, intent_index)))
            {
                break;
            }
            EXTRACT_JSON_INT_BY_KEY(cur_intent_index, intent_item, index);
            if (cur_intent_index != target_intent_index)
            {
                continue;
            }
            succeed_found_intent = true;
            cJSON *intent_value_item = NULL;
            if (NULL == (intent_value_item = cJSON_GetObjectItem(intent_item, "value")))
            {
                LOGE(AIUI_TAG, "not found intent.value");
                break;
            }
            _CPY_TO_RESULT_KEY_DATA(self, intent_value_item->valuestring, strlen(intent_value_item->valuestring));
            break;
        }
    } while (0);
    if (!succeed_found_intent && NULL != decode_text)
    {
        _CPY_TO_RESULT_KEY_DATA(self, decode_text, decode_text_size);
    }
    if (NULL != decode_text_root)
    {
        cJSON_Delete(decode_text_root);
    }
    _CLEAN_DECODE_VAR(text);

    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

static int _aiui_parse_nlp(aiui_message_parse_t* self, const cJSON *payload_obj, cJSON* nlp_obj)
{
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(text);
    do
    {
        _DECODE_OR_BREAK(nlp_obj, text);
        cJSON_ReplaceItemInObject(nlp_obj, "text", cJSON_CreateString(decode_text));

        char *new_nlp_json = cJSON_PrintUnformatted(nlp_obj);
        _CPY_TO_RESULT_DATA(self, new_nlp_json, strlen(new_nlp_json));
        cJSON_free(new_nlp_json);

#if(defined(AIUI_CONFIG_SPLICING_STRINGS) && 0 != AIUI_CONFIG_SPLICING_STRINGS)  // <-- 我们拼接 nlp.text. 关闭此宏, 则由客户拼接.
        if (0 == self->cur_result.params.dts && NULL != self->session_slicing_rlt.nlp_text)
        {
            aiui_hal_free(self->session_slicing_rlt.nlp_text);
            self->session_slicing_rlt.nlp_text = NULL;
        }
        self->session_slicing_rlt.nlp_text = aiui_append_string(self->session_slicing_rlt.nlp_text, decode_text);
        _CPY_TO_RESULT_KEY_DATA(self, NULL == self->session_slicing_rlt.nlp_text ? "" : self->session_slicing_rlt.nlp_text,
            NULL == self->session_slicing_rlt.nlp_text ? 0: strlen(self->session_slicing_rlt.nlp_text));
        if ( NULL != self->session_slicing_rlt.nlp_text &&
            (2 == self->cur_result.params.dts || 3 == self->cur_result.params.dts))
        {
            aiui_hal_free(self->session_slicing_rlt.nlp_text);
            self->session_slicing_rlt.nlp_text = NULL;
        }
#else
        _CPY_TO_RESULT_KEY_DATA(self, decode_text, decode_text_size);
#endif
    } while (0);
    _CLEAN_DECODE_VAR(text);

    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

static int _aiui_parse_tts(aiui_message_parse_t* self, const cJSON *payload_obj, cJSON* tts_obj)
{
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(audio);
    do
    {
        _DECODE_OR_BREAK(tts_obj, audio);
#if 1 // <-- 默认替换base64音频节点内容, 若外部需要自己解, 关闭此宏即可
        cJSON_ReplaceItemInObject(tts_obj, "audio", cJSON_CreateString("base64_audio_placeholder"));
#endif

        char *decoded_json = cJSON_PrintUnformatted(tts_obj);
        _CPY_TO_RESULT_DATA(self, decoded_json, strlen(decoded_json));
        cJSON_free(decoded_json);

        _CPY_TO_RESULT_KEY_DATA(self, decode_audio, decode_audio_size);
    } while (0);
    _CLEAN_DECODE_VAR(audio);

    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

static int _aiui_parse_facial_expression(aiui_message_parse_t* self,
                                         const cJSON *payload_obj,
                                         cJSON *expression_obj)
{
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(text);
    do
    {
        _DECODE_OR_BREAK(expression_obj, text);
        _CPY_TO_RESULT_KEY_DATA(self, decode_text, decode_text_size);
    } while (0);
    _CLEAN_DECODE_VAR(text);
    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

//=============================================================================

static int _aiui_parse_cloud_event(aiui_message_parse_t* self, const cJSON *payload_obj, cJSON *cloud_event_obj)
{
   // event:  {"compress":"","encoding":"","format":"","seq":0,"status":0,"text":"{\"type\":\"Vad\",\"data\":\"\",\"key\":\"Bos\",\"desc\":{}}"}
    UNUSED(payload_obj);
    _DEFINE_DECODE_VAR(text);
    cJSON *event_text_cjson = NULL;
    do
    {
        _DECODE_OR_BREAK(cloud_event_obj, text);
        cJSON_ReplaceItemInObject(cloud_event_obj, "text", cJSON_CreateString(decode_text));

        char *new_event_json = cJSON_PrintUnformatted(cloud_event_obj);
        _CPY_TO_RESULT_DATA(self, new_event_json, strlen(new_event_json));
        cJSON_free(new_event_json);

        if (NULL == (event_text_cjson = cJSON_Parse(decode_text)))
        {
            LOGE(AIUI_TAG, "failed on parse cloud.event.text");
            break;
        }
        // 解析 event.text.key 放到 key_data 中.
        cJSON *item_type = NULL, *item_key = NULL;
        if ( NULL != (item_type = cJSON_GetObjectItem(event_text_cjson, "type")) &&
            NULL != (item_key = cJSON_GetObjectItem(event_text_cjson, "key")))
        {
            char key_value_buf[256] = {0};
            snprintf(key_value_buf, sizeof(key_value_buf), "%s=%s", item_type->valuestring, item_key->valuestring);
            _CPY_TO_RESULT_KEY_DATA(self, key_value_buf, strlen(key_value_buf));
        }
    } while (0);
    if (event_text_cjson)
    {
        cJSON_Delete(event_text_cjson);
    }
    _CLEAN_DECODE_VAR(text);

    self->cb.on_result(&(self->cur_result), self->cb_param);
    return 0;
}

// 解析对应sub的参数, 但不包括 ${sub}.text
static void _parse_sub_params(aiui_message_parse_t* self, const cJSON *sub_node)
{
    strncpy(self->cur_result.params.sub, sub_node->string, sizeof(self->cur_result.params.sub) - 1U);
    EXTRACT_JSON_INT_BY_KEY(self->cur_result.params.dts, sub_node, status);
    EXTRACT_JSON_INT_BY_KEY(self->cur_result.params.dseq, sub_node, seq);
    EXTRACT_JSON_STR_BY_KEY(self->cur_result.params.dte, sizeof(self->cur_result.params.dte),
                             sub_node, encoding);
    EXTRACT_JSON_STR_BY_KEY(self->cur_result.params.dtf, sizeof(self->cur_result.params.dtf),
                             sub_node, format);
    LOGI(AIUI_TAG, "result sub=%s status=%d seq=%d sid=%s",
         self->cur_result.params.sub,
         self->cur_result.params.dts,
         self->cur_result.params.dseq,
         self->cur_result.session.sid);
}

static int _aiui_parse_payload(aiui_message_parse_t* self,
                               const cJSON* parameter_obj,
                               const cJSON* payload_obj)
{
    if (NULL == payload_obj || NULL == self)
    {
        return 0;
    }
    int ret = 0;

#define _PARSE_PAYLOAD_NODE(NODE_NAME, target_function)                           \
    do {                                                                          \
        cJSON *node_obj = NULL;                                                   \
         if (NULL == (node_obj = cJSON_GetObjectItem(payload_obj, NODE_NAME)))    \
         { break; }                                                               \
        _CLEANUP_AIUI_CUR_RESULT_SUB(self);                                       \
        _parse_sub_params(self, node_obj);                                        \
        ret |= target_function(self, payload_obj, node_obj);                      \
    } while (0)

    _PARSE_PAYLOAD_NODE("event", _aiui_parse_cloud_event);
    _PARSE_PAYLOAD_NODE("iat", _aiui_parse_iat);
    do
    {
        cJSON *tidy_obj = cJSON_GetObjectItem(payload_obj, "cbm_tidy");
        if (NULL != tidy_obj)
        {
            _CLEANUP_AIUI_CUR_RESULT_SUB(self);
            _parse_sub_params(self, tidy_obj);
            ret |= _aiui_parse_tidy(self, payload_obj, parameter_obj, tidy_obj);
        }
    } while (0);
    // _PARSE_PAYLOAD_NODE("cbm_intent_split", _aiui_parse_intent_split);
    _PARSE_PAYLOAD_NODE("cbm_semantic", _aiui_parse_cbm_semantic);
    _PARSE_PAYLOAD_NODE("nlp", _aiui_parse_nlp);
    _PARSE_PAYLOAD_NODE("cbm_facial_expression", _aiui_parse_facial_expression);
    _PARSE_PAYLOAD_NODE("tts", _aiui_parse_tts);

    return ret;
}

static int _aiui_parse_header(aiui_message_parse_t* self, cJSON* header)
{
    if (NULL == header || NULL == self)
    {
        return 1;
    }
    cJSON* code = cJSON_GetObjectItem(header, "code");
    if (NULL == code)
    {
        return 1;
    }
    //cJSON* cid = cJSON_GetObjectItem(header, "cid");
    cJSON* sid = cJSON_GetObjectItem(header, "sid");
    cJSON* status = cJSON_GetObjectItem(header, "status");
    cJSON* stmid = cJSON_GetObjectItem(header, "stmid");

    if (NULL != sid && NULL != sid->valuestring)
    {
        strncpy(self->cur_result.session.sid, sid->valuestring,
            sizeof(self->cur_result.session.sid) - 1U);
        // LOGI(AIUI_TAG, "current sid=%s",  self->cur_result.session.sid);
    }
    if (NULL != status)
    {
        self->cur_result.session.status = status->valueint;
    }
    if (stmid != NULL && NULL != stmid->valuestring && '\0' != stmid->valuestring[0])
    {
        sscanf(stmid->valuestring, "%*[^0-9]%d", &(self->cur_result.session.stm_id));
    }
    if (0 != (self->cur_result.session.code = code->valueint))
    {
        LOGE(AIUI_TAG, "err(%d): sid=%s",
            self->cur_result.session.code, self->cur_result.session.sid);
        char *header_json_str = cJSON_PrintUnformatted(header);
        if (NULL != header_json_str)
        {
            /*
             * The error header contains the server's diagnostic message and
             * session id, but not the login key or secret. Keep it observable
             * so protocol/configuration errors can be diagnosed exactly.
             */
            LOGE(AIUI_TAG, "server error header: %s", header_json_str);
            _CPY_TO_RESULT_DATA(self, header_json_str, strlen(header_json_str));
            cJSON_free(header_json_str);
        }
    }
    return 0;
}

static int _aiui_parse_result(aiui_message_parse_t* self, cJSON* root)
{
    _RESET_AIUI_CUR_RESULT(self);

    cJSON* header = cJSON_GetObjectItem(root, "header");
    int ret = _aiui_parse_header(self, header);

    /*
     * Some rapid-interaction failures (for example code 500 / res empty)
     * contain only a header and no payload.  The original parser merely
     * logged those frames, so the application stayed in awaiting_reply with
     * capture paused forever.  Surface the error through the normal result
     * callback; aiui_api_esp will translate it to AIUI_EVENT_ERROR.
     */
    if (self->cur_result.session.code != 0)
    {
        self->cb.on_result(&(self->cur_result), self->cb_param);
        return ret;
    }

    cJSON* payload = cJSON_GetObjectItem(root, "payload");
    if (payload)
    {
        cJSON* parameter = cJSON_GetObjectItem(root, "parameter");
        ret |= _aiui_parse_payload(self, parameter, payload);
    }
    return ret;
}

int aiui_message_parse(AIUI_MESSAGE_PARSE_HANDLE handle, const char* msg, const int len)
{
    if (NULL == msg || '\0' == msg[0] || NULL == handle || len < 2)
    {
        return -1;
    }
    // LOGD(AIUI_TAG, "len=%d, msg=%s", len, msg);

    aiui_message_parse_t* self = (aiui_message_parse_t*)handle;
    cJSON* result = cJSON_Parse(msg);
    if (NULL == result || cJSON_IsInvalid(result))
    {
        return -1;
    }

    int ret = _aiui_parse_result(self, result);

    cJSON_Delete(result);
    return ret;
}

void aiui_message_parse_uninit(AIUI_MESSAGE_PARSE_HANDLE handle)
{
    if (NULL == handle)
    {
        return;
    }
    aiui_message_parse_t* self = (aiui_message_parse_t*)handle;
    if (NULL != self->session_slicing_rlt.iat_text)
    {
        aiui_hal_free(self->session_slicing_rlt.iat_text);
        self->session_slicing_rlt.iat_text = NULL;
    }
    if (NULL != self->session_slicing_rlt.nlp_text)
    {
        aiui_hal_free(self->session_slicing_rlt.nlp_text);
        self->session_slicing_rlt.nlp_text = NULL;
    }
    _RESET_AIUI_CUR_RESULT(self);

    aiui_hal_free(handle);
}
