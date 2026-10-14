#ifndef __AIUI_HTTP_API_H__
#define __AIUI_HTTP_API_H__

typedef void *AIUI_HTTP_HANDLE;

typedef enum {
    AIUI_HTTP_EVENT_DISCONNECTED = 0,
    AIUI_HTTP_EVENT_CONNECTED,
    AIUI_HTTP_EVENT_MESSAGE,

    AIUI_HTTP_EVENT_MAX
} AIUI_HTTP_EVENT_TYPE;

typedef int (*aiui_http_recv_cb)(const void *user_param,
                                 AIUI_HTTP_EVENT_TYPE type,
                                 void *buf,
                                 const int len);
/**
* 创建http请求句柄:
* @param[in] url 请求的url
* @param[in] type 请求的类型(GET/POST)
* @param[in] cb   数据接收回调函数，与aiui_http_recv函数互斥，即设置了回调函数就不能再用aiui_http_recv接收数据了.
* @param[in] param 回调函数的参数
* @param[in] name 接收线程的名称,不设置回调函数传空即可(http创建接收线程时，需要线程名称)
* @return: 返回http句柄，若为NULL，则创建失败
*/
AIUI_HTTP_HANDLE aiui_http_create_request(
    const char *url, const char *type, const aiui_http_recv_cb cb, void *param, const char *name);

/**
* 添加http头文件信息
* @param[in] handle aiui_http_create_request返回值
* @param[in] data http请求头信息内容,必须带'\0'
* @return 0:成功, -1: 失败
*/
int aiui_http_add_header(AIUI_HTTP_HANDLE handle, const char *data);

/*
* http发送请求:
* @param[in] handle aiui_http_create_request返回值
* @param[in] data http请求body信息内容,必须带'\0'
* @return 0:成功, -1: 失败
*/
int aiui_http_send_request(AIUI_HTTP_HANDLE handle, const char *body);

/**
* http发送数据
* 区别于aiui_http_send_request：
* aiui_http_send_request是建立一个http请求,
* aiui_http_send_msg是建立http请求后，要向服务端发送数据时调用,
* 比如基于http建立websocket连接后，websocket需要向客户端发送音频等数据时,
* 需要调用此函数. 若http连接建立后，不需要向服务端发送数据，不调用此函数
* @param[in] handle aiui_http_create_request返回值
* @param[in] msg 要发送的数据
* @param[in] len 数据的长度
* @return: 发送的实际长度
*/
int aiui_http_send_msg(AIUI_HTTP_HANDLE handle, const char *msg, const int len);

/**
* 接收对端发送的数据: (目的是方便客户端读取数据量较小的http连接,比如音乐授权、获取音乐url等)
* aiui_http_create_request 函数中的回调函数参数不为空时，此函数失效.
* @param[in] handle aiui_http_create_request返回值
* @param[in] buf 存储接收数据的指针
* @param[in] len 要接收的数据长度
* @return: 返回实际的数据长度
*/
int aiui_http_recv(AIUI_HTTP_HANDLE handle, char *buf, const int len);

/**
* 销毁http请求
* @param[in] handle: aiui_http_create_request返回值
*/
void aiui_http_destroy_request(AIUI_HTTP_HANDLE handle);
#endif    // __AIUI_HTTP_API_H__
