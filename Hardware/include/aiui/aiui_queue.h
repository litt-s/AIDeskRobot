#ifndef __AIUI_QUEUE_H__
#define __AIUI_QUEUE_H__

typedef void *AIUI_QUEUE_HANDLE;
/*
 * @brief
 * 该queue目的是存放任意类型的数据，使用的void*来存放用户的数据.
 * 所以该void*不支持临时地址,要么全局地址，要么malloc的地址.
 * 该地址的内存释放时机由用户控制
 */
AIUI_QUEUE_HANDLE aiui_create_queue();

void aiui_destroy_queue(AIUI_QUEUE_HANDLE queue);

int aiui_queue_push(AIUI_QUEUE_HANDLE queue, void *data);

void aiui_queue_pop(AIUI_QUEUE_HANDLE queue);

void *aiui_queue_front(AIUI_QUEUE_HANDLE queue);

int aiui_queue_is_empty(AIUI_QUEUE_HANDLE queue);

int aiui_queue_size(AIUI_QUEUE_HANDLE queue);

#endif // __AIUI_QUEUE_H__
