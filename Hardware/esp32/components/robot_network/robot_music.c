#include "robot_music.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "robot_network.h"

#define MUSIC_HOST "ocp-music-api.cn-huabei-1.xf-yun.com"
#define MUSIC_GET_URL_PATH "/manage/netease/redirect/getUrl"
#define MUSIC_GET_URL_BASE "https://" MUSIC_HOST MUSIC_GET_URL_PATH
#define MUSIC_REDIRECT_PATH "/manage/netease/redirect"
#define MUSIC_REDIRECT_BASE "https://" MUSIC_HOST MUSIC_REDIRECT_PATH
#define MUSIC_PLAY_URL_COMMAND \
    "/openapi/music/basic/song/playurl/get/v2"
#define MUSIC_SEARCH_COMMAND \
    "/openapi/music/basic/search/song/get/v2"
#define HTTP_TIMEOUT_MS 10000
#define HTTP_RESPONSE_CAPACITY 32768

static const char *TAG = "robot_music";

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} http_response_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_response_t *response = (http_response_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response ||
        !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    size_t available = response->capacity > response->length
                           ? response->capacity - response->length - 1
                           : 0;
    size_t incoming = (size_t)event->data_len;
    size_t copy = incoming < available ? incoming : available;
    if (copy > 0) {
        memcpy(response->data + response->length, event->data, copy);
        response->length += copy;
        response->data[response->length] = '\0';
    }
    if (copy != incoming) {
        response->overflow = true;
    }
    return ESP_OK;
}

static esp_err_t http_request(const char *url,
                              esp_http_client_method_t method,
                              const char *body,
                              char *response_data,
                              size_t response_capacity)
{
    if (!url || !response_data || response_capacity < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    response_data[0] = '\0';
    http_response_t response = {
        .data = response_data,
        .capacity = response_capacity,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event,
        .user_data = &response,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type",
                                   "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK
                     ? esp_http_client_get_status_code(client)
                     : 0;
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    if (response.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP request failed with status=%d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t get_client_ip(char *value, size_t value_size)
{
    esp_netif_t *netif =
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK ||
        info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int length = snprintf(value, value_size, IPSTR, IP2STR(&info.ip));
    return length > 0 && length < (int)value_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t url_encode(const char *source, char *dest,
                            size_t dest_size)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!source || !dest || dest_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t written = 0;
    for (const unsigned char *p = (const unsigned char *)source; *p; ++p) {
        bool safe = isalnum(*p) || *p == '-' || *p == '_' ||
                    *p == '.' || *p == '~';
        size_t need = safe ? 1 : 3;
        if (written + need >= dest_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (safe) {
            dest[written++] = (char)*p;
        } else {
            dest[written++] = '%';
            dest[written++] = hex[*p >> 4];
            dest[written++] = hex[*p & 0x0f];
        }
    }
    dest[written] = '\0';
    return ESP_OK;
}

static esp_err_t make_authorization(
    const robot_aiui_credentials_t *credentials,
    const char *date,
    const char *path,
    char *authorization,
    size_t authorization_size)
{
    char signature_origin[256];
    int length = snprintf(signature_origin, sizeof(signature_origin),
                          "host: %s\ndate: %s\nPOST %s HTTP/1.1",
                          MUSIC_HOST, date, path);
    if (length <= 0 || length >= (int)sizeof(signature_origin)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const mbedtls_md_info_t *md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    unsigned char digest[32];
    if (!md || mbedtls_md_hmac(
                   md,
                   (const unsigned char *)credentials->appsecret,
                   strlen(credentials->appsecret),
                   (const unsigned char *)signature_origin,
                   strlen(signature_origin), digest) != 0) {
        return ESP_FAIL;
    }

    unsigned char signature[64] = {0};
    size_t signature_length = 0;
    if (mbedtls_base64_encode(signature, sizeof(signature),
                              &signature_length, digest,
                              sizeof(digest)) != 0) {
        return ESP_FAIL;
    }
    char origin[512];
    length = snprintf(
        origin, sizeof(origin),
        "api_key=\"%s\", algorithm=\"hmac-sha256\", "
        "headers=\"host date request-line\", signature=\"%s\"",
        credentials->appkey, signature);
    if (length <= 0 || length >= (int)sizeof(origin)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t encoded_length = 0;
    if (mbedtls_base64_encode(
            (unsigned char *)authorization, authorization_size,
            &encoded_length, (const unsigned char *)origin,
            strlen(origin)) != 0 ||
        encoded_length >= authorization_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    authorization[encoded_length] = '\0';
    return ESP_OK;
}

static esp_err_t gateway_post(const char *path,
                              const char *base_url,
                              const char *body,
                              char *response,
                              size_t response_size)
{
    if (!path || !base_url || !body || !response || response_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    robot_aiui_credentials_t credentials = {0};
    esp_err_t err = robot_network_load_aiui_credentials(&credentials);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial_number[20];
    snprintf(serial_number, sizeof(serial_number),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char client_ip[IP4ADDR_STRLEN_MAX];
    err = get_client_ip(client_ip, sizeof(client_ip));
    if (err != ESP_OK) {
        memset(&credentials, 0, sizeof(credentials));
        return err;
    }

    time_t now = time(NULL);
    struct tm utc = {0};
    gmtime_r(&now, &utc);
    char date[64] = {0};
    strftime(date, sizeof(date),
             "%a, %d %b %Y %H:%M:%S GMT", &utc);
    char authorization[768] = {0};
    err = make_authorization(&credentials, date, path, authorization,
                             sizeof(authorization));

    char encoded_date[192];
    char encoded_authorization[2304];
    char encoded_appid[128];
    char encoded_serial[64];
    char encoded_ip[64];
    if (err == ESP_OK) {
        err = url_encode(date, encoded_date, sizeof(encoded_date));
    }
    if (err == ESP_OK) {
        err = url_encode(authorization, encoded_authorization,
                         sizeof(encoded_authorization));
    }
    if (err == ESP_OK) {
        err = url_encode(credentials.appid, encoded_appid,
                         sizeof(encoded_appid));
    }
    if (err == ESP_OK) {
        err = url_encode(serial_number, encoded_serial,
                         sizeof(encoded_serial));
    }
    if (err == ESP_OK) {
        err = url_encode(client_ip, encoded_ip, sizeof(encoded_ip));
    }
    memset(&credentials, 0, sizeof(credentials));
    if (err != ESP_OK) {
        memset(authorization, 0, sizeof(authorization));
        return err;
    }

    char *request_url = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (!request_url) {
        request_url = malloc(4096);
    }
    if (!request_url) {
        memset(authorization, 0, sizeof(authorization));
        memset(encoded_authorization, 0, sizeof(encoded_authorization));
        return ESP_ERR_NO_MEM;
    }
    int request_length = snprintf(
        request_url, 4096,
        "%s?authorization=%s&date=%s&host=%s&appId=%s&serialNumber=%s"
        "&clientIp=%s",
        base_url, encoded_authorization, encoded_date, MUSIC_HOST,
        encoded_appid, encoded_serial, encoded_ip);
    memset(authorization, 0, sizeof(authorization));
    memset(encoded_authorization, 0, sizeof(encoded_authorization));
    if (request_length <= 0 || request_length >= 4096) {
        memset(request_url, 0, 4096);
        free(request_url);
        return ESP_ERR_INVALID_SIZE;
    }

    err = http_request(request_url, HTTP_METHOD_POST, body, response,
                       response_size);
    memset(request_url, 0, 4096);
    free(request_url);
    return err;
}

static bool json_false(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_IsObject(object)
                            ? cJSON_GetObjectItemCaseSensitive(object, key)
                            : NULL;
    return cJSON_IsFalse(value) ||
           (cJSON_IsNumber(value) && value->valueint == 0) ||
           (cJSON_IsString(value) && value->valuestring &&
            strcmp(value->valuestring, "false") == 0);
}

static bool find_first_song(const cJSON *node,
                            unsigned depth,
                            char *item_id,
                            size_t item_id_size,
                            char *title,
                            size_t title_size)
{
    if (!node || depth == 0) {
        return false;
    }
    if (cJSON_IsString(node) && node->valuestring &&
        (node->valuestring[0] == '{' || node->valuestring[0] == '[')) {
        cJSON *nested = cJSON_Parse(node->valuestring);
        bool found = nested && find_first_song(
            nested, depth - 1, item_id, item_id_size, title, title_size);
        cJSON_Delete(nested);
        return found;
    }
    if (cJSON_IsObject(node)) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(node, "name");
        const bool song_shape =
            cJSON_GetObjectItemCaseSensitive(node, "duration") ||
            cJSON_GetObjectItemCaseSensitive(node, "artists") ||
            cJSON_GetObjectItemCaseSensitive(node, "album") ||
            cJSON_GetObjectItemCaseSensitive(node, "playFlag");
        if (song_shape && cJSON_IsString(id) && id->valuestring &&
            id->valuestring[0] != '\0' && cJSON_IsString(name) &&
            name->valuestring && name->valuestring[0] != '\0' &&
            !json_false(node, "playFlag") && !json_false(node, "visible")) {
            if (strlcpy(item_id, id->valuestring, item_id_size) <
                    item_id_size &&
                strlcpy(title, name->valuestring, title_size) < title_size) {
                return true;
            }
        }
    }
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        if (find_first_song(child, depth - 1, item_id, item_id_size,
                            title, title_size)) {
            return true;
        }
    }
    return false;
}

static esp_err_t parse_search_result(const char *response,
                                     char *item_id,
                                     size_t item_id_size,
                                     char *title,
                                     size_t title_size)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "msg");
    int value = cJSON_IsNumber(code) ? code->valueint : -1;
    bool found = value == 0 && find_first_song(
        root, 8, item_id, item_id_size, title, title_size);
    if (!found) {
        ESP_LOGW(TAG, "song search failed: code=%d msg=\"%.120s\"",
                 value,
                 cJSON_IsString(message) && message->valuestring
                     ? message->valuestring
                     : "");
    }
    cJSON_Delete(root);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t parse_gateway_url(const char *response,
                                   char *signed_url,
                                   size_t signed_url_size)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *code =
        cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *message =
        cJSON_GetObjectItemCaseSensitive(root, "msg");
    int value = cJSON_IsNumber(code) ? code->valueint : -1;
    const cJSON *data =
        cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *url = cJSON_IsObject(data)
                           ? cJSON_GetObjectItemCaseSensitive(data, "url")
                           : NULL;
    esp_err_t err = ESP_OK;
    if (value != 0 || !cJSON_IsString(url) || !url->valuestring ||
        (strncmp(url->valuestring, "http://", 7) != 0 &&
         strncmp(url->valuestring, "https://", 8) != 0)) {
        ESP_LOGW(TAG, "signed URL request failed: code=%d msg=\"%.120s\"",
                 value,
                 cJSON_IsString(message) && message->valuestring
                     ? message->valuestring
                     : "");
        err = ESP_FAIL;
    } else if (strlcpy(signed_url, url->valuestring,
                       signed_url_size) >= signed_url_size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t parse_play_url(const char *response,
                                char *play_url,
                                size_t play_url_size)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * Production redirect responses have appeared in both documented form
     *     {"code":0,"data":{"url":"..."}}
     * and a NetEase wrapper whose root only contains msg="ok" while the
     * actual result is nested.  Treat a valid HTTP(S) URL as the authoritative
     * success signal and walk nested objects/arrays instead of requiring the
     * root code field.
     */
    bool copied = false;
    const cJSON *stack[12] = {root};
    unsigned depths[12] = {8};
    size_t stack_size = 1;
    int value = -1;
    const char *message = NULL;
    esp_err_t err = ESP_FAIL;
    while (stack_size > 0 && !copied) {
        --stack_size;
        const cJSON *node = stack[stack_size];
        const unsigned depth = depths[stack_size];
        if (!node || depth == 0) {
            continue;
        }
        if (cJSON_IsObject(node)) {
            const cJSON *code =
                cJSON_GetObjectItemCaseSensitive(node, "code");
            if (value < 0 && cJSON_IsNumber(code)) {
                value = code->valueint;
            } else if (value < 0 && cJSON_IsString(code) &&
                       code->valuestring) {
                value = atoi(code->valuestring);
            }
            const cJSON *msg =
                cJSON_GetObjectItemCaseSensitive(node, "msg");
            if (!message && cJSON_IsString(msg) && msg->valuestring) {
                message = msg->valuestring;
            }
            const cJSON *url =
                cJSON_GetObjectItemCaseSensitive(node, "url");
            if (cJSON_IsString(url) && url->valuestring &&
                (strncmp(url->valuestring, "http://", 7) == 0 ||
                 strncmp(url->valuestring, "https://", 8) == 0)) {
                if (strlcpy(play_url, url->valuestring,
                            play_url_size) >= play_url_size) {
                    err = ESP_ERR_INVALID_SIZE;
                } else {
                    copied = true;
                    err = ESP_OK;
                }
                break;
            }
        }
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (stack_size < sizeof(stack) / sizeof(stack[0])) {
                stack[stack_size] = child;
                depths[stack_size] = depth - 1;
                ++stack_size;
            }
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "play URL request failed: code=%d msg=\"%.120s\"",
                 value, message ? message : "");
    } else if (value != 0 && value != -1) {
        ESP_LOGW(TAG,
                 "play URL returned with nonzero wrapper code=%d; "
                 "valid URL accepted",
                 value);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t robot_music_resolve_play_url(const char *item_id,
                                       char *play_url,
                                       size_t play_url_size)
{
    if (!item_id || item_id[0] == '\0' || !play_url ||
        play_url_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    play_url[0] = '\0';
    char *response = heap_caps_malloc(HTTP_RESPONSE_CAPACITY,
                                      MALLOC_CAP_SPIRAM |
                                          MALLOC_CAP_8BIT);
    if (!response) {
        response = malloc(HTTP_RESPONSE_CAPACITY);
    }
    if (!response) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *body_root = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    if (!body_root || !business ||
        !cJSON_AddStringToObject(body_root, "opi_cmd",
                                 MUSIC_PLAY_URL_COMMAND) ||
        !cJSON_AddStringToObject(business, "songId", item_id)) {
        cJSON_Delete(business);
        cJSON_Delete(body_root);
        memset(response, 0, HTTP_RESPONSE_CAPACITY);
        free(response);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(body_root, "bizContent", business);
    char *body = cJSON_PrintUnformatted(body_root);
    cJSON_Delete(body_root);
    if (!body) {
        memset(response, 0, HTTP_RESPONSE_CAPACITY);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "requesting signed play URL");
    esp_err_t err = gateway_post(MUSIC_GET_URL_PATH, MUSIC_GET_URL_BASE,
                                 body, response,
                                 HTTP_RESPONSE_CAPACITY);
    cJSON_free(body);
    if (err != ESP_OK) {
        memset(response, 0, HTTP_RESPONSE_CAPACITY);
        free(response);
        return err;
    }

    char *signed_url = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM |
                                                 MALLOC_CAP_8BIT);
    if (!signed_url) {
        signed_url = malloc(4096);
    }
    if (!signed_url) {
        memset(response, 0, HTTP_RESPONSE_CAPACITY);
        free(response);
        return ESP_ERR_NO_MEM;
    }
    err = parse_gateway_url(response, signed_url, 4096);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "requesting NetEase play URL");
        err = http_request(signed_url, HTTP_METHOD_GET, NULL, response,
                           HTTP_RESPONSE_CAPACITY);
    }
    if (err == ESP_OK) {
        err = parse_play_url(response, play_url, play_url_size);
    }
    memset(signed_url, 0, 4096);
    free(signed_url);
    memset(response, 0, HTTP_RESPONSE_CAPACITY);
    free(response);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "play URL resolved");
    }
    return err;
}

esp_err_t robot_music_search_first(const char *keyword,
                                   char *item_id,
                                   size_t item_id_size,
                                   char *title,
                                   size_t title_size)
{
    if (!keyword || keyword[0] == '\0' || !item_id || item_id_size < 2 ||
        !title || title_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    item_id[0] = '\0';
    title[0] = '\0';

    cJSON *body_root = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    if (!body_root || !business ||
        !cJSON_AddStringToObject(body_root, "opi_cmd",
                                 MUSIC_SEARCH_COMMAND) ||
        !cJSON_AddStringToObject(business, "keyword", keyword) ||
        !cJSON_AddNumberToObject(business, "limit", 5) ||
        !cJSON_AddNumberToObject(business, "offset", 0)) {
        cJSON_Delete(business);
        cJSON_Delete(body_root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(body_root, "bizContent", business);
    char *body = cJSON_PrintUnformatted(body_root);
    cJSON_Delete(body_root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char *response = heap_caps_malloc(HTTP_RESPONSE_CAPACITY,
                                      MALLOC_CAP_SPIRAM |
                                          MALLOC_CAP_8BIT);
    if (!response) {
        response = malloc(HTTP_RESPONSE_CAPACITY);
    }
    if (!response) {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "searching bound NetEase account: \"%.120s\"",
             keyword);
    esp_err_t err = gateway_post(MUSIC_REDIRECT_PATH,
                                 MUSIC_REDIRECT_BASE, body, response,
                                 HTTP_RESPONSE_CAPACITY);
    cJSON_free(body);
    if (err == ESP_OK) {
        err = parse_search_result(response, item_id, item_id_size,
                                  title, title_size);
    }
    memset(response, 0, HTTP_RESPONSE_CAPACITY);
    free(response);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NetEase search matched: \"%.120s\"", title);
    }
    return err;
}
