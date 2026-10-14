/**
* @file  aiui_api.h  
* @brief sdk对外接口
* @version   v1.0.0
*/

#ifndef __AIUI_API_H__
#define __AIUI_API_H__

#include <stddef.h>
#include "aiui_param.h"

typedef enum
{
    AIUI_EVENT_RESERVED = 0, // 保留
    AIUI_EVENT_CONNECTED_TO_SERVER, // 连接到服务器
    AIUI_EVENT_SERVER_DISCONNECTED, // 与服务器断开连接
    AIUI_EVENT_CONNECTED_TIMEOUT,   // 连接超时
    AIUI_EVENT_ERROR = 6, // SDK 错误

    AIUI_EVENT_RESULT = 10, // 结果事件, 例如: iat/tts/nlp
} AIUI_EVENT_TYPE;

/**
 * 常规回调. 非 AIUI_EVENT_RESULT 事件回调数据.
 */
typedef struct aiui_general_data_s
{
    int code;
    int arg1;
    int arg2;
    size_t message_size;
    const char *message;
} aiui_general_data_t;

/**
 * aiui 泛型回调.
 * 具体联合体中放的是哪种数据请根据 type 字段来做判断:
 *   1. 若 AIUI_EVENT_RESULT == type, 则联合体有效数据是 aiui_event_result_t result;
 *   2. 当前若非 1 上述的情况, 则有效数据是 aiui_general_data_t general;
 */
typedef struct aiui_callback_event_s
{
    AIUI_EVENT_TYPE type;
    // 联合体. 具体应该取的数据请根据 type 来做判断.
    // 后期可根据type内容扩展.
    union
    {
        aiui_general_data_t general;
        aiui_event_result_t result;
    };
} aiui_callback_event_t;

typedef void (*aiui_event)(const void *user_param, aiui_callback_event_t *event);

typedef void *AIUI_HANDLE;

#ifdef __cplusplus
extern "C" {
#endif

/**
* 创建 AIUI_HANDLE 句柄
*
* @param[in] init_params 初始化参数
* @param[in] event_cb 事件回调函数.
* @param[in] user_params 回调函数私有参数
* @return 成功返回AIUI_API句柄，失败返回NULL
*/
AIUI_HANDLE aiui_init(const char *init_params, aiui_event event_cb, void *user_params);

/**
* 销毁 AIUI_HANDLE 句柄
*
* @param[in] handle aiui_init 返回的句柄
*/
void aiui_uninit(AIUI_HANDLE handle);

/**
* 开启一轮新的会话
*
* @param[in] handle aiui_init返回句柄
* @return 0:成功, -1: 失败
 */
int aiui_start(AIUI_HANDLE handle);

/**
* 停止会话, 断开连接
*
* @param[in] handle aiui_init返回句柄
* @return 0: 成功, -1: 失败
 */
int aiui_stop(AIUI_HANDLE handle);

/**
* 向云端发送文本数据
*
* @param[in] handle aiui_init返回句柄
* @param[in] buf 文本数据内容,文本数据为utf-8类型
* @param[in] len 发送数据长度
* @return : = 0发送的数据长度, -1: 发送失败
*/
int aiui_send_text(AIUI_HANDLE handle, const char *buf, const int len);

/**
* 向云端发送音频数据
*
* @param[in] handle aiui_init返回句柄
* @param[in] buf 音频数据内容,音频数据为 pcm 格式
* @param[in] len 发送数据长度. 推荐 40ms 1c16k16b 音频, 也就是 1280 字节
* @return : = 0发送的数据长度, -1: 发送失败
*/
int aiui_send_audio(AIUI_HANDLE handle, const char *buf, const int len);

/**
* 主动合成.
*
* @param[in] handle aiui_init返回句柄
* @param[in] buf 文本数据
* @param[in] len 发送数据长度
* @return : = 0发送的数据长度, -1: 发送失败
*/
int aiui_send_tts(AIUI_HANDLE handle, const char *buf, const int len);

/**
* 获取版本信息
*/
const char *aiui_get_version();

#ifdef __cplusplus
}
#endif

#endif    // __AIUI_H__
