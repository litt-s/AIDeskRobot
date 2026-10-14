#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*robot_wakeup_fn)(void *ctx);

esp_err_t robot_wakeup_init(robot_wakeup_fn callback, void *ctx);
esp_err_t robot_wakeup_start(void);
void robot_wakeup_stop(void);
void robot_wakeup_deinit(void);
esp_err_t robot_wakeup_feed_pcm(const int16_t *samples,
                                size_t sample_count,
                                uint64_t capture_ms);

#ifdef __cplusplus
}
#endif
