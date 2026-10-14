#ifndef __AIUI_SOCKET_API_H__
#define __AIUI_SOCKET_API_H__

typedef void *AIUI_SOCKET_HANDLE;

typedef enum { AIUI_SOCKET_RAW = 0, AIUI_SOCKET_SSL } AIUI_SOCKET_TYPE;

AIUI_SOCKET_HANDLE aiui_socket_connect(const char *address,
                                       const char *port,
                                       AIUI_SOCKET_TYPE type);

void aiui_socket_close(AIUI_SOCKET_HANDLE handle);

void aiui_socket_destroy(AIUI_SOCKET_HANDLE handle);

int aiui_socket_send(AIUI_SOCKET_HANDLE handle, const char *buf, const size_t len);

int aiui_socket_recv(AIUI_SOCKET_HANDLE handle, char *buf, const size_t len);

int aiui_connect_is_ok(AIUI_SOCKET_HANDLE handle);

#endif    // __AIUI_SOCKET_API_H__