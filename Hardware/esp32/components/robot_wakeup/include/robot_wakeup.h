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

/*
 * The phrase the engine listens for right now, exactly as it was handed to
 * MultiNet (hanzi or pinyin).  Logged so "which wake word is this device on"
 * never has to be guessed.
 */
const char *robot_wakeup_phrase(void);

/*
 * Replace the wake phrase at runtime and remember it in NVS.
 *
 * MultiNet validates phrases itself and *reports* the ones it cannot parse
 * instead of failing loudly, so an unusable phrase is caught here: the previous
 * phrase is put back (a bad phrase must never leave the robot deaf) and the
 * reason is written to `error` for the caller to show.  ESP_OK means the new
 * phrase is live.
 */
esp_err_t robot_wakeup_set_phrase(const char *phrase, char *error,
                                  size_t error_size);

#ifdef __cplusplus
}
#endif
