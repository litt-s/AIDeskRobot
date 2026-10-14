#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Declared outside the extern "C" block on purpose: the C++ caller passes a
 * static member function, and a C-linkage function pointer type would make
 * that assignment ill-formed. */
typedef esp_err_t (*robot_cloud_speak_fn)(void *ctx, const char *text);

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Server origin without a trailing slash, e.g. https://desk.example.org
     * or http://192.168.1.20:8787 while developing locally. */
    const char *base_url;
    /* Device key issued once by the console. */
    const char *device_key;
    uint32_t poll_interval_ms;
    uint32_t status_interval_ms;
    uint32_t request_timeout_ms;
    robot_cloud_speak_fn speak;
    void *speak_ctx;
} robot_cloud_config_t;

esp_err_t robot_cloud_init(const robot_cloud_config_t *config);
esp_err_t robot_cloud_start(void);
void robot_cloud_stop(void);
void robot_cloud_deinit(void);

/* True after a successful hello and while polling keeps succeeding. */
bool robot_cloud_online(void);

/* Server origin and device key as configured, so the voice channel can reuse
 * them instead of asking the app to configure the same endpoint twice. */
const char *robot_cloud_base_url(void);
const char *robot_cloud_device_key(void);

#ifdef __cplusplus
}
#endif
