#ifndef __AIUI_LOG_H__
#define __AIUI_LOG_H__

#include <time.h>
#include <sys/time.h>
#include <stdio.h>

typedef enum
{
    LOG_VERBOSE_LEVEL = 1,
    LOG_DEBUG_LEVEL,
    LOG_INFO_LEVEL,
    LOG_WARNING_LEVEL,
    LOG_ERROR_LEVEL,
} log_level_enum;

#ifndef LOG_DEFAULT_LEVEL
// #    define LOG_DEFAULT_LEVEL LOG_DEBUG_LEVEL    //LOG_ERROR_LEVEL
#    define LOG_DEFAULT_LEVEL LOG_VERBOSE_LEVEL
#endif

//#define CLOG_COLOR
#ifdef CLOG_COLOR
#    define CORC "\033[0m"
#    define CORE "\033[1;31m"    // red(high light)
#    define CORW "\033[1;33m"    // purple(high light)
#    define CORI "\033[1;33m"    // yellow
#    define CORD "\033[1;35m"    // dark green
#    define CORV "\033[1;34m"    // blue
#    define CORN "\033[1;33m"    // green
#    define CORT "\033[1;32m"    // green
#else
#    define CORC
#    define CORE
#    define CORW
#    define CORI
#    define CORD
#    define CORV
#    define CORN
#    define CORT
#endif    // CLOG_COLOR

extern int g_aiui_log_level;

#define TLOGD(TAG, format, ...)                                                      \
    do {                                                                             \
        time_t timep;                                                                \
        struct tm *p;                                                                \
        time(&timep);                                                                \
        p = gmtime(&timep);                                                          \
            struct timeval tv_now;                                                   \
            gettimeofday(&tv_now, NULL);                                             \
        printf(CORT                                                                  \
               "[D]"                                                                 \
               "[%u-%02u-%02u %02u:%02u:%02u.%03lu]  [%s ##%d]" format "" CORC "\n", \
               (p->tm_year + 1900),                                                  \
               p->tm_mon + 1,                                                        \
               p->tm_mday,                                                           \
               p->tm_hour + 8,                                                       \
               p->tm_min,                                                            \
               p->tm_sec,                                                            \
               (tv_now.tv_usec) / 1000,                                              \
               TAG,                                                                  \
               __LINE__,                                                             \
               ##__VA_ARGS__);                                                       \
    } while (0)

#define LOGD(TAG, format, ...)                                                           \
    do {                                                                                 \
        if (g_aiui_log_level <= LOG_DEBUG_LEVEL) {                                       \
            time_t timep;                                                                \
            struct tm *p;                                                                \
            time(&timep);                                                                \
            p = gmtime(&timep);                                                          \
            struct timeval tv_now;                                                       \
            gettimeofday(&tv_now, NULL);                                                 \
            printf(CORD                                                                  \
                   "[D]"                                                                 \
                   "[%u-%02u-%02u %02u:%02u:%02u.%03lu]  [%s ##%d]" format "" CORC "\n", \
                   (p->tm_year + 1900),                                                  \
                   p->tm_mon + 1,                                                        \
                   p->tm_mday,                                                           \
                   p->tm_hour + 8,                                                       \
                   p->tm_min,                                                            \
                   p->tm_sec,                                                            \
                   (tv_now.tv_usec) / 1000,                                              \
                   TAG,                                                                  \
                   __LINE__,                                                             \
                   ##__VA_ARGS__);                                                       \
        }                                                                                \
    } while (0)

#define LOGE(TAG, format, ...)                                                                    \
    do {                                                                                          \
        if (g_aiui_log_level <= LOG_ERROR_LEVEL) {                                                \
            time_t timep;                                                                         \
            struct tm *p;                                                                         \
            time(&timep);                                                                         \
            p = gmtime(&timep);                                                                   \
            struct timeval tv_now;                                                                \
            gettimeofday(&tv_now, NULL);                                                          \
            printf(CORE "[%u-%02u-%02u %02u:%02u:%02u.%03lu] [E][%s ##%d]" format "" CORC "\n",   \
                   (p->tm_year + 1900),                                                           \
                   p->tm_mon + 1,                                                                 \
                   p->tm_mday,                                                                    \
                   p->tm_hour + 8,                                                                \
                   p->tm_min,                                                                     \
                   p->tm_sec,                                                                     \
                   (tv_now.tv_usec) / 1000,                                                       \
                   TAG,                                                                           \
                   __LINE__,                                                                      \
                   ##__VA_ARGS__);                                                                \
        }                                                                                         \
    } while (0)

#define LOGV(TAG, format, ...)                                                                   \
    do {                                                                                         \
        if (g_aiui_log_level <= LOG_VERBOSE_LEVEL) {                                             \
            printf(CORV "[V][%s ##%d]" format "" CORC "\n", TAG, __LINE__, ##__VA_ARGS__);       \
        }                                                                                        \
    } while (0)

#define LOGW(TAG, format, ...)                                                                   \
    do {                                                                                         \
        if (g_aiui_log_level <= LOG_WARNING_LEVEL) {                                             \
            printf(CORW "[W][%s ##%d]" format "" CORC "\n", TAG, __LINE__, ##__VA_ARGS__);       \
        }                                                                                        \
    } while (0)

#define LOGI(TAG, format, ...)                                                                \
    do {                                                                                      \
        if (g_aiui_log_level <= LOG_INFO_LEVEL) {                                             \
            printf(CORI "[I][%s ##%d]" format "" CORC "\n", TAG, __LINE__, ##__VA_ARGS__);    \
        }                                                                                     \
    } while (0)

#endif    //__AIUI_LOG_H__
