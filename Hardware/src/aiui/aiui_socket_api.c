#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <stdlib.h>

//#define AIUI_SUPPORT_SSL
#ifdef AIUI_SUPPORT_SSL
#    include "mbedtls/net_sockets.h"
#    include "mbedtls/ssl.h"
#    include "mbedtls/entropy.h"
#    include "mbedtls/ctr_drbg.h"
#    include "mbedtls/debug.h"
#endif

#include "aiui_log.h"
#include "aiui_wrappers_os.h"
#include "aiui_socket_api.h"

#define AIUI_TAG "aiui_socket"

#define AIUI_INVALID_SOCKET -1

typedef struct aiui_socket_info_s
{
    int sock;
    AIUI_SOCKET_TYPE type;
#ifdef AIUI_SUPPORT_SSL
    mbedtls_net_context server_fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt cacert;
#endif
} aiui_socket_info_t;
static void _aiui_socket_set_nonblock(int sock, int blocked)
{
    if (sock == AIUI_INVALID_SOCKET) return;

    ioctl(sock, FIONBIO, &blocked);
}

static void _aiui_socket_set_recv_timeout(int sock, int seconds)
{
    if (sock == AIUI_INVALID_SOCKET) return;
    struct timeval tv = {seconds, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void _aiui_socket_set_send_timeout(int sock, int seconds)
{
    if (sock == AIUI_INVALID_SOCKET) return;

    struct timeval tv = {seconds, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void _aiui_socket_set_keep_alive(int sock)
{
    if (sock == AIUI_INVALID_SOCKET) return;
    unsigned long opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&opt, sizeof(opt));
}

static int _aiui_socket_is_ready(int sock)
{
    if (AIUI_INVALID_SOCKET == sock)
    {
        return 0;
    }
    struct timeval tv = {5, 0};
    fd_set set;
    fd_set rset;
    fd_set eset;
    FD_ZERO(&set);
    FD_ZERO(&rset);
    FD_ZERO(&eset);
    FD_SET(sock, &set);
    FD_SET(sock, &rset);
    FD_SET(sock, &eset);
    LOGD(AIUI_TAG, "_aiui_socket_is_ready 1");
    if (select(sock + 1, &rset, &set, &eset, &tv) > 0)
    {
        LOGD(AIUI_TAG, "_aiui_socket_is_ready 2 %d", errno);
        if (FD_ISSET(sock, &set))
        {
            int error = -1;
            socklen_t len = sizeof(error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
            if (0 == error)
            {
                return 1;
            }
        }
        if (FD_ISSET(sock, &rset))
        {
            LOGD(AIUI_TAG, "_aiui_socket_is_ready 2.1 %d", errno);
        }

        if (FD_ISSET(sock, &eset))
        {
            LOGD(AIUI_TAG, "_aiui_socket_is_ready 2. %d", errno);
        }
    }
    LOGD(AIUI_TAG, "_aiui_socket_is_ready 3 %d", errno);
    return 0;
}

static int _aiui_socket_is_send_ready(int sock)
{
    if (AIUI_INVALID_SOCKET == sock)
    {
        return 0;
    }
    struct timeval tv = {5, 0};
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    if (select(sock + 1, NULL, &set, NULL, &tv) > 0)
    {
        if (FD_ISSET(sock, &set))
        {
            return 1;
        }
    }
    return 0;
}

static int _aiui_socket_is_recv_ready(int sock)
{
    if (AIUI_INVALID_SOCKET == sock)
    {
        return 0;
    }
    struct timeval tv = {3, 0};
    fd_set rset = {0};
    fd_set except_set = {0};
    FD_ZERO(&rset);
    FD_ZERO(&except_set);
    FD_SET(sock, &rset);
    FD_SET(sock, &except_set);
    int ret = select(sock + 1, &rset, NULL, &except_set, &tv);
    if (ret > 0 && FD_ISSET(sock, &rset))
    {
        return 1;
    }
    return 0;
}
    
static AIUI_SOCKET_HANDLE _aiui_socket_connect_raw(const char *address, const char *port)
{
    aiui_socket_info_t *handle = (aiui_socket_info_t *)aiui_hal_malloc(sizeof(aiui_socket_info_t));
    handle->type = AIUI_SOCKET_RAW;
    struct sockaddr_in sin = {0};
    memset(&sin, 0, sizeof(sin));
    LOGD(AIUI_TAG, "len = %lu, address = %s", strlen(address), address);
    struct hostent *host = gethostbyname(address);
    LOGD(AIUI_TAG, "host = %p %s", host, strerror(errno));
    if (NULL == host)
    {
        aiui_hal_free(handle);
        return NULL;
    }
    handle->sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    LOGD(AIUI_TAG, "sock = %d", handle->sock);
    if (handle->sock == AIUI_INVALID_SOCKET) 
    {
        aiui_hal_free(handle);
        return NULL;
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = *((uint32_t *)host->h_addr_list[0]);
    sin.sin_port = htons(atoi(port));

	int ret = connect(handle->sock, (struct sockaddr *)&sin, sizeof(struct sockaddr));
	if (ret < 0)
	{
		if (errno != EINPROGRESS)
		{
			aiui_socket_destroy(handle);
			return NULL;
		}
	}

    if (_aiui_socket_is_ready(handle->sock) == 0)
    {
        aiui_socket_destroy(handle);
        LOGE(AIUI_TAG, "connect failed");
        return NULL;
    }
    _aiui_socket_set_send_timeout(handle->sock, 5);
    _aiui_socket_set_recv_timeout(handle->sock, 5);
    _aiui_socket_set_keep_alive(handle->sock);
    LOGD(AIUI_TAG, "connect success");
    return (void *)handle;
}

#ifdef AIUI_SUPPORT_SSL
static void my_debug(void *ctx, int level, const char *file, int line, const char *str)
{
    ((void)level);

    //printf("%s:%04d: %s", file, line, str);
    //fflush(  (FILE *) ctx  );
}

static AIUI_SOCKET_HANDLE _aiui_socket_connect_ssl(const char *address, const char *port)
{
    int ret = -1;
    aiui_socket_info_t *handle = (aiui_socket_info_t *)aiui_hal_malloc(sizeof(aiui_socket_info_t));
    handle->sock = -1;
    handle->type = AIUI_SOCKET_SSL;

//#    ifdef MBEDTLS_DEBUG_C
#    ifdef AIUI_SUPPORT_SSL
    mbedtls_debug_set_threshold(1);
#    endif

    mbedtls_net_init(&handle->server_fd);
    mbedtls_ssl_init(&handle->ssl);
    mbedtls_ssl_config_init(&handle->conf);
    mbedtls_ctr_drbg_init(&handle->ctr_drbg);
    mbedtls_x509_crt_init(&handle->cacert);
    mbedtls_entropy_init(&handle->entropy);
    mbedtls_ctr_drbg_seed(&handle->ctr_drbg, mbedtls_entropy_func, &handle->entropy, NULL, 0);
    do {
        ret = mbedtls_net_connect(&handle->server_fd, address, port, MBEDTLS_NET_PROTO_TCP);
        LOGD(AIUI_TAG, "ret = %d %s %s", ret, address, port);
        if (ret != 0) break;
        ret = mbedtls_ssl_config_defaults(&handle->conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) break;
        /* OPTIONAL is not optimal for security,
     * but makes interop easier in this simplified example */
        mbedtls_ssl_conf_authmode(&handle->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_ca_chain(&handle->conf, &handle->cacert, NULL);
        mbedtls_ssl_conf_rng(&handle->conf, mbedtls_ctr_drbg_random, &handle->ctr_drbg);
        mbedtls_ssl_conf_dbg(&handle->conf, my_debug, 0);

        ret = mbedtls_ssl_setup(&handle->ssl, &handle->conf);
        if (ret != 0) break;
        ret = mbedtls_ssl_set_hostname(&handle->ssl, address);
        if (ret != 0) break;
        mbedtls_ssl_set_bio(
            &handle->ssl, &handle->server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        while ((ret = mbedtls_ssl_handshake(&handle->ssl)) != 0)
        {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                break;
            }
        }
        /* In real life, we probably want to bail out when ret != 0 */
        /*uint32_t flags = 0;
        if ((flags = mbedtls_ssl_get_verify_result(&handle->ssl)) != 0) {
            char vrfy_buf[512];

            mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);
            printf("%s\n", vrfy_buf);
            break;
        }*/
    } while (0);
    if (ret != 0)
    {
        aiui_socket_destroy(handle);
        return NULL;
    }
    mbedtls_net_set_nonblock(&handle->server_fd);
    LOGD(AIUI_TAG, "connect success with ssl");
    return (void *)handle;
}
#endif

AIUI_SOCKET_HANDLE
aiui_socket_connect(const char *address, const char *port, AIUI_SOCKET_TYPE type)
{
#ifdef AIUI_SUPPORT_SSL
    if (AIUI_SOCKET_SSL == type)
    {
        return _aiui_socket_connect_ssl(address, port);
    }
#else
    (void) type;
    if (strcmp(port, "443") == 0) port = "80";    // 如果不支持SSL，将443端口改为80
#endif
    return _aiui_socket_connect_raw(address, port);
}

void aiui_socket_close(AIUI_SOCKET_HANDLE handle)
{
    aiui_socket_info_t *self = (aiui_socket_info_t *)handle;
    if (NULL == self)
    {
        return;
    }
#ifdef AIUI_SUPPORT_SSL
    if (self->type == AIUI_SOCKET_SSL)
    {
        shutdown(self->server_fd.fd, SHUT_RDWR);
        return;
    }
#endif
    shutdown(self->sock, SHUT_RDWR);
}

void aiui_socket_destroy(AIUI_SOCKET_HANDLE handle)
{
    aiui_socket_info_t *self = (aiui_socket_info_t *)handle;
    if (NULL == self)
    {
        return;
    }
#ifdef AIUI_SUPPORT_SSL
    if (self->type == AIUI_SOCKET_SSL)
    {
        mbedtls_net_free(&self->server_fd);
        mbedtls_x509_crt_free(&self->cacert);
        mbedtls_ssl_free(&self->ssl);
        mbedtls_ssl_config_free(&self->conf);
        mbedtls_ctr_drbg_free(&self->ctr_drbg);
        mbedtls_entropy_free(&self->entropy);
        aiui_hal_free(handle);
        return;
    }
#endif

    close(self->sock);
    aiui_hal_free(handle);
}
int aiui_socket_send(AIUI_SOCKET_HANDLE handle, const char *buf, const size_t len)
{
    aiui_socket_info_t *self = (aiui_socket_info_t *)handle;
    if (NULL == self) return -1;
    int sock = self->sock;
#ifdef AIUI_SUPPORT_SSL
    if (self->type == AIUI_SOCKET_SSL) sock = self->server_fd.fd;
#endif
    if (sock < 0) return -1;
    size_t left_write = len;
    if (_aiui_socket_is_send_ready(sock) <= 0) return -1;
    int ret = -1;
    while (left_write > 0) {
#ifdef AIUI_SUPPORT_SSL
        if (self->type == AIUI_SOCKET_SSL)
        {
            ret = mbedtls_ssl_write(&self->ssl, (const unsigned char *)buf, left_write);
        }
        else
        {
            ret = send(sock, buf, left_write, 0);
        }
#else
        ret = send(sock, buf, left_write, 0);
#endif
        //LOGD(AIUI_TAG, "send ret = %d", ret);

        if (ret <= 0)
        {
            break;
        }

        buf += ret;
        left_write -= ret;
    }
    return len - left_write;
}

int aiui_socket_recv(AIUI_SOCKET_HANDLE handle, char *buf, const size_t len)
{
    aiui_socket_info_t *self = (aiui_socket_info_t *)handle;
    if (NULL == self)
    {
        return -1;
    }
    int sock = self->sock;
#ifdef AIUI_SUPPORT_SSL
    if (self->type == AIUI_SOCKET_SSL)
    {
        sock = self->server_fd.fd;
    }
#endif
    if (sock < 0)
    {
        return -1;
    }
    if (_aiui_socket_is_recv_ready(sock) <= 0)
    {
        return -1;
    }

    int left_read = len;
    int ret = -1;
    while (left_read > 0)
    {
#ifdef AIUI_SUPPORT_SSL
        if (self->type == AIUI_SOCKET_SSL)
        {
            ret = mbedtls_ssl_read(&self->ssl, (unsigned char *)buf, left_read);
        }
        else
        {
            ret = recv(self->sock, buf, left_read, 0);
        }
#else
        ret = recv(self->sock, buf, left_read, 0);
#endif
        if (ret <= 0)
        {
            break;
        }
        buf += ret;
        left_read -= ret;
    }
    return len - left_read;
}

int aiui_connect_is_ok(AIUI_SOCKET_HANDLE handle)
{
    aiui_socket_info_t *self = (aiui_socket_info_t *)handle;
    if (NULL == self)
    {
        return -1;
    }
    int sock = self->sock;
#ifdef AIUI_SUPPORT_SSL
    if (self->type == AIUI_SOCKET_SSL)
    {
        sock = self->server_fd.fd;
    }
#endif
    if (sock < 0)
    {
        return -1;
    }

    char buffer[1];
    int ret = recv(sock, buffer, 1, MSG_PEEK);
    // 对于正常的socket 1：缓冲区有数据， 返回1；2：缓冲区无数据，返回-1， errno 为 EWOULDBLOCK或EAGAIN
    if (ret > 0 || (-1 == ret && (errno == EWOULDBLOCK || errno == EAGAIN)))
    {
        return 0;
    }
    // 对端关闭,返回0
    LOGD(AIUI_TAG, "ret = %d, errno = %d", ret, errno);
    return -1;
}
