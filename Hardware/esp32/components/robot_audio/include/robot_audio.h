#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*robot_pcm_consumer_fn)(void *ctx,
                                      const int16_t *samples,
                                      size_t sample_count,
                                      uint64_t capture_ms);
typedef void (*robot_playback_drained_fn)(void *ctx, uint32_t generation);
typedef void (*robot_playback_started_fn)(void *ctx, uint32_t generation);

typedef struct {
    int sample_rate_hz;
    int mic_bclk_gpio;
    int mic_ws_gpio;
    int mic_data_gpio;
    int speaker_bclk_gpio;
    int speaker_ws_gpio;
    int speaker_data_gpio;
    robot_playback_started_fn playback_started;
    void *playback_started_ctx;
    robot_playback_drained_fn playback_drained;
    void *playback_drained_ctx;
} robot_audio_config_t;

esp_err_t robot_audio_init(const robot_audio_config_t *config);
esp_err_t robot_audio_start(void);
void robot_audio_stop(void);
void robot_audio_deinit(void);
esp_err_t robot_audio_register_pcm_consumer(robot_pcm_consumer_fn consumer, void *ctx);
/* Adds an extra listener next to the primary consumer instead of replacing it.
 * The wake word engine already owns the primary slot, so a second feature that
 * needs to tap the microphone (the cloud voice recorder) must not evict it. */
esp_err_t robot_audio_add_pcm_tap(robot_pcm_consumer_fn tap, void *ctx);
esp_err_t robot_audio_remove_pcm_tap(robot_pcm_consumer_fn tap);
esp_err_t robot_audio_play_pcm(const int16_t *samples, size_t sample_count);
esp_err_t robot_audio_finish_playback(void);
uint32_t robot_audio_start_playback_stream(void);
uint32_t robot_audio_get_playback_generation(void);
void robot_audio_abort_playback(void);
esp_err_t robot_audio_play_wav_file(const char *path);
esp_err_t robot_audio_play_wakeup_tone(void);
esp_err_t robot_audio_set_volume(uint8_t volume);
uint8_t robot_audio_get_volume(void);
void robot_audio_set_capture_paused(bool paused);
bool robot_audio_is_playing(void);
size_t robot_audio_playback_buffered_samples(void);

#ifdef __cplusplus
}
#endif
