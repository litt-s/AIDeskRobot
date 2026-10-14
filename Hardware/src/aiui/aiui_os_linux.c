#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#include "aiui_log.h"
#include "aiui_wrappers_os.h"

#define AIUI_TAG "aiui_os"

static int g_malloc_count = 0;
static int g_free_count = 0;
static pthread_mutex_t g_mutex_for_mem_counter = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_MEM_COUNTER() pthread_mutex_lock(&g_mutex_for_mem_counter)
#define UNLOCK_MEM_COUNTER() pthread_mutex_unlock(&g_mutex_for_mem_counter)

void *aiui_hal_malloc(int size)
{
    LOCK_MEM_COUNTER();
    ++g_malloc_count;
    UNLOCK_MEM_COUNTER();
    return malloc(size);
}

void aiui_hal_free(void *ptr)
{
    if (NULL == ptr)
    {
        return;
    }
    LOCK_MEM_COUNTER();
    ++g_free_count;
    UNLOCK_MEM_COUNTER();
    free(ptr);
}

void aiui_print_mem_info()
{
    LOCK_MEM_COUNTER();
    LOGD(AIUI_TAG, "malloc = %d, free = %d", g_malloc_count, g_free_count);
    UNLOCK_MEM_COUNTER();
}

char *aiui_append_string(char *src, const char *append)
{
    int append_size;
    if (NULL == append || (append_size = strlen(append) + 1) < 2)
    {
        return src;
    }
    if (NULL == src)
    {
        char *result = (char *)aiui_hal_malloc(append_size);
        memcpy(result, append, append_size);
        return result;
    }

    const int src_len = strlen(src);
    char *result = (char *)aiui_hal_malloc(src_len + append_size);
    memcpy(result, src, src_len);
    memcpy(result + src_len, append, append_size);
    aiui_hal_free(src);
    return result;
}

void aiui_hal_sleep(int time_ms)
{
    usleep(1000 * time_ms);
}

void aiui_hal_get_current_ms(uint64_t* p_cur_ms)
{
    struct timeval cur_time;
    gettimeofday(&cur_time, NULL);
    *p_cur_ms = ((uint64_t)cur_time.tv_sec * 1000U + (uint64_t)cur_time.tv_usec / 1000U);
}

int aiui_hal_thread_create(void **thread_handle, aiui_thread_param_t *param,
                           void *(*start_routine)(void *), void *arg)
{
    // if (param != NULL) LOGE(AIUI_TAG, "thread name = %s", param->name);
    /*pthread_attr_t attr;
    pthread_attr_init(&attr);
    int nnn = pthread_attr_setstacksize(&attr, param->stack_size * 8);
    LOGD(AIUI_TAG, "nnn = %d %d", nnn, EINVAL);
    size_t stack_size = 0;
    pthread_attr_getstacksize(&attr, &stack_size);
    LOGD(AIUI_TAG, "stack_size = %d", stack_size);*/
    int ret = pthread_create((pthread_t *)thread_handle, NULL, start_routine, arg);
    if (ret != 0)
    {
        LOGD(AIUI_TAG, "pthread_create error: %d\n", (int)ret);
        return -1;
    }
    if (param != NULL)
    {
        LOGD(AIUI_TAG, "thread name = %s", param->name);
    }
    if (param == NULL || param->deatch == 1)
    {
        pthread_detach((pthread_t)*thread_handle);
        // LOGD(AIUI_TAG, "attach thread");
    }
    return 0;
}

void aiui_hal_thread_destroy(void *thread_handle)
{
    if (thread_handle != NULL)
    {
        pthread_join((pthread_t)thread_handle, NULL);
    }
}

void *aiui_hal_sem_create(void)
{
    sem_t *sem = (sem_t *)aiui_hal_malloc(sizeof(sem_t));
    if (NULL == sem)
    {
        return NULL;
    }

    if (0 != sem_init(sem, 0, 0))
    {
        aiui_hal_free(sem);
        return NULL;
    }
    return sem;
}

void aiui_hal_sem_destroy(void *sem)
{
    if (NULL == sem)
    {
        return;
    }
    sem_destroy((sem_t *)sem);
    aiui_hal_free(sem);
}

void aiui_hal_sem_post(void *sem)
{
    if (NULL == sem)
    {
        return;
    }
    sem_post((sem_t *)sem);
}

int aiui_hal_sem_wait(void *sem, int timeout_ms)
{
    if (NULL == sem)
    {
        LOGE(AIUI_TAG, "sem not init");
        return -1;
    }
    if (timeout_ms <= 0)
    {
        sem_wait((sem_t *)sem);
        return 0;
    }

    int s;
    struct timespec ts =
    {
        0,
    };
    do
    {
        if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        {
            return -1;
        }

        s = 0;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000)
        {
            ts.tv_nsec -= 1000000000;
            s = 1;
        }

        ts.tv_sec += timeout_ms / 1000 + s;
    } while (((s = sem_timedwait((sem_t *)sem, &ts)) != 0) && errno == EINTR);

    return (s == 0) ? 0 : -1;
}

void aiui_hal_sem_clear(void *sem)
{
    if (NULL == sem)
    {
        return;
    }
    int value = 0;
    sem_getvalue((sem_t *)sem, &value);
    // LOGE(AIUI_TAG, "value = %d", value);
    while (value-- != 0)
    {
        aiui_hal_sem_wait(sem, -1);  // 清空信号量
    }
}

void *aiui_hal_mutex_create(void)
{
    int err_num;
    pthread_mutex_t *mutex = (pthread_mutex_t *)aiui_hal_malloc(sizeof(pthread_mutex_t));
    if (NULL == mutex)
    {
        return NULL;
    }

    if (0 != (err_num = pthread_mutex_init(mutex, NULL)))
    {
        LOGE(AIUI_TAG, "create mutex failed(%d)", err_num);
        aiui_hal_free(mutex);
        return NULL;
    }

    return mutex;
}

void aiui_hal_mutex_destroy(void *mutex)
{
    if (NULL == mutex)
    {
        return;
    }
    pthread_mutex_destroy((pthread_mutex_t *)mutex);

    aiui_hal_free(mutex);
}

int aiui_hal_mutex_lock(void *mutex)
{
    if (NULL == mutex)
    {
        return 0;
    }
    return pthread_mutex_lock((pthread_mutex_t *)mutex);
}

int aiui_hal_mutex_unlock(void *mutex)
{
    if (NULL == mutex)
    {
        return 0;
    }
    return pthread_mutex_unlock((pthread_mutex_t *)mutex);
}
