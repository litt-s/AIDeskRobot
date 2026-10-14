#ifndef __AIUI_WEBSOCKET_API_H__
#define __AIUI_WEBSOCKET_API_H__

typedef enum
{
    AIUI_WS_CONNECT_STATUS_DISCONNECTED = 0,
    AIUI_WS_CONNECT_STATUS_CONNECTED = 1,
} aiui_websocket_connect_status_e;

typedef struct aiui_websocket_cb_s
{
    void (*on_connect_status_changed)(void *user_param, aiui_websocket_connect_status_e status);
    void (*on_message)(void *user_param, const char *msg, int len);
} aiui_websocket_cb_t;

typedef void *AIUI_WEBSOCKET_HANDLE;

/**
* 创建websocket句柄:
* @param[in] cb 数据接收回调函数
* @param[in] param 回调函数的私有参数.
* @return: 返回wensocket句柄,若为NULL，则创建失败
*/
AIUI_WEBSOCKET_HANDLE aiui_websocket_create(aiui_websocket_cb_t cb, void *param);

/**
* websocket连接:
* @param[in] handle aiui_websocket_create返回值
* @param[in] url   websocket url
* @param[in] origin_host 主机域名
* @return 0:成功, -1: 失败
*/
int aiui_websocket_connect(AIUI_WEBSOCKET_HANDLE handle, const char *url, const char *origin_host);

/**
* websocket断开:
* @param[in] handle aiui_websocket_create返回值
* @return 0:成功, -1: 失败
*/
int aiui_websocket_disconnect(AIUI_WEBSOCKET_HANDLE handle);

/**
* 发送二进制数据:
* @param[in] handle aiui_websocket_create返回值
* @param[in] msg: 数据内容
* @param[in] len: 数据长度
* @return: 返回发送的数据长度
*/
int aiui_websocket_send_bin(AIUI_WEBSOCKET_HANDLE handle, const char *msg, int len);

/**
* 发送文本数据:
* @param[in] handle aiui_websocket_create返回值
* @param[in] msg: 数据内容
* @param[in] len: 数据长度
* @return: 返回发送的数据长度
*/
int aiui_websocket_send_text(AIUI_WEBSOCKET_HANDLE handle, const char *msg, int len);

/**
* 销毁websocket句柄:
* @param[in] handle aiui_websocket_create返回值
*/
void aiui_websocket_destroy(AIUI_WEBSOCKET_HANDLE handle);

#endif    // __AIUI_WEBSOCKET_API_H__
