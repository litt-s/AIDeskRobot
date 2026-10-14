#include "robot_network.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

#define GOT_IP_BIT BIT0
#define REPROVISION_BIT BIT1
#define DNS_PORT 53
#define FORM_LIMIT 2048
#define MAX_SCAN_RESULTS 20

typedef enum {
    CONNECTION_IDLE = 0,
    CONNECTION_CONNECTING,
    CONNECTION_CONNECTED,
    CONNECTION_REPROVISION_REQUESTED,
    CONNECTION_PROVISIONING,
} connection_state_t;

typedef struct {
    robot_network_config_t config;
    EventGroupHandle_t events;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    httpd_handle_t http;
    TaskHandle_t dns_task;
    int dns_socket;
    bool initialized;
    bool wifi_started;
    volatile bool dns_running;
    volatile bool time_ready;
    connection_state_t connection_state;
    char ap_ssid[33];
} network_state_t;

static const char *TAG = "robot_network";
static network_state_t s_network;
static portMUX_TYPE s_connection_lock = portMUX_INITIALIZER_UNLOCKED;

static connection_state_t get_connection_state(void)
{
    connection_state_t state;
    portENTER_CRITICAL(&s_connection_lock);
    state = s_network.connection_state;
    portEXIT_CRITICAL(&s_connection_lock);
    return state;
}

static void set_connection_state(connection_state_t state)
{
    portENTER_CRITICAL(&s_connection_lock);
    s_network.connection_state = state;
    portEXIT_CRITICAL(&s_connection_lock);
}

/*
 * The setup page is assembled from a static head, the current configuration and
 * a static tail, and each concern (Wi-Fi, the console device channel, the AIUI
 * credentials) has its own form and its own submit button.  One shared form was
 * the reason the two settings interfered: saving the device code meant
 * resubmitting the Wi-Fi fields too, and the empty password box then overwrote
 * the stored password, so changing the device code quietly took the network
 * down with it.  With one form per concern the browser sends only the fields of
 * the section being saved, and save_handler only writes that section's NVS
 * namespace - whatever is not submitted is not written.
 */
static const char PROVISION_PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>DeskEmoji Setup</title><style>"
    "*{box-sizing:border-box}body{font:16px system-ui,sans-serif;max-width:520px;"
    "margin:24px auto;padding:0 16px;background:#f4f6f8;color:#17212b}"
    ".card{background:#fff;border-radius:16px;padding:20px;box-shadow:0 4px 18px "
    "#0001;margin-top:16px}"
    "h2{margin:0 0 6px}h3{margin:0}"
    ".hint{color:#637180;margin:8px 0 0}"
    "#state{margin:0;color:#183243}"
    "label{display:block;font-weight:600;margin-top:16px}"
    "input{width:100%;padding:12px;margin-top:7px;border:1px solid #c9d2dc;"
    "border-radius:10px;font-size:16px}"
    "#wifi-list{margin-top:8px;border:1px solid #d8e0e8;border-radius:12px;"
    "max-height:260px;overflow:auto}"
    ".wifi{display:flex;width:100%;padding:13px 12px;border:0;border-bottom:1px solid "
    "#edf0f3;background:#fff;color:#17212b;text-align:left;font-size:16px}"
    ".wifi:last-child{border-bottom:0}.wifi.selected{background:#e8f2ff;color:#075fae}"
    ".wifi-name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".wifi-meta{margin-left:8px;color:#6d7884;font-size:13px}"
    ".status{padding:18px;text-align:center;color:#687684}"
    ".secondary{width:100%;padding:10px;margin-top:10px;border:1px solid #1769aa;"
    "border-radius:10px;background:#fff;color:#1769aa;font-size:15px}"
    ".primary{width:100%;padding:13px;margin-top:18px;border:0;border-radius:10px;"
    "background:#1769aa;color:#fff;font-size:16px;font-weight:700}"
    "details{margin-top:14px}summary{cursor:pointer;color:#1769aa}"
    "</style></head><body><h2>DeskEmoji 配网</h2>"
    "<div class=card><p class=hint id=state>";

static const char PROVISION_PAGE_TAIL[] =
    "</p></div>"
    /*
     * One form, one save button.  Every box that is filled in is written and
     * every box left blank keeps whatever is stored, which is what makes the
     * Wi-Fi and the device code independent: the browser never has to resubmit
     * the Wi-Fi fields to change the device code, and a blank password box
     * cannot erase the stored password.  Nothing here is a required field, so
     * "I only want to change one thing" needs no extra gesture - just leave the
     * other boxes empty.
     */
    "<div class=card><form method=post action=/save id=setup-form>"
    "<h3>Wi-Fi</h3>"
    "<p class=hint>只想改设备码时，这一整段都可以不填。</p>"
    "<label>附近的 Wi-Fi</label><div id=wifi-list><div class=status>正在扫描…</div></div>"
    "<button class=secondary type=button id=rescan>重新扫描</button>"
    "<input type=hidden name=ssid id=ssid>"
    "<details><summary>未找到网络？手动输入</summary>"
    "<input id=manual-ssid maxlength=32 placeholder='Wi-Fi 名称'></details>"
    "<label>Wi-Fi 密码</label><input id=wifi-password name=password type=password "
    "maxlength=63 placeholder='留空则沿用已保存的密码'>"
    "<p class=hint>Wi-Fi 名留空＝Wi-Fi 完全不动；换到别的网络时请照实填名称与密码"
    "（新网络是开放网络则密码留空）。</p>"
    "<h3 style='margin-top:24px'>控制台设备通道</h3>"
    "<p class=hint>服务器地址填控制台 Worker 的地址，本地开发形如 "
    "http://192.168.1.10:8787（前端页面的 5173 不是它）；漏写 http:// 会自动补上。"
    "两项各自独立，留空的那一项保持原值。</p>"
    "<label>服务器地址</label><input name=server maxlength=127 "
    "inputmode=url autocapitalize=off autocorrect=off spellcheck=false "
    "placeholder='http://192.168.1.10:8787'>"
    "<label>设备码</label><input name=devicekey maxlength=63 "
    "autocapitalize=off autocorrect=off spellcheck=false "
    "placeholder='XXXX-XXXX-XXXX'>"
    "<h3 style='margin-top:24px'>AIUI 凭据（可选）</h3>"
    "<p class=hint>三项需同时填写；三项都不填则保持现有凭据不变。</p>"
    "<label>APPID</label><input name=appid maxlength=32>"
    "<label>APPKEY</label><input name=appkey type=password maxlength=64>"
    "<label>APPSECRET</label><input name=appsecret type=password maxlength=64>"
    "<p class=hint>没填的输入框一律保持原来的值，保存后设备自动重启。</p>"
    "<button class=primary type=submit>保存并重启</button>"
    "</form></div>"
    "<script>"
    "const list=document.getElementById('wifi-list'),ssid=document.getElementById('ssid'),"
    "manual=document.getElementById('manual-ssid'),rescan=document.getElementById('rescan'),"
    "password=document.getElementById('wifi-password'),"
    "current=document.getElementById('current-ssid');"
    "function choose(name,secure,button){ssid.value=name;manual.value='';"
    "document.querySelectorAll('.wifi').forEach(x=>x.classList.remove('selected'));"
    "button.classList.add('selected');button.dataset.secure=secure}"
    "async function scan(){rescan.disabled=true;list.replaceChildren();"
    "const waiting=document.createElement('div');waiting.className='status';"
    "waiting.textContent='正在扫描附近的 Wi-Fi…';list.append(waiting);"
    "try{const response=await fetch('/scan',{cache:'no-store'});"
    "if(!response.ok)throw new Error();const data=await response.json();"
    "list.replaceChildren();if(!data.networks.length){waiting.textContent='未发现 Wi-Fi，"
    "请重新扫描或手动输入';list.append(waiting);return}"
    "data.networks.forEach(network=>{const button=document.createElement('button');"
    "button.type='button';button.className='wifi';"
    "const name=document.createElement('span');name.className='wifi-name';"
    "name.textContent=network.ssid;const meta=document.createElement('span');"
    "meta.className='wifi-meta';meta.textContent=(network.secure?'加密 ':'开放 ')+"
    "network.rssi+' dBm';button.append(name,meta);"
    "button.addEventListener('click',()=>choose(network.ssid,"
    "network.secure===false?'0':'1',button));list.append(button)})"
    "}catch(e){waiting.textContent='扫描失败，请点击重新扫描';list.replaceChildren(waiting)}"
    "finally{rescan.disabled=false}}"
    "rescan.addEventListener('click',scan);manual.addEventListener('input',()=>{"
    "ssid.value=manual.value;document.querySelectorAll('.wifi').forEach("
    "x=>x.classList.remove('selected'))});"
    /*
     * Only complain when a Wi-Fi is actually being set: an empty network box is
     * a legitimate "leave the Wi-Fi alone", and re-picking the network already
     * in use may legitimately leave the password box empty.
     */
    "document.getElementById('setup-form').addEventListener('submit',event=>{"
    "if(!ssid.value.trim()){return}"
    "const selected=document.querySelector('.wifi.selected');"
    "if(!selected||selected.dataset.secure!=='1'||password.value){return}"
    "const chosen=selected.querySelector('.wifi-name');"
    "if(chosen&&current&&chosen.textContent===current.textContent){return}"
    "event.preventDefault();alert('该网络是加密网络，请填写 Wi-Fi 密码')});"
    "scan();</script></body></html>";/* Escapes text for HTML and streams it in pieces.  Building the whole line at
 * once would need a buffer sized for the worst case message, and this runs on
 * the HTTP server task, whose stack is shared with every other request. */
static esp_err_t send_escaped_text(httpd_req_t *request, const char *text)
{
    char buffer[96];
    size_t used = 0;
    for (const char *cursor = text; *cursor; ++cursor) {
        const char *escape = NULL;
        switch (*cursor) {
        case '&': escape = "&amp;"; break;
        case '<': escape = "&lt;"; break;
        case '>': escape = "&gt;"; break;
        case '"': escape = "&quot;"; break;
        case '\'': escape = "&#39;"; break;
        default: break;
        }
        size_t length = escape ? strlen(escape) : 1;
        if (used + length + 1 > sizeof(buffer)) {
            esp_err_t err = httpd_resp_send_chunk(request, buffer, (ssize_t)used);
            if (err != ESP_OK) {
                return err;
            }
            used = 0;
        }
        if (escape) {
            memcpy(buffer + used, escape, length);
        } else {
            buffer[used] = *cursor;
        }
        used += length;
    }
    if (used == 0) {
        return ESP_OK;
    }
    return httpd_resp_send_chunk(request, buffer, (ssize_t)used);
}

/* Reads back what is stored, leaving unset fields empty.  Either output may be
 * NULL when the caller only wants the other one. */
static void load_saved_wifi(char *ssid, size_t ssid_size,
                            char *password, size_t password_size)
{
    if (ssid) {
        memset(ssid, 0, ssid_size);
    }
    if (password) {
        memset(password, 0, password_size);
    }
    nvs_handle_t nvs = 0;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    if (ssid) {
        size_t length = ssid_size;
        if (nvs_get_str(nvs, "ssid", ssid, &length) != ESP_OK) {
            ssid[0] = '\0';
        }
    }
    if (password) {
        size_t length = password_size;
        if (nvs_get_str(nvs, "password", password, &length) != ESP_OK) {
            password[0] = '\0';
        }
    }
    nvs_close(nvs);
}

/* Mirrors the console's mask (....-....-ABCD) so both ends describe the same
 * credential the same way; the full device code never leaves NVS. */
static void device_key_mask(const char *key, char *output, size_t output_size)
{
    char normalized[65] = {0};
    size_t kept = 0;
    for (const char *cursor = key; *cursor && kept + 1 < sizeof(normalized);
         ++cursor) {
        if (isalnum((unsigned char)*cursor)) {
            normalized[kept++] = (char)toupper((unsigned char)*cursor);
        }
    }
    normalized[kept] = '\0';
    const char *tail = normalized + (kept > 4 ? kept - 4 : 0);
    snprintf(output, output_size, "••••-••••-%s", tail);
}

static void emit_event(int event_id)
{
    if (s_network.config.event_cb) {
        s_network.config.event_cb(s_network.config.event_ctx, event_id);
    }
}

static void url_decode(char *value)
{
    char *src = value;
    char *dst = value;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            ++src;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtoul(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_value(const char *form, const char *key,
                       char *output, size_t output_size)
{
    size_t key_len = strlen(key);
    for (const char *item = form; item && *item;) {
        const char *next = strchr(item, '&');
        size_t item_len = next ? (size_t)(next - item) : strlen(item);
        if (item_len > key_len &&
            memcmp(item, key, key_len) == 0 && item[key_len] == '=') {
            size_t value_len = item_len - key_len - 1;
            if (value_len >= output_size) {
                value_len = output_size - 1;
            }
            memcpy(output, item + key_len + 1, value_len);
            output[value_len] = '\0';
            url_decode(output);
            return true;
        }
        item = next ? next + 1 : NULL;
    }
    return false;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    char ssid[33] = {0};
    /* Read as well so the page can say whether a password is stored at all:
     * an empty stored password is invisible otherwise, and it is exactly the
     * state that leaves a robot unable to connect while the page says nothing. */
    char password[64] = {0};
    char key_mask[32] = {0};
    robot_cloud_credentials_t cloud = {0};
    robot_aiui_credentials_t aiui = {0};

    load_saved_wifi(ssid, sizeof(ssid), password, sizeof(password));
    const bool have_cloud =
        robot_network_load_cloud_credentials(&cloud) == ESP_OK;
    const bool have_aiui =
        robot_network_load_aiui_credentials(&aiui) == ESP_OK;
    if (have_cloud && cloud.device_key[0]) {
        device_key_mask(cloud.device_key, key_mask, sizeof(key_mask));
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(request, PROVISION_PAGE_HEAD,
                                          HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        err = send_escaped_text(request, "当前配置 — Wi-Fi：");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, "<b id=current-ssid>",
                                    HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, ssid[0] ? ssid : "未配置");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, "</b>", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK && ssid[0]) {
        err = send_escaped_text(request,
                                password[0] ? "（已保存密码）" : "（未保存密码）");
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, " ｜ 服务器地址：");
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, have_cloud && cloud.base_url[0]
                                             ? cloud.base_url
                                             : "未配置（用固件内置值）");
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, " ｜ 设备码：");
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, key_mask[0] ? key_mask
                                                     : "未配置（用固件内置值）");
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, " ｜ AIUI 凭据：");
    }
    if (err == ESP_OK) {
        err = send_escaped_text(request, have_aiui ? "已配置" : "未配置");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, PROVISION_PAGE_TAIL,
                                    HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, NULL, 0);
    }
    memset(password, 0, sizeof(password));
    return err;
}

static void json_escape_ssid(const uint8_t *ssid, char *output,
                             size_t output_size)
{
    size_t write = 0;
    for (size_t read = 0; ssid[read] && read < 32 && write + 1 < output_size;
         ++read) {
        uint8_t value = ssid[read];
        if ((value == '"' || value == '\\') && write + 2 < output_size) {
            output[write++] = '\\';
            output[write++] = (char)value;
        } else if (value < 0x20 && write + 6 < output_size) {
            int count = snprintf(output + write, output_size - write,
                                 "\\u%04x", value);
            if (count <= 0) {
                break;
            }
            write += (size_t)count;
        } else {
            output[write++] = (char)value;
        }
    }
    output[write] = '\0';
}

static esp_err_t scan_handler(httpd_req_t *request)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"networks\":[]}");
    }

    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "scan result failed");
    }
    uint16_t record_count =
        found > MAX_SCAN_RESULTS ? MAX_SCAN_RESULTS : found;
    wifi_ap_record_t *records =
        record_count ? calloc(record_count, sizeof(*records)) : NULL;
    if (record_count && !records) {
        esp_wifi_clear_ap_list();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }
    if (record_count) {
        err = esp_wifi_scan_get_ap_records(&record_count, records);
        if (err != ESP_OK) {
            free(records);
            esp_wifi_clear_ap_list();
            return httpd_resp_send_err(request,
                                       HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "scan result failed");
        }
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send_chunk(request, "{\"networks\":[",
                          HTTPD_RESP_USE_STRLEN);
    bool first = true;
    for (uint16_t i = 0; i < record_count; ++i) {
        records[i].ssid[sizeof(records[i].ssid) - 1] = '\0';
        if (!records[i].ssid[0] ||
            strcmp((const char *)records[i].ssid, s_network.ap_ssid) == 0) {
            continue;
        }
        bool duplicate = false;
        for (uint16_t j = 0; j < i; ++j) {
            if (strcmp((const char *)records[i].ssid,
                       (const char *)records[j].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        char escaped_ssid[193];
        char item[280];
        json_escape_ssid(records[i].ssid, escaped_ssid,
                         sizeof(escaped_ssid));
        int length = snprintf(
            item, sizeof(item),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            first ? "" : ",", escaped_ssid, records[i].rssi,
            records[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        if (length <= 0 || (size_t)length >= sizeof(item) ||
            httpd_resp_send_chunk(request, item, (ssize_t)length) != ESP_OK) {
            free(records);
            httpd_resp_send_chunk(request, NULL, 0);
            return ESP_FAIL;
        }
        first = false;
    }
    free(records);
    httpd_resp_send_chunk(request, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(request, NULL, 0);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

/*
 * One save button for the whole page.  Every box that is filled in is written
 * and every box left blank keeps whatever is stored, which is what keeps the
 * Wi-Fi and the device code independent: changing the device code no longer
 * requires touching the Wi-Fi fields, and a blank password box cannot erase a
 * stored password.  There is deliberately no required field on the page, so
 * "I only want to change one thing" needs no extra gesture.
 */
static esp_err_t send_saved_page(httpd_req_t *request, const char *heading,
                                 const char *detail, bool restart)
{
    char page[360];
    snprintf(page, sizeof(page),
             "<!doctype html><html><head><meta charset=utf-8>"
             "<meta name=viewport content='width=device-width,initial-scale=1'>"
             "<title>DeskEmoji Setup</title></head><body style='font:16px "
             "system-ui,sans-serif;max-width:520px;margin:24px auto;padding:0 16px'>"
             "<h2>%s</h2><p>%s</p></body></html>",
             heading, detail);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, page);
    if (restart) {
        xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL);
    }
    return err;
}

static void append_change(char *output, size_t output_size, const char *item)
{
    size_t used = strlen(output);
    if (used == 0) {
        snprintf(output, output_size, "%s", item);
    } else if (used + 1 < output_size) {
        snprintf(output + used, output_size - used, "、%s", item);
    }
}

static esp_err_t write_wifi(const char *ssid, const char *password)
{
    char effective[64] = {0};
    strlcpy(effective, password, sizeof(effective));
    if (!effective[0]) {
        /*
         * Same network, blank password box: keep the stored password.  That is
         * not only a convenience - an empty box that erased the password would
         * leave the robot unable to connect and back on this page, which is
         * exactly how "I only changed the device code" came to look like "the
         * Wi-Fi broke".  A genuinely open network stores an empty password
         * already, and a different network name with a blank password box is
         * treated as an open network, so both cases still behave sensibly.
         */
        char stored_ssid[33] = {0};
        char stored_password[64] = {0};
        load_saved_wifi(stored_ssid, sizeof(stored_ssid), stored_password,
                        sizeof(stored_password));
        if (stored_password[0] && strcmp(stored_ssid, ssid) == 0) {
            strlcpy(effective, stored_password, sizeof(effective));
        }
        memset(stored_password, 0, sizeof(stored_password));
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "ssid", ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "password", effective);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    memset(effective, 0, sizeof(effective));
    return err;
}

static esp_err_t write_aiui(const char *appid, const char *appkey,
                            const char *appsecret)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("aiui_secret", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "appid", appid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "appkey", appkey);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "appsecret", appsecret);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    return err;
}

/*
 * A bare host[:port] is what people actually type, and the ESP-IDF HTTP client
 * refuses it outright ("Error parse url"), leaving a robot that looks online
 * but can never reach the console.  So a missing scheme is filled in here
 * rather than stored as typed; anything that already carries a scheme is kept
 * verbatim (https included).  Returns false when the result would not fit.
 */
static bool normalize_server(const char *server, char *output, size_t output_size)
{
    if (strstr(server, "://")) {
        if (strlen(server) >= output_size) {
            return false;
        }
        strlcpy(output, server, output_size);
        return true;
    }
    return snprintf(output, output_size, "http://%s", server) <
           (int)output_size;
}

static esp_err_t write_cloud(const char *server, const char *device_key)
{
    char normalized[128] = {0};
    if (server[0] && !normalize_server(server, normalized, sizeof(normalized))) {
        return ESP_ERR_INVALID_SIZE;
    }
    /*
     * Field by field rather than all-or-nothing, so following the console to a
     * new address does not require retyping the device code and rebinding does
     * not require retyping the address.
     */
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("cloud_cfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK && normalized[0]) {
        err = nvs_set_str(nvs, "url", normalized);
    }
    if (err == ESP_OK && device_key[0]) {
        err = nvs_set_str(nvs, "key", device_key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    return err;
}

static esp_err_t save_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= FORM_LIMIT) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    char *form = calloc(1, (size_t)request->content_len + 1);
    if (!form) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int chunk = httpd_req_recv(request, form + received,
                                   (size_t)request->content_len - received);
        if (chunk <= 0) {
            break;
        }
        received += (size_t)chunk;
    }
    if (received != (size_t)request->content_len) {
        memset(form, 0, (size_t)request->content_len + 1);
        free(form);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "short form");
    }
    char ssid[33] = {0};
    char password[64] = {0};
    char appid[33] = {0};
    char appkey[65] = {0};
    char appsecret[65] = {0};
    char server[128] = {0};
    char devicekey[64] = {0};
    form_value(form, "ssid", ssid, sizeof(ssid));
    form_value(form, "password", password, sizeof(password));
    form_value(form, "appid", appid, sizeof(appid));
    form_value(form, "appkey", appkey, sizeof(appkey));
    form_value(form, "appsecret", appsecret, sizeof(appsecret));
    form_value(form, "server", server, sizeof(server));
    form_value(form, "devicekey", devicekey, sizeof(devicekey));
    memset(form, 0, (size_t)request->content_len + 1);
    free(form);

    /*
     * The AIUI credentials are the one set that cannot be written piecemeal:
     * nothing works until all three are present, so a partial set is refused
     * before anything at all is written rather than saved half-way.
     */
    const bool have_aiui = appid[0] || appkey[0] || appsecret[0];
    if (have_aiui && !(appid[0] && appkey[0] && appsecret[0])) {
        memset(password, 0, sizeof(password));
        memset(appkey, 0, sizeof(appkey));
        memset(appsecret, 0, sizeof(appsecret));
        memset(devicekey, 0, sizeof(devicekey));
        return send_saved_page(request, "AIUI 凭据不完整",
                               "APPID、APPKEY、APPSECRET 需要同时填写，本次未写入任何内容。",
                               false);
    }

    char changed[96] = {0};
    esp_err_t err = ESP_OK;
    if (ssid[0]) {
        err = write_wifi(ssid, password);
        if (err == ESP_OK) {
            append_change(changed, sizeof(changed), "Wi-Fi");
        }
    }
    if (err == ESP_OK && have_aiui) {
        err = write_aiui(appid, appkey, appsecret);
        if (err == ESP_OK) {
            append_change(changed, sizeof(changed), "AIUI 凭据");
        }
    }
    if (err == ESP_OK && (server[0] || devicekey[0])) {
        err = write_cloud(server, devicekey);
        if (err == ESP_OK) {
            if (server[0] && devicekey[0]) {
                append_change(changed, sizeof(changed), "服务器地址与设备码");
            } else if (devicekey[0]) {
                append_change(changed, sizeof(changed), "设备码");
            } else {
                append_change(changed, sizeof(changed), "服务器地址");
            }
        }
    }
    memset(password, 0, sizeof(password));
    memset(appkey, 0, sizeof(appkey));
    memset(appsecret, 0, sizeof(appsecret));
    memset(devicekey, 0, sizeof(devicekey));
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_saved_page(request, "服务器地址太长",
                               "请填写形如 http://192.168.1.10:8787 的地址（不带 http:// 会自动补上）。",
                               false);
    }
    if (err != ESP_OK) {
        return send_saved_page(request, "保存失败",
                               "写入设备存储时出错，请重试（未改动的内容保持原值）。",
                               false);
    }
    if (!changed[0]) {
        return send_saved_page(request, "没有改动",
                               "所有输入框都是空的，未写入任何内容，设备继续正常运行。",
                               false);
    }
    char detail[200];
    snprintf(detail, sizeof(detail),
             "已保存：%s。设备正在重启，没填的项保持原来的值。", changed);
    return send_saved_page(request, "保存成功", detail, true);
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t request[512];
    s_network.dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_network.dns_socket < 0) {
        s_network.dns_running = false;
        vTaskDelete(NULL);
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(s_network.dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(s_network.dns_socket, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(s_network.dns_socket);
        s_network.dns_socket = -1;
        s_network.dns_running = false;
        vTaskDelete(NULL);
    }
    while (s_network.dns_running) {
        struct sockaddr_in source;
        socklen_t source_size = sizeof(source);
        int length = recvfrom(s_network.dns_socket, request, sizeof(request) - 16, 0,
                              (struct sockaddr *)&source, &source_size);
        if (length < 12) {
            continue;
        }
        request[2] = 0x81;
        request[3] = 0x80;
        request[6] = 0;
        request[7] = 1;
        size_t pos = (size_t)length;
        const uint8_t answer[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3c, 0x00, 0x04,
            192, 168, 4, 1
        };
        memcpy(request + pos, answer, sizeof(answer));
        sendto(s_network.dns_socket, request, pos + sizeof(answer), 0,
               (struct sockaddr *)&source, source_size);
    }
    close(s_network.dns_socket);
    s_network.dns_socket = -1;
    vTaskDelete(NULL);
}

static esp_err_t start_portal(void)
{
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.lru_purge_enable = true;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_network.http, &server_config);
    if (err != ESP_OK) {
        return err;
    }
    httpd_uri_t root = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_handler,
    };
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_handler,
    };
    httpd_register_uri_handler(s_network.http, &save);
    httpd_register_uri_handler(s_network.http, &scan);
    httpd_register_uri_handler(s_network.http, &root);
    s_network.dns_running = true;
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3,
                    &s_network.dns_task) != pdPASS) {
        s_network.dns_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    connection_state_t state = get_connection_state();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED &&
        state != CONNECTION_PROVISIONING &&
        state != CONNECTION_REPROVISION_REQUESTED) {
        const bool was_online = s_network.time_ready;
        s_network.time_ready = false;
        set_connection_state(CONNECTION_CONNECTING);
        if (was_online) {
            emit_event(ROBOT_NETWORK_DISCONNECTED);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        bool accepted = false;
        portENTER_CRITICAL(&s_connection_lock);
        if (s_network.connection_state == CONNECTION_CONNECTING) {
            s_network.connection_state = CONNECTION_CONNECTED;
            accepted = true;
        }
        portEXIT_CRITICAL(&s_connection_lock);
        if (accepted) {
            xEventGroupSetBits(s_network.events, GOT_IP_BIT);
            emit_event(ROBOT_NETWORK_GOT_IP);
        }
    }
}

static esp_err_t sync_time(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        esp_netif_sntp_deinit();
        err = esp_netif_sntp_init(&config);
    }
    if (err == ESP_OK) {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    }
    if (err == ESP_OK) {
        s_network.time_ready = true;
        emit_event(ROBOT_NETWORK_TIME_READY);
    }
    return err;
}

esp_err_t robot_network_init(const robot_network_config_t *config)
{
    if (!config || s_network.initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_network, 0, sizeof(s_network));
    s_network.config = *config;
    s_network.dns_socket = -1;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_network.sta_netif = esp_netif_create_default_wifi_sta();
    s_network.ap_netif = esp_netif_create_default_wifi_ap();
    s_network.events = xEventGroupCreate();
    if (!s_network.sta_netif || !s_network.ap_netif || !s_network.events) {
        robot_network_deinit();
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) {
        robot_network_deinit();
        return err;
    }
    s_network.initialized = true;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         wifi_event, NULL);
    }
    if (err != ESP_OK) {
        robot_network_deinit();
        return err;
    }
    return ESP_OK;
}

esp_err_t robot_network_start(void)
{
    if (!s_network.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    char ssid[33] = {0};
    char password[64] = {0};
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "ssid", ssid, &ssid_size);
    }
    if (err == ESP_OK) {
        esp_err_t password_err = nvs_get_str(nvs, "password", password, &password_size);
        if (password_err == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
        } else if (password_err != ESP_OK) {
            err = password_err;
        }
    }
    if (nvs) {
        nvs_close(nvs);
    }
    if (err != ESP_OK || ssid[0] == '\0') {
        return robot_network_enter_provisioning();
    }
    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, password, sizeof(station.sta.password));
    station.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), TAG, "STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    s_network.wifi_started = true;
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to disable Wi-Fi power save: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for AIUI streaming");
    }
    xEventGroupClearBits(s_network.events, GOT_IP_BIT | REPROVISION_BIT);
    set_connection_state(CONNECTION_CONNECTING);
    esp_wifi_connect();
    unsigned seconds = s_network.config.sta_timeout_seconds
        ? s_network.config.sta_timeout_seconds : 60;
    EventBits_t bits = xEventGroupWaitBits(s_network.events,
                                           GOT_IP_BIT | REPROVISION_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(seconds * 1000U));
    if (bits & REPROVISION_BIT) {
        ESP_LOGI(TAG, "BOOT requested local provisioning during Wi-Fi connect");
        return robot_network_enter_provisioning();
    }
    if (!(bits & GOT_IP_BIT)) {
        set_connection_state(CONNECTION_IDLE);
        ESP_LOGW(TAG, "saved Wi-Fi did not connect within %u seconds", seconds);
        return robot_network_enter_provisioning();
    }
    err = sync_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NTP sync failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t robot_network_enter_provisioning(void)
{
    if (!s_network.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (get_connection_state() == CONNECTION_PROVISIONING &&
        s_network.http != NULL) {
        /*
         * Already serving the setup page.  Re-announce instead of returning
         * silently: a caller can ask again precisely because its own idea of
         * the state drifted - the app can leave its provisioning state on a
         * late event - and someone pressing BOOT for the setup page has to get
         * the screen and the prompt back rather than "nothing happened".
         */
        if (s_network.config.provisioning_cb) {
            s_network.config.provisioning_cb(s_network.config.provisioning_ctx,
                                             s_network.ap_ssid, "192.168.4.1");
        }
        return ESP_OK;
    }
    set_connection_state(CONNECTION_PROVISIONING);
    s_network.time_ready = false;
    if (s_network.wifi_started) {
        esp_wifi_stop();
        s_network.wifi_started = false;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_network.ap_ssid, sizeof(s_network.ap_ssid),
             "DeskEmoji-%02X%02X", mac[4], mac[5]);
    wifi_config_t access_point = {0};
    strlcpy((char *)access_point.ap.ssid, s_network.ap_ssid,
            sizeof(access_point.ap.ssid));
    access_point.ap.ssid_len = (uint8_t)strlen(s_network.ap_ssid);
    access_point.ap.channel = 1;
    access_point.ap.max_connection = 4;
    access_point.ap.authmode = WIFI_AUTH_OPEN;
    /*
     * Keep the STA interface enabled while serving the SoftAP. The browser
     * uses it only for nearby-network scans; connection still happens after
     * credentials are saved and the device restarts.
     */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &access_point), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start");
    s_network.wifi_started = true;
    ESP_RETURN_ON_ERROR(start_portal(), TAG, "portal");
    emit_event(ROBOT_NETWORK_PROVISIONING);
    if (s_network.config.provisioning_cb) {
        s_network.config.provisioning_cb(s_network.config.provisioning_ctx,
                                         s_network.ap_ssid, "192.168.4.1");
    }
    return ESP_OK;
}

bool robot_network_request_reprovision(void)
{
    if (!s_network.initialized || !s_network.events) {
        return false;
    }
    bool accepted = false;
    portENTER_CRITICAL(&s_connection_lock);
    if (s_network.connection_state == CONNECTION_CONNECTING) {
        s_network.connection_state = CONNECTION_REPROVISION_REQUESTED;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    if (accepted) {
        xEventGroupSetBits(s_network.events, REPROVISION_BIT);
    }
    return accepted;
}

bool robot_network_time_ready(void)
{
    return s_network.time_ready;
}

esp_err_t robot_network_sync_time(void)
{
    return sync_time();
}

esp_err_t robot_network_load_aiui_credentials(robot_aiui_credentials_t *credentials)
{
    if (!credentials) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credentials, 0, sizeof(*credentials));
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("aiui_secret", NVS_READONLY, &nvs);
    size_t appid_size = sizeof(credentials->appid);
    size_t appkey_size = sizeof(credentials->appkey);
    size_t secret_size = sizeof(credentials->appsecret);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "appid", credentials->appid, &appid_size);
    }
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "appkey", credentials->appkey, &appkey_size);
    }
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "appsecret", credentials->appsecret, &secret_size);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        memset(credentials, 0, sizeof(*credentials));
    }
    return err;
}

esp_err_t robot_network_load_cloud_credentials(robot_cloud_credentials_t *credentials)
{
    if (!credentials) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credentials, 0, sizeof(*credentials));
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("cloud_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    /*
     * Each field stands on its own, so a device provisioned with only one of
     * them still uses that one and falls back for the other.  A missing field
     * is therefore not an error here, unlike the AIUI credentials, where all
     * three are useless without the rest.
     */
    bool found = false;
    size_t url_size = sizeof(credentials->base_url);
    if (nvs_get_str(nvs, "url", credentials->base_url, &url_size) == ESP_OK) {
        found = true;
    } else {
        credentials->base_url[0] = '\0';
    }
    size_t key_size = sizeof(credentials->device_key);
    if (nvs_get_str(nvs, "key", credentials->device_key, &key_size) == ESP_OK) {
        found = true;
    } else {
        credentials->device_key[0] = '\0';
    }
    nvs_close(nvs);
    return found ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

void robot_network_stop(void)
{
    set_connection_state(CONNECTION_IDLE);
    s_network.time_ready = false;
    if (s_network.http) {
        httpd_stop(s_network.http);
        s_network.http = NULL;
    }
    s_network.dns_running = false;
    if (s_network.dns_socket >= 0) {
        shutdown(s_network.dns_socket, SHUT_RDWR);
    }
    if (s_network.wifi_started) {
        esp_wifi_stop();
        s_network.wifi_started = false;
    }
    esp_netif_sntp_deinit();
}

void robot_network_deinit(void)
{
    robot_network_stop();
    if (s_network.initialized) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event);
        esp_wifi_deinit();
    }
    if (s_network.sta_netif) {
        esp_netif_destroy_default_wifi(s_network.sta_netif);
    }
    if (s_network.ap_netif) {
        esp_netif_destroy_default_wifi(s_network.ap_netif);
    }
    if (s_network.events) {
        vEventGroupDelete(s_network.events);
    }
    memset(&s_network, 0, sizeof(s_network));
}
