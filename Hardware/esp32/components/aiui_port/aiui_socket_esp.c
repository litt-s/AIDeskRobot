#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aiui_socket_api.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"

#define AIUI_SOCKET_TIMEOUT_MS 5000

typedef struct {
    int fd;
    bool tls_enabled;
    esp_tls_t *tls;
} aiui_esp_socket_t;

static const char *TAG = "aiui_socket";

static bool wait_fd(int fd, bool write_ready)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval timeout = {
        .tv_sec = AIUI_SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (AIUI_SOCKET_TIMEOUT_MS % 1000) * 1000,
    };
    int result = select(fd + 1,
                        write_ready ? NULL : &set,
                        write_ready ? &set : NULL,
                        NULL,
                        &timeout);
    return result > 0 && FD_ISSET(fd, &set);
}

static int connect_raw(const char *address, int port)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo *result = NULL;
    if (getaddrinfo(address, port_text, &hints, &result) != 0) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *item = result; item; item = item->ai_next) {
        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, item->ai_addr, item->ai_addrlen);
        if (rc == 0 || (errno == EINPROGRESS && wait_fd(fd, true))) {
            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size);
            if (socket_error == 0) {
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd >= 0) {
        struct timeval timeout = {
            .tv_sec = AIUI_SOCKET_TIMEOUT_MS / 1000,
            .tv_usec = (AIUI_SOCKET_TIMEOUT_MS % 1000) * 1000,
        };
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    }
    return fd;
}

AIUI_SOCKET_HANDLE aiui_socket_connect(const char *address,
                                       const char *port_text,
                                       AIUI_SOCKET_TYPE type)
{
    if (!address || !port_text) {
        return NULL;
    }
    char *end = NULL;
    long port_value = strtol(port_text, &end, 10);
    if (!end || *end != '\0' || port_value <= 0 || port_value > UINT16_MAX) {
        return NULL;
    }
    aiui_esp_socket_t *socket_handle = calloc(1, sizeof(*socket_handle));
    if (!socket_handle) {
        return NULL;
    }
    socket_handle->fd = -1;
    socket_handle->tls_enabled = type == AIUI_SOCKET_SSL;
    if (socket_handle->tls_enabled) {
        socket_handle->tls = esp_tls_init();
        if (!socket_handle->tls) {
            free(socket_handle);
            return NULL;
        }
        esp_tls_cfg_t config = {
            .timeout_ms = AIUI_SOCKET_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .common_name = address,
            .skip_common_name = false,
        };
        int rc = esp_tls_conn_new_sync(address, (int)strlen(address),
                                       (int)port_value, &config,
                                       socket_handle->tls);
        if (rc != 1 ||
            esp_tls_get_conn_sockfd(socket_handle->tls, &socket_handle->fd) != ESP_OK) {
            ESP_LOGE(TAG, "TLS connect failed");
            esp_tls_conn_destroy(socket_handle->tls);
            free(socket_handle);
            return NULL;
        }
    } else {
        socket_handle->fd = connect_raw(address, (int)port_value);
        if (socket_handle->fd < 0) {
            free(socket_handle);
            return NULL;
        }
    }
    return socket_handle;
}

void aiui_socket_close(AIUI_SOCKET_HANDLE handle)
{
    aiui_esp_socket_t *socket_handle = (aiui_esp_socket_t *)handle;
    if (socket_handle && socket_handle->fd >= 0) {
        shutdown(socket_handle->fd, SHUT_RDWR);
    }
}

void aiui_socket_destroy(AIUI_SOCKET_HANDLE handle)
{
    aiui_esp_socket_t *socket_handle = (aiui_esp_socket_t *)handle;
    if (!socket_handle) {
        return;
    }
    if (socket_handle->tls) {
        esp_tls_conn_destroy(socket_handle->tls);
    } else if (socket_handle->fd >= 0) {
        close(socket_handle->fd);
    }
    free(socket_handle);
}

int aiui_socket_send(AIUI_SOCKET_HANDLE handle, const char *buffer, size_t length)
{
    aiui_esp_socket_t *socket_handle = (aiui_esp_socket_t *)handle;
    if (!socket_handle || socket_handle->fd < 0 || (!buffer && length)) {
        return -1;
    }
    size_t sent = 0;
    while (sent < length) {
        ssize_t rc = socket_handle->tls_enabled
            ? esp_tls_conn_write(socket_handle->tls, buffer + sent, length - sent)
            : send(socket_handle->fd, buffer + sent, length - sent, 0);
        if (rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE ||
            (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
            if (!wait_fd(socket_handle->fd, true)) {
                break;
            }
            continue;
        }
        if (rc <= 0) {
            break;
        }
        sent += (size_t)rc;
    }
    /*
     * A WebSocket frame is indivisible at this layer. Reporting a partial
     * TLS write as success makes the next frame continue in the middle of
     * the previous one and permanently desynchronizes the cloud parser.
     * Force the caller to discard this connection and reconnect instead.
     */
    if (sent != length) {
        ESP_LOGW(TAG, "socket send incomplete: %u/%u bytes",
                 (unsigned)sent, (unsigned)length);
        return -1;
    }
    return (int)sent;
}

int aiui_socket_recv(AIUI_SOCKET_HANDLE handle, char *buffer, size_t length)
{
    aiui_esp_socket_t *socket_handle = (aiui_esp_socket_t *)handle;
    if (!socket_handle || socket_handle->fd < 0 || !buffer || length == 0) {
        return -1;
    }
    /*
     * A TLS read can leave decrypted application data inside mbedTLS after
     * the underlying TCP record has been consumed. select() on the raw fd
     * cannot see those bytes. Waiting on the fd first therefore stalls the
     * final, short WebSocket frame until another TCP packet arrives or the
     * connection is closed.
     */
    const bool tls_data_buffered =
        socket_handle->tls_enabled &&
        esp_tls_get_bytes_avail(socket_handle->tls) > 0;
    if (!tls_data_buffered && !wait_fd(socket_handle->fd, false)) {
        return -1;
    }
    ssize_t rc = socket_handle->tls_enabled
        ? esp_tls_conn_read(socket_handle->tls, buffer, length)
        : recv(socket_handle->fd, buffer, length, 0);
    if (rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE ||
        (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
        return -1;
    }
    return rc > 0 ? (int)rc : -1;
}

int aiui_connect_is_ok(AIUI_SOCKET_HANDLE handle)
{
    aiui_esp_socket_t *socket_handle = (aiui_esp_socket_t *)handle;
    if (!socket_handle || socket_handle->fd < 0) {
        return -1;
    }
    if (socket_handle->tls_enabled &&
        esp_tls_get_bytes_avail(socket_handle->tls) > 0) {
        return 0;
    }
    char byte;
    int rc = recv(socket_handle->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (rc > 0 || (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return 0;
    }
    return -1;
}
