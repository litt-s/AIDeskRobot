#include "aiui_wrappers_os.h"
#include "aiui_log.h"
#include "aiui_queue.h"

#define AIUI_LOG "aiui_queue"
typedef struct aiui_queue_element_s {
	void* data;
	struct aiui_queue_element_s* next;
} aiui_queue_element_t;

typedef struct aiui_queue_s
{
    int size;
    aiui_queue_element_t* head;
    aiui_queue_element_t* tail;
} aiui_queue_t;

AIUI_QUEUE_HANDLE aiui_create_queue()
{
    aiui_queue_t* queue = (aiui_queue_t*)aiui_hal_malloc(sizeof(aiui_queue_t));
    queue->size = 0;
    queue->head = NULL;
    queue->tail = NULL;
    
    return queue;
}

void aiui_destroy_queue(AIUI_QUEUE_HANDLE queue)
{   
    if (NULL == queue)
    {
        return;
    }
    aiui_hal_free(queue);
}

int aiui_queue_push(AIUI_QUEUE_HANDLE queue, void* data)
{
    if (NULL == queue)
    {
        return -1;
    }
    aiui_queue_t* self = (aiui_queue_t*)queue;
    aiui_queue_element_t* element = (aiui_queue_element_t*)aiui_hal_malloc(sizeof(aiui_queue_element_t));
    if (NULL == element)
    {
        LOGE(AIUI_LOG, "failed on allocate queue element");
        return -1;
    }
    element->data =data;
    element->next = NULL;
    if (self->head == NULL)
    {
        self->head = element;
        self->tail = element;
    }
    else
    {
        self->tail->next = element;
        self->tail = element;
    }
    self->size++;
    return 0;
}

void aiui_queue_pop(AIUI_QUEUE_HANDLE queue)
{
    if (NULL == queue)
    {
        return;
    }
    aiui_queue_t* self = (aiui_queue_t*)queue;
    if (self->head == NULL)
    {
        LOGD(AIUI_LOG, "queue is empty");
        return;
    }
    aiui_queue_element_t* element = self->head;
    self->head = self->head->next;
    if (self->head == NULL)
    {
        self->tail = NULL;
    }
    aiui_hal_free(element);
    --self->size;
}

void* aiui_queue_front(AIUI_QUEUE_HANDLE queue)
{
    if (NULL == queue)
    {
        return NULL;
    }
    aiui_queue_t* self = (aiui_queue_t*)queue;
    if (self->head == NULL)
    {
        return NULL;
    }

    return self->head->data;
}

int aiui_queue_is_empty(AIUI_QUEUE_HANDLE queue)
{
    if (NULL == queue)
    {
        return -1;
    }
    aiui_queue_t* self = (aiui_queue_t*)queue;
    if (self->head == NULL)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

int aiui_queue_size(AIUI_QUEUE_HANDLE queue)
{
    if (NULL == queue)
    {
        return -1;
    }
    return ((aiui_queue_t *)queue)->size;
}
