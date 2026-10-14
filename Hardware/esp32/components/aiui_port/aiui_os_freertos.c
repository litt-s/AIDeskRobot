#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "aiui_log.h"
#include "aiui_wrappers_os.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AIUI_TAG "aiui_os"
#define AIUI_MIN_TASK_STACK_BYTES 6144

typedef struct {
    TaskHandle_t task;
    SemaphoreHandle_t finished;
    void *(*entry)(void *);
    void *arg;
} aiui_task_t;

static portMUX_TYPE s_mem_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_malloc_count;
static unsigned s_free_count;

static void aiui_task_entry(void *arg)
{
    aiui_task_t *task = (aiui_task_t *)arg;
    task->entry(task->arg);
    xSemaphoreGive(task->finished);
    vTaskDelete(NULL);
}

void *aiui_hal_malloc(int size)
{
    if (size <= 0) {
        return NULL;
    }
    /*
     * AIUI builds and destroys several JSON/base64 buffers for every 20 ms
     * audio frame. Keep that churn out of the internal DMA-capable heap used
     * by Wi-Fi/TLS.
     */
    void *ptr = heap_caps_malloc_prefer(
        (size_t)size, 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ptr) {
        portENTER_CRITICAL(&s_mem_lock);
        ++s_malloc_count;
        portEXIT_CRITICAL(&s_mem_lock);
    }
    return ptr;
}

void aiui_hal_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    portENTER_CRITICAL(&s_mem_lock);
    ++s_free_count;
    portEXIT_CRITICAL(&s_mem_lock);
    heap_caps_free(ptr);
}

void aiui_print_mem_info(void)
{
    portENTER_CRITICAL(&s_mem_lock);
    unsigned allocations = s_malloc_count;
    unsigned frees = s_free_count;
    portEXIT_CRITICAL(&s_mem_lock);
    ESP_LOGI(AIUI_TAG,
             "alloc=%u free=%u live=%u internal=%u largest=%u dma=%u "
             "psram=%u",
             allocations, frees, allocations - frees,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

char *aiui_append_string(char *src, const char *append)
{
    if (!append || append[0] == '\0') {
        return src;
    }
    size_t src_len = src ? strlen(src) : 0;
    size_t append_len = strlen(append);
    if (src_len > INT_MAX - append_len - 1) {
        return src;
    }
    char *result = aiui_hal_malloc((int)(src_len + append_len + 1));
    if (!result) {
        return src;
    }
    if (src_len) {
        memcpy(result, src, src_len);
    }
    memcpy(result + src_len, append, append_len + 1);
    aiui_hal_free(src);
    return result;
}

void aiui_hal_sleep(int time_ms)
{
    if (time_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)time_ms));
    } else {
        taskYIELD();
    }
}

void aiui_hal_get_current_ms(uint64_t *current_ms)
{
    if (current_ms) {
        *current_ms = (uint64_t)(esp_timer_get_time() / 1000);
    }
}

int aiui_hal_thread_create(void **thread_handle,
                           aiui_thread_param_t *param,
                           void *(*start_routine)(void *),
                           void *arg)
{
    if (!thread_handle || !start_routine) {
        return -1;
    }
    aiui_task_t *task = calloc(1, sizeof(*task));
    if (!task) {
        return -1;
    }
    task->finished = xSemaphoreCreateBinary();
    task->entry = start_routine;
    task->arg = arg;
    if (!task->finished) {
        free(task);
        return -1;
    }
    uint32_t stack_bytes = AIUI_MIN_TASK_STACK_BYTES;
    UBaseType_t priority = 5;
    const char *name = "aiui";
    if (param) {
        if (param->stack_size > 0) {
            uint32_t requested = (uint32_t)param->stack_size * 2U;
            if (requested > stack_bytes) {
                stack_bytes = requested;
            }
        }
        if (param->priority > 0 && param->priority < configMAX_PRIORITIES) {
            priority = (UBaseType_t)param->priority;
        } else if (param->priority >= configMAX_PRIORITIES) {
            ESP_LOGW(AIUI_TAG,
                     "invalid task priority %d (max %d), using default %u",
                     param->priority, configMAX_PRIORITIES - 1,
                     (unsigned)priority);
        }
        if (param->name) {
            name = param->name;
        }
    }
    BaseType_t ok = xTaskCreate(aiui_task_entry, name, stack_bytes, task,
                                priority, &task->task);
    if (ok != pdPASS) {
        vSemaphoreDelete(task->finished);
        free(task);
        return -1;
    }
    *thread_handle = task;
    return 0;
}

void aiui_hal_thread_destroy(void *thread_handle)
{
    aiui_task_t *task = (aiui_task_t *)thread_handle;
    if (!task) {
        return;
    }
    xSemaphoreTake(task->finished, portMAX_DELAY);
    vSemaphoreDelete(task->finished);
    free(task);
}

void *aiui_hal_sem_create(void)
{
    return xSemaphoreCreateCounting(UINT16_MAX, 0);
}

void aiui_hal_sem_destroy(void *sem)
{
    if (sem) {
        vSemaphoreDelete((SemaphoreHandle_t)sem);
    }
}

void aiui_hal_sem_post(void *sem)
{
    if (sem) {
        xSemaphoreGive((SemaphoreHandle_t)sem);
    }
}

int aiui_hal_sem_wait(void *sem, int timeout_ms)
{
    if (!sem) {
        return -1;
    }
    TickType_t ticks = timeout_ms <= 0 ? portMAX_DELAY : pdMS_TO_TICKS((uint32_t)timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)sem, ticks) == pdTRUE ? 0 : -1;
}

void aiui_hal_sem_clear(void *sem)
{
    if (!sem) {
        return;
    }
    while (xSemaphoreTake((SemaphoreHandle_t)sem, 0) == pdTRUE) {
    }
}

void *aiui_hal_mutex_create(void)
{
    return xSemaphoreCreateMutex();
}

void aiui_hal_mutex_destroy(void *mutex)
{
    if (mutex) {
        vSemaphoreDelete((SemaphoreHandle_t)mutex);
    }
}

int aiui_hal_mutex_lock(void *mutex)
{
    if (!mutex) {
        return 0;
    }
    return xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY) == pdTRUE ? 0 : -1;
}

int aiui_hal_mutex_unlock(void *mutex)
{
    if (!mutex) {
        return 0;
    }
    return xSemaphoreGive((SemaphoreHandle_t)mutex) == pdTRUE ? 0 : -1;
}
