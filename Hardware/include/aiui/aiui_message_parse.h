#ifndef __AIUI_MESSAGE_PARSE_H__
#define __AIUI_MESSAGE_PARSE_H__

#include "aiui_param.h"

typedef enum
{
    AIUI_MESSAGE_HEADER = 0,
    AIUI_MESSAGE_INTENTE_SPLIT,
    AIUI_MESSAGE_ERROR,

    AIUI_MESSAGE_MAX
} AIUI_MESSAGE_TYPE;

typedef struct
{
    void (*on_result)(aiui_event_result_t *result, void* user_param);
    // void (*header)(const char *cid, void* param);
    // void (*error)(int code, const char* info, void* param);
    // void (*iat)(const char *iat, int status, void* param);
    // void (*intent_split_num)(int num, void* param);
    // void (*intent_split)(int index, const char* value, void* param);
    // void (*cbm_semantic)(int index, const char* value, void* param);
    // void (*nlp)(int index, const char* value, int status, void* param);
    // void (*tts)(const char* value, void* param);
} aiui_msg_parse_cb;

typedef void *AIUI_MESSAGE_PARSE_HANDLE;
typedef void (*aiui_message_parse_cb)(void *user_param, AIUI_MESSAGE_TYPE type, const char *msg);

AIUI_MESSAGE_PARSE_HANDLE aiui_message_parse_init(aiui_msg_parse_cb cb, void *user_param);

int aiui_message_parse(AIUI_MESSAGE_PARSE_HANDLE handle, const char *msg, const int len);

void aiui_message_parse_uninit(AIUI_MESSAGE_PARSE_HANDLE handle);

#endif    // __AIUI_MESSAGE_PARSE_H__
