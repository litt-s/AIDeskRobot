#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cloud voice channel: record one utterance, hand the whole PCM to the console
 * server, then play back the PCM it streams down.
 *
 * The device never opens a WebSocket and never speaks the model protocol; the
 * server does ASR, the model call and TTS, and answers with a framed stream of
 * {"type":"asr"|"reply"|"error"|"done"} events interleaved with raw PCM chunks.
 * That keeps this side small enough to reason about on a microcontroller.
 *
 * Reuses the server origin and device key already configured for robot_cloud.
 */

typedef struct {
    /*
     * Capture rate, in Hz.  Must match what the audio component was actually
     * configured for, because every duration in this module is derived from it
     * and it is what we tell the server to expect.
     */
    uint32_t sample_rate_hz;
    /* Hard cap on one utterance, so a stuck detector cannot fill memory. */
    uint32_t max_record_ms;
    /* Stop once this much trailing silence follows detected speech. */
    uint32_t silence_stop_ms;
    /* Ignore the silence detector for this long, so the first syllable is not
     * mistaken for a pause. */
    uint32_t min_record_ms;
    /* Level below which a frame counts as silence (16-bit PCM RMS). */
    uint32_t silence_threshold;
    /* Ceiling for the upload plus the streamed answer. */
    uint32_t request_timeout_ms;
} robot_voice_options_t;

/* Called when a turn starts and ends, so the caller can drive the UI and stop
 * the wake word engine from fighting the microphone. */
typedef void (*robot_voice_state_fn)(void *ctx, bool listening);

esp_err_t robot_voice_init(const robot_voice_options_t *options);
esp_err_t robot_voice_start(void);
void robot_voice_stop(void);
void robot_voice_deinit(void);
void robot_voice_set_state_callback(robot_voice_state_fn callback, void *ctx);

/*
 * Ask for one turn. Returns ESP_ERR_INVALID_STATE when the cloud channel is not
 * configured or when a turn is already in progress; the caller should report
 * that back as the command result rather than retrying.
 */
esp_err_t robot_voice_trigger(void);
/*
 * Ends the turn that is in progress: stops the recording if the robot is still
 * listening, and cuts the reply stream short if it is already speaking.  Safe
 * to call from the button handler on any task; does nothing when idle.
 */
void robot_voice_cancel(void);
bool robot_voice_busy(void);
/* Result of the last turn, for the command acknowledgement. */
const char *robot_voice_last_result(void);

/*
 * Speak one sentence on the robot without going through the wake word or the
 * built-in AIUI session. This is what the console's "播报" button uses on a
 * build without AIUI credentials, where the AIUI route can only answer
 * "no voice service configured".
 */
esp_err_t robot_voice_say(const char *text);

#ifdef __cplusplus
}
#endif
