#ifndef __AIUI_SPLIT_SYNTHESIZE_H__
#define __AIUI_SPLIT_SYNTHESIZE_H__

typedef void* AIUI_SPLIT_HANDLE;

typedef void (*aiui_split_synthesize)(const void *user, const char *text, int intent, bool completed);

AIUI_SPLIT_HANDLE aiui_split_synthesize_init(aiui_split_synthesize cb, void* user);

void aiui_set_intent_num(AIUI_SPLIT_HANDLE handle, int num);

int aiui_append_answer(AIUI_SPLIT_HANDLE handle, int index, const char* data);

void aiui_split_synthesize_uninit(AIUI_SPLIT_HANDLE handle);

#endif // __AIUI_SPLIT_SYNTHESIZE_H__