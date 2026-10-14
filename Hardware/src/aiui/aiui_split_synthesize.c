#include <string.h>
#include <stdbool.h>
#include "aiui_log.h"
#include "aiui_wrappers_os.h"
#include "aiui_split_synthesize.h"

#define AIUI_TAG "aiui_split"
#define AIUI_TTS_MIN_LENGTH 50 // tts 最小合成分割长度
typedef struct aiui_split_info_s
{
    bool completed; // 该意图的nlp是否合成结束
    bool finished;  // nlp结果是否下发结束
    char* data;
} aiui_split_info_t;

typedef struct aiui_split_synthesize_s
{
    int number;
    void* tid;
    bool stop;
    void* lock;
    void* sem;
    aiui_split_synthesize cb;
    void* user;
    aiui_split_info_t* split_info;

} aiui_split_synthesize_t;

// 通过字符,.? 和中文，。？来分割
static const char* s_punctuation_marks[] =
{
    ",",
    "，",
    ".",
    "。",
    "?",
    "？",
    "!",
    "！"
};

static char* _aiui_search_segmentation(char* strings)
{
    // 找出上述标点中最靠后的
    char* pos_max = NULL;
    char* src = strings;
    for (size_t i = 0; i < sizeof(s_punctuation_marks) / sizeof(s_punctuation_marks[0]); i++)
    {
        while (true)
        {
            char* pos = strstr(src, s_punctuation_marks[i]);
            // printf("pos = %p, %s\n", pos, pos);
            if (NULL == pos)
            {
                break;;
            }
            pos += strlen(s_punctuation_marks[i]);
            if (pos > pos_max)
            {
                pos_max = pos;
            }
            src = pos;
        }
        
    }
    return pos_max;
}


static char* _aiui_extract_string(char* strings)
{
    char* src = strings;
    int len = strlen(strings);
    char* pos = _aiui_search_segmentation(src);
    if (NULL == pos)
    {
        return NULL;
    }
    int offset = pos - strings;
    //printf("pos = %s, strings = %s\n", pos, src);
    //printf("offset = %d\n", offset);
    if (offset >= AIUI_TTS_MIN_LENGTH)
    {
        char* tts = (char*)aiui_hal_malloc(offset + 1);
        memcpy(tts, strings, offset);
        tts[offset] = '\0';
        memcpy(strings, strings + offset, offset);
        strings[len - offset] = '\0';
        return tts;
    }
    return NULL;
}

static void* _aiui_split_synthesize_task(void* param)
{
    aiui_split_synthesize_t* self = (aiui_split_synthesize_t*)param;
    while (!self->stop)
    {
        char* tts = NULL;
        int i = 0;
        // 主动合成策略：intent 0合成完，才去合成intent 1，然后intent 2...
        aiui_hal_mutex_lock(self->lock);
        for (i = 0; i < self->number; i++)
        {
            if (self->split_info[i].completed)
                continue;
            if (NULL == self->split_info[i].data || strlen(self->split_info[i].data) == 0)
            {
                if (self->split_info[i].finished) //  下发完成标识为true且没有文本信息时,主动合成下个intent文本
                {
                    self->split_info[i].completed = true;
                    self->cb(self->user, NULL, i, self->split_info[i].completed);
                    continue;
                }
                else
                    break;
            }
            // 如果nlp已经下发完整,不用分割，直接全部合成
            if (self->split_info[i].finished)
            {
                tts =  self->split_info[i].data;
                self->split_info[i].data = NULL;
                self->split_info[i].completed = true;
            }
            else
            {
                tts = _aiui_extract_string(self->split_info[i].data);
            }
            //LOGD(AIUI_TAG, "tts = %s, intent = %d", tts, i);
            if (NULL == tts)
                break;
            // 调用主动合成接口，合成tts
            // 下方完整调用合成之后，释放内存,修改合成结束标志位
            self->cb(self->user, tts, i, self->split_info[i].completed);
            aiui_hal_free(tts);
            // 上一个意图回答合成完毕后，紧接着合成下一个意图回答，否则跳出合成，等待意图回答文本
            if (self->split_info[i].finished)
            {
                continue;
            }
            break;
        }
        aiui_hal_mutex_unlock(self->lock);
        aiui_hal_sem_wait(self->sem, -1);
    }
    return NULL;
}

AIUI_SPLIT_HANDLE aiui_split_synthesize_init(aiui_split_synthesize cb, void* user)
{
    aiui_split_synthesize_t* handle = (aiui_split_synthesize_t*)aiui_hal_malloc(sizeof(aiui_split_synthesize_t));

    handle->split_info = NULL;
    handle->number = 0;
    handle->lock = aiui_hal_mutex_create();
    handle->sem  = aiui_hal_sem_create();
    handle->stop= false;
    handle->cb = cb;
    handle->user = user;
    handle->tid = NULL;
    aiui_thread_param_t thread_param = {0};
    thread_param.name = "aiui_split_synthesize";
    thread_param.deatch = 0;
    thread_param.priority = 5;
    thread_param.stack_size = 2048;
    aiui_hal_thread_create(&handle->tid, &thread_param, _aiui_split_synthesize_task, handle);
    return (AIUI_SPLIT_HANDLE)handle;
}

void aiui_set_intent_num(AIUI_SPLIT_HANDLE handle, int num)
{
    if (NULL == handle)
    {
        return;
    }
    aiui_split_synthesize_t* self = (aiui_split_synthesize_t*)handle;
    aiui_hal_mutex_lock(self->lock);
    for (int i = 0; i < self->number; i++)
    {
        if (self->split_info[i].data != NULL)
        {
            aiui_hal_free(self->split_info[i].data);
        }
    }
    aiui_hal_free(self->split_info);

    self->number = num;
    self->split_info = (aiui_split_info_t*)aiui_hal_malloc(sizeof(aiui_split_info_t) * num);

    for (int i = 0; i < num; i++)
    {
        self->split_info[i].finished = false;
        self->split_info[i].completed = false;
        self->split_info[i].data = NULL;
    }
    aiui_hal_mutex_unlock(self->lock);
}

int aiui_append_answer(AIUI_SPLIT_HANDLE handle, int index, const char* data)
{
    if (NULL == handle)
        return -1;
    aiui_split_synthesize_t* self = (aiui_split_synthesize_t*)handle;
    if (index >= self->number)
    {
        return -1;
    }
    aiui_hal_mutex_lock(self->lock);
    // 结束单独设计一个标记(空字符或空指针)的目的:担忧大模型下发时有时也是一个空字符
    if (data == NULL || strlen(data) <= 0)
    {
        self->split_info[index].finished = true;
    }
    else
    {
        self->split_info[index].data = aiui_append_string(self->split_info[index].data, data);
    }
    aiui_hal_mutex_unlock(self->lock);
    aiui_hal_sem_post(self->sem);
    return 0;
}

void aiui_split_synthesize_uninit(AIUI_SPLIT_HANDLE handle)
{
    if (NULL == handle)
    {
        return;
    }
    aiui_split_synthesize_t* self = (aiui_split_synthesize_t*)handle;
    self->stop = true;
    aiui_hal_sem_post(self->sem);
    aiui_hal_thread_destroy(self->tid);
    for (int i = 0; i < self->number; i++)
    {
        if (self->split_info[i].data != NULL)
        {
            aiui_hal_free(self->split_info[i].data);
        }
    }
    aiui_hal_free(self->split_info);
    aiui_hal_mutex_destroy(self->lock);
    aiui_hal_sem_destroy(self->sem);
    aiui_hal_free(handle);
}
