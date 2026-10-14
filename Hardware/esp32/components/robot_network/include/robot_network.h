#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*robot_network_event_fn)(void *ctx, int event_id);
typedef void (*robot_provisioning_ui_fn)(void *ctx, const char *ap_ssid, const char *url);

typedef struct {
    unsigned sta_timeout_seconds;
    robot_network_event_fn event_cb;
    void *event_ctx;
    robot_provisioning_ui_fn provisioning_cb;
    void *provisioning_ctx;
} robot_network_config_t;

/*
 * Where the console channel points.  Provisioned from the setup page rather
 * than compiled in, because the address of the machine running the console
 * changes whenever it joins a different network, and reflashing the robot to
 * follow it is not a reasonable thing to ask.  Either field may be empty, and
 * the caller is expected to fall back to its built-in default for that field
 * alone.
 */
typedef struct {
    char base_url[128];
    char device_key[64];
} robot_cloud_credentials_t;

enum {
    ROBOT_NETWORK_GOT_IP = 1,
    ROBOT_NETWORK_TIME_READY,
    ROBOT_NETWORK_DISCONNECTED,
    ROBOT_NETWORK_PROVISIONING,
};

esp_err_t robot_network_init(const robot_network_config_t *config);
esp_err_t robot_network_start(void);
esp_err_t robot_network_enter_provisioning(void);
bool robot_network_request_reprovision(void);
bool robot_network_time_ready(void);
esp_err_t robot_network_sync_time(void);
esp_err_t robot_network_load_cloud_credentials(robot_cloud_credentials_t *credentials);
void robot_network_stop(void);
void robot_network_deinit(void);

#ifdef __cplusplus
}
#endif
