#include "robot_voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "robot_audio.h"
/* For robot_cloud_base_url()/robot_cloud_device_key(): the voice channel reuses
 * the server origin and device key already configured for the console channel. */
#include "robot_cloud.h"

#define TAG "robot_voice"

#define VOICE_URL_MAX 288
/*
 * 一次从 TLS 里最多取多少字节。1 KB 在局域网够用，但换成公网托管后（RTT 几十毫秒）
 * 读请求的大小直接决定吞吐：32 KB/s 的音频要求每秒 32 次 1 KB 的读，稍有抖动就跟不上，
 * 播放就会一卡一卡。4 KB 把同样的音频摊到 8 次读，余量大得多。
 */
#define VOICE_READ_CHUNK 4096
/* Upper bound on a single utterance, independent of what the caller asks for. */
#define VOICE_MAX_RECORD_MS 30000
#define VOICE_DEFAULT_MAX_RECORD_MS 10000
#define VOICE_DEFAULT_SAMPLE_RATE 16000
#define VOICE_DEFAULT_SILENCE_STOP_MS 900
#define VOICE_DEFAULT_MIN_RECORD_MS 400
/* Safety net so a noisy room cannot keep a conversation alive forever. */
#define VOICE_MAX_TURNS_PER_CONVERSATION 30
/*
 * A reply can be long, and the next turn must not begin until it has been
 * spoken: the recording would otherwise overlap the playback, and the tap drops
 * exactly those frames.  Generous ceiling, checked against `running` and the
 * cancel flag so a stuck playback cannot wedge the voice task.
 */
#define VOICE_REPLY_DRAIN_WAIT_MS 180000
#define VOICE_DEFAULT_SILENCE_THRESHOLD 700
#define VOICE_DEFAULT_TIMEOUT_MS 45000
/* Ceiling on waiting for the wake chime to finish before recording starts. */
#define VOICE_DRAIN_WAIT_MS 2500
/* A JSON event is a short object; PCM arrives in tens-of-KB TTS chunks. Both
 * caps exist so a corrupt or hostile length field cannot exhaust the heap. */
#define VOICE_MAX_EVENT_BYTES 4096
#define VOICE_MAX_PCM_BYTES 65536
#define VOICE_POLL_INTERVAL_MS 40
/* Statistics window for the per-utterance capture report, and the shortest
 * silence worth calling a sentence break. */
#define VOICE_REPORT_FRAME_MS 20
#define VOICE_REPORT_LONG_GAP_MS 400
/* Silence kept on either side of the speech when the capture is trimmed. */
#define VOICE_TRIM_HEAD_MS 150
#define VOICE_TRIM_TAIL_MS 250
/* How much of a long internal pause survives the compressor. */
#define VOICE_SILENCE_KEEP_MS 300

/* Server -> device framing. Kept in step with server/voice/stream.ts. */
#define VOICE_FRAME_JSON 0x01
#define VOICE_FRAME_PCM 0x02
#define VOICE_FRAME_HEADER_BYTES 5

/* Why a turn ended.  The follow-up loop keys off this instead of comparing the
 * human-readable result string, which is meant for the console's command log. */
typedef enum {
    VOICE_OUTCOME_NONE = 0,
    VOICE_OUTCOME_DONE,
    VOICE_OUTCOME_NO_SPEECH,
    VOICE_OUTCOME_CANCELLED,
    VOICE_OUTCOME_ERROR,
} voice_outcome_t;

typedef struct {
    bool running;
    bool busy;
    bool recording;
    bool stop_requested;
    /* Set by robot_voice_cancel() from the button handler: ends the recording
     * and cuts the reply stream short. */
    volatile bool cancel_requested;
    /* Set by robot_voice_interrupt_listen(): the wake word cut a reply short,
     * so start another listening turn instead of ending the conversation. */
    volatile bool restart_requested;
    bool speech_seen;
    uint32_t trailing_silence_ms;
    size_t recorded_samples;
    size_t record_capacity_samples;
    int16_t *record_buffer;
    SemaphoreHandle_t trigger;
    SemaphoreHandle_t idle;
    TaskHandle_t task;
    robot_voice_options_t options;
    robot_voice_state_fn state_callback;
    void *state_ctx;
    char last_result[160];
    voice_outcome_t last_outcome;
} voice_state_t;

static voice_state_t s_voice;

/* Samples per millisecond for a 16-bit mono stream at the capture rate. */
static uint32_t voice_samples_per_ms(void)
{
    uint32_t rate = s_voice.options.sample_rate_hz;
    return rate > 0 ? rate / 1000U : 16U;
}

/* ---------- frame parser ---------- */

typedef struct {
    uint8_t header[VOICE_FRAME_HEADER_BYTES];
    size_t header_len;
    uint8_t type;
    uint8_t *payload;
    size_t payload_expected;
    size_t payload_got;
    size_t skip_remaining;
} voice_frame_t;

static void frame_reset(voice_frame_t *frame)
{
    free(frame->payload);
    memset(frame, 0, sizeof(*frame));
}

static uint32_t le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Returns true when the event carries the end-of-turn marker. */
static bool report_event(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGD(TAG, "unparsed event: %s", json);
        return false;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *kind = cJSON_IsString(type) ? type->valuestring : "";
    bool finished = strcmp(kind, "done") == 0;
    if (strcmp(kind, "asr") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, "heard: %s", text->valuestring);
        }
    } else if (strcmp(kind, "reply") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, "answer: %s", text->valuestring);
        }
    } else if (strcmp(kind, "error") == 0) {
        const cJSON *message = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(message)) {
            /* Surfaced to the console through the command acknowledgement, so
             * keep it short enough to fit rather than truncating silently. */
            snprintf(s_voice.last_result, sizeof(s_voice.last_result), "%.120s",
                     message->valuestring);
            ESP_LOGW(TAG, "server reported: %s", message->valuestring);
        }
    }
    cJSON_Delete(root);
    return finished;
}

/* ---------- capture tap ---------- */

/*
 * Runs inside the audio capture task, so it only copies samples and updates
 * detector counters; anything slower would drop microphone frames.
 */
static void voice_pcm_tap(void *ctx, const int16_t *samples, size_t sample_count, uint64_t capture_ms)
{
    (void)ctx;
    (void)capture_ms;
    if (!s_voice.recording || samples == NULL || sample_count == 0) {
        return;
    }
    /*
     * Drop frames while the wake prompt is still sounding.  The wake path
     * queues the chime and asks for a turn in the same breath, so "playback is
     * idle" when the turn starts can simply mean "has not started yet" - and the
     * chime then lands in the recording (a 20k peak ahead of the first word)
     * together with the wait that follows it, and each of those gaps is a place
     * the recogniser will split.  The prompt is short and the listener answers
     * after it, so skipping this costs at most the first syllable.
     */
    if (robot_audio_is_playing()) {
        return;
    }
    size_t room = s_voice.record_capacity_samples - s_voice.recorded_samples;
    if (sample_count > room) {
        sample_count = room;
    }
    if (sample_count > 0) {
        memcpy(s_voice.record_buffer + s_voice.recorded_samples, samples,
               sample_count * sizeof(int16_t));
        s_voice.recorded_samples += sample_count;
    }
    if (s_voice.recorded_samples >= s_voice.record_capacity_samples) {
        s_voice.stop_requested = true;
        return;
    }

    /* Compare mean-square against the squared threshold instead of taking a
     * square root: same decision, no float math in the capture task. */
    int64_t sum = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t value = samples[i];
        sum += value * value;
    }
    int64_t threshold = (int64_t)s_voice.options.silence_threshold;
    bool loud = sum >= threshold * threshold * (int64_t)sample_count;
    /* Derived from the actual capture rate; hardcoding it here would make every
     * silence timeout wrong the moment the rate changes. */
    const uint32_t per_ms = voice_samples_per_ms();
    uint32_t frame_ms = (uint32_t)(sample_count / per_ms);
    uint64_t recorded_ms = (uint64_t)s_voice.recorded_samples / per_ms;

    if (loud) {
        s_voice.speech_seen = true;
        s_voice.trailing_silence_ms = 0;
        return;
    }
    if (!s_voice.speech_seen) {
        /*
         * Nobody has spoken yet on this turn.  With a follow-up window the wait
         * is bounded here: otherwise a silent turn runs to max_record_ms and
         * uploads ten seconds of room tone, and every time the window expires
         * the robot would pay for a recognition round trip it cannot use.
         */
        if (s_voice.options.follow_up_ms > 0 &&
            recorded_ms >= s_voice.options.follow_up_ms) {
            s_voice.stop_requested = true;
        }
        return;
    }
    if (recorded_ms < s_voice.options.min_record_ms) {
        return;
    }
    s_voice.trailing_silence_ms += frame_ms;
    if (s_voice.trailing_silence_ms >= s_voice.options.silence_stop_ms) {
        s_voice.stop_requested = true;
    }
}

/* ---------- microphone and upload ---------- */

static esp_err_t voice_write_all(esp_http_client_handle_t client, const void *data, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = size;
    while (remaining > 0) {
        int wrote = esp_http_client_write(client, (const char *)cursor, (int)remaining);
        if (wrote <= 0) {
            return ESP_FAIL;
        }
        cursor += wrote;
        remaining -= (size_t)wrote;
    }
    return ESP_OK;
}

static void voice_handle_pcm(const uint8_t *payload, size_t size, bool *playback_open)
{
    if (size < sizeof(int16_t)) {
        return;
    }
    if (!*playback_open) {
#if CONFIG_ROBOT_CLOUD_VOICE_BARGE_IN
        /*
         * Barge-in: keep capturing while the robot speaks so the wake word can
         * be heard over its own reply.  The recording buffer stays clean anyway
         * - the tap drops every frame that arrives while audio is playing.
         */
#else
        /* Mute the microphone while the robot speaks, and stop the wake word
         * engine from hearing the reply as a command. */
        robot_audio_set_capture_paused(true);
#endif
        robot_audio_start_playback_stream();
        *playback_open = true;
    }
    int16_t *samples = (int16_t *)payload;
    robot_audio_play_pcm(samples, size / sizeof(int16_t));
}

/* ---------- shared response handling ---------- */

/*
 * Read a framed response stream to its end, playing PCM as it arrives and
 * logging JSON events. Shared by the conversation turn and by plain
 * announcements, so both paths agree on the framing and on how playback is
 * opened and closed.
 */
static esp_err_t voice_read_response(esp_http_client_handle_t client)
{
    uint8_t *chunk = malloc(VOICE_READ_CHUNK);
    if (chunk == NULL) {
        return ESP_ERR_NO_MEM;
    }

    voice_frame_t frame = {0};
    bool playback_open = false;
    bool done = false;

    /*
     * 收流诊断：一卡一卡到底是"服务端不给数据"还是"设备读得慢"，看这两个数就知道——
     * pcm 字节数除以耗时就是实际吞吐，音频只有 32 KB/s，低于它必然播不满。
     * 同时 robot_audio 在播放队列塞满时会打 "playback queue full"（那一块音频被丢掉）。
     */
    size_t pcm_bytes = 0;
    const TickType_t read_started = xTaskGetTickCount();

    int read = 0;
    while (!done && !s_voice.cancel_requested &&
           (read = esp_http_client_read(client, (char *)chunk, VOICE_READ_CHUNK)) > 0) {
        for (int index = 0; index < read; ++index) {
            uint8_t byte = chunk[index];

            if (frame.skip_remaining > 0) {
                frame.skip_remaining--;
                continue;
            }
            if (frame.header_len < VOICE_FRAME_HEADER_BYTES) {
                frame.header[frame.header_len++] = byte;
                if (frame.header_len == VOICE_FRAME_HEADER_BYTES) {
                    frame.type = frame.header[0];
                    size_t length = le32(&frame.header[1]);
                    size_t limit = frame.type == VOICE_FRAME_PCM ? VOICE_MAX_PCM_BYTES : VOICE_MAX_EVENT_BYTES;
                    if (length == 0) {
                        frame_reset(&frame);
                    } else if (length > limit) {
                        ESP_LOGW(TAG, "dropping oversized frame (type %u, %u bytes)",
                                 frame.type, (unsigned)length);
                        frame.header_len = 0;
                        frame.skip_remaining = length;
                    } else {
                        frame.payload = malloc(length);
                        if (frame.payload == NULL) {
                            frame.header_len = 0;
                            frame.skip_remaining = length;
                        } else {
                            frame.payload_expected = length;
                            frame.payload_got = 0;
                        }
                    }
                }
                continue;
            }

            frame.payload[frame.payload_got++] = byte;
            if (frame.payload_got < frame.payload_expected) {
                continue;
            }

            if (frame.type == VOICE_FRAME_PCM) {
                pcm_bytes += frame.payload_expected;
                voice_handle_pcm(frame.payload, frame.payload_expected, &playback_open);
            } else if (frame.type == VOICE_FRAME_JSON) {
                char *event = malloc(frame.payload_expected + 1);
                if (event != NULL) {
                    memcpy(event, frame.payload, frame.payload_expected);
                    event[frame.payload_expected] = '\0';
                    if (report_event(event)) {
                        done = true;
                    }
                    free(event);
                }
            }
            frame_reset(&frame);
        }
    }

    const bool cancelled = s_voice.cancel_requested;
    if (read < 0 && !cancelled) {
        ESP_LOGW(TAG, "stream ended early");
    }
    frame_reset(&frame);
    free(chunk);

    /*
     * ≤32 KB/s 就说明读链路跟不上播放（1 KB 读 + 公网 RTT 时代的老问题），
     * 越大越安全；同时也会打一行 queue full 的告警，那是真的丢了音频。
     */
    const uint32_t read_ms = (uint32_t)((xTaskGetTickCount() - read_started) * portTICK_PERIOD_MS);
    if (pcm_bytes > 0 && read_ms > 0) {
        ESP_LOGI(TAG, "stream done: pcm=%u bytes in %u ms = %u KB/s (need 32)",
                 (unsigned)pcm_bytes, (unsigned)read_ms,
                 (unsigned)((pcm_bytes / 1024) * 1000 / read_ms));
    }

    if (playback_open) {
        if (cancelled) {
            /* Go quiet now: drop what is still queued instead of playing it
             * out, then end the stream as usual. */
            robot_audio_abort_playback();
        }
        robot_audio_finish_playback();
        robot_audio_set_capture_paused(false);
    }
    if (cancelled) {
        return ESP_ERR_INVALID_STATE;
    }
    return done ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* Opens a POST to `path` with the device credentials, writing `body` first. */
static esp_err_t voice_open_request(const char *path, const char *content_type,
                                    const void *body, size_t body_bytes,
                                    bool report_capture_rate,
                                    esp_http_client_handle_t *out)
{
    const char *base_url = robot_cloud_base_url();
    const char *device_key = robot_cloud_device_key();
    if (base_url == NULL || base_url[0] == '\0' || device_key == NULL || device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[VOICE_URL_MAX];
    int written = snprintf(url, sizeof(url), "%s%s", base_url, path);
    if (written <= 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    char authorization[160];
    snprintf(authorization, sizeof(authorization), "Device %s", device_key);
    /*
     * Tell the server what rate the capture channel is actually running at.
     * It has to declare the same rate to the speech service, and deriving it
     * independently on each side is exactly how the two drift apart.
     */
    char sample_rate[16];
    snprintf(sample_rate, sizeof(sample_rate), "%u",
             (unsigned)s_voice.options.sample_rate_hz);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = (int)s_voice.options.request_timeout_ms,
        .keep_alive_enable = false,
        /*
         * 默认接收缓冲只有 512 字节，一帧音频（~10 KB）要拆成二十来次传输层读取；
         * 公网 RTT 一放大，音频就喂不满播放缓冲。放大到 4 KB / 2 KB 只是多几 KB 内存。
         */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    /* Only the capture request describes audio we produced; an announcement is
     * synthesised at the fixed playback rate, so the header would not apply. */
    if (report_capture_rate) {
        esp_http_client_set_header(client, "X-Sample-Rate", sample_rate);
    }

    esp_err_t err = esp_http_client_open(client, (int)body_bytes);
    if (err == ESP_OK && body_bytes > 0) {
        err = voice_write_all(client, body, body_bytes);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request to %s failed: %s", path, esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* The server answers configuration problems as a short JSON body. */
        char body_text[256];
        int got = esp_http_client_read(client, body_text, sizeof(body_text) - 1);
        body_text[got > 0 ? got : 0] = '\0';
        /* last_result is shown in the console's command log, so bound the body
         * rather than letting it push the line past the end of the buffer. */
        snprintf(s_voice.last_result, sizeof(s_voice.last_result),
                 got > 0 ? "服务端返回 HTTP %d：%.100s" : "服务端返回 HTTP %d",
                 status, body_text);
        ESP_LOGW(TAG, "%s", s_voice.last_result);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = client;
    return ESP_OK;
}

static esp_err_t voice_turn(void)
{
    esp_http_client_handle_t client = NULL;
    esp_err_t err = voice_open_request("/api/device/voice", "application/octet-stream",
                                       s_voice.record_buffer,
                                       s_voice.recorded_samples * sizeof(int16_t),
                                       true, &client);
    if (err != ESP_OK) {
        if (!s_voice.last_result[0]) {
            snprintf(s_voice.last_result, sizeof(s_voice.last_result), "音频上传失败");
        }
        s_voice.last_outcome = VOICE_OUTCOME_ERROR;
        return err;
    }

    snprintf(s_voice.last_result, sizeof(s_voice.last_result), "对话完成");
    err = voice_read_response(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Cancelled by a button press; the console's command log should say so
         * rather than keep "对话完成". */
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "已取消");
        s_voice.last_outcome = VOICE_OUTCOME_CANCELLED;
    } else {
        s_voice.last_outcome =
            err == ESP_OK ? VOICE_OUTCOME_DONE : VOICE_OUTCOME_ERROR;
    }
    return err;
}

/*
 * Speak one sentence without recording anything.  This is what the console's
 * announcement button uses; no wake word and no session are involved.
 */
esp_err_t robot_voice_say(const char *text)
{
    /*
     * 这三条是提前返回，原来自顾自 return 不写 last_result，调用方只能看到一句
     * "cloud ready prompt unavailable: "（冒号后面空的），等于没有线索。
     */
    if (s_voice.record_buffer == NULL || !s_voice.running) {
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "语音通道未启动");
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "播报文本为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_voice.busy) {
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "正在对话中");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", text);
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_voice.busy = true;
    snprintf(s_voice.last_result, sizeof(s_voice.last_result), "已播报");
    esp_http_client_handle_t client = NULL;
    esp_err_t err = voice_open_request("/api/device/say", "application/json",
                                       serialized, strlen(serialized), false, &client);
    cJSON_free(serialized);
    if (err == ESP_OK) {
        err = voice_read_response(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    s_voice.busy = false;
    ESP_LOGI(TAG, "announcement finished: %s", s_voice.last_result);
    return err;
}

/* ---------- turn orchestration ---------- */

static void voice_publish_state(bool listening)
{
    if (s_voice.state_callback != NULL) {
        s_voice.state_callback(s_voice.state_ctx, listening);
    }
}

/* Mean-square test for one 20 ms frame, shared by the report and by the
 * silence compressor. */
static bool voice_frame_is_voiced(size_t offset, size_t frame_samples)
{
    int64_t sum = 0;
    for (size_t i = 0; i < frame_samples; ++i) {
        int32_t value = s_voice.record_buffer[offset + i];
        sum += value * value;
    }
    const int64_t threshold = (int64_t)s_voice.options.silence_threshold;
    return sum >= threshold * threshold * (int64_t)frame_samples;
}

/*
 * Shortens long internal silences to VOICE_SILENCE_KEEP_MS.  The recogniser
 * treats a long pause as a sentence boundary, which is how "你能帮我做哪些
 * 工作" comes back as "你。能帮我。做哪些工作？" and then the model comments on
 * the choppy input.  The words themselves are untouched - a pause that long
 * only decides where sentences end, and one turn should stay one sentence.
 * Short pauses (below VOICE_REPORT_LONG_GAP_MS) are copied as they are, so the
 * audio is only edited where it would have been split anyway.  Returns the
 * number of samples removed.
 */
static size_t voice_compact_silence(void)
{
    const uint32_t per_ms = voice_samples_per_ms();
    const size_t frame_samples = (size_t)per_ms * VOICE_REPORT_FRAME_MS;
    if (frame_samples == 0) {
        return 0;
    }
    const size_t keep_frames = VOICE_SILENCE_KEEP_MS / VOICE_REPORT_FRAME_MS;
    const size_t long_frames = VOICE_REPORT_LONG_GAP_MS / VOICE_REPORT_FRAME_MS;
    const size_t before = s_voice.recorded_samples;
    size_t read = 0, write = 0;
    while (read + frame_samples <= before) {
        if (voice_frame_is_voiced(read, frame_samples)) {
            memmove(s_voice.record_buffer + write, s_voice.record_buffer + read,
                    frame_samples * sizeof(int16_t));
            read += frame_samples;
            write += frame_samples;
            continue;
        }
        size_t run = 1;
        while (read + (run + 1) * frame_samples <= before &&
               !voice_frame_is_voiced(read + run * frame_samples, frame_samples)) {
            ++run;
        }
        size_t keep = run;
        if (run >= long_frames && keep > keep_frames) {
            keep = keep_frames;
        }
        memmove(s_voice.record_buffer + write, s_voice.record_buffer + read,
                keep * frame_samples * sizeof(int16_t));
        read += run * frame_samples;
        write += keep * frame_samples;
    }
    /* Keep the trailing partial frame (up to one frame) so nothing at the very
     * end is lost to rounding. */
    if (read < before) {
        memmove(s_voice.record_buffer + write, s_voice.record_buffer + read,
                (before - read) * sizeof(int16_t));
        write += before - read;
    }
    s_voice.recorded_samples = write;
    return before - write;
}
/*
 * Finalises the capture: one summary line per utterance, then the silence
 * around the speech is trimmed away.
 *
 * A fragmented transcript ("介绍。1下。你自己。") has two possible causes with
 * opposite fixes: the capture dropping frames, or pauses that really are in the
 * audio.  Comparing the buffered audio against the wall-clock duration
 * separates them - if the buffer holds as much as the recording lasted, capture
 * kept up - and the gap figures then say what the recogniser had to split on.
 * A gap is counted only when speech follows it, so the harmless trailing
 * silence is not reported as a break.
 *
 * The trim matters because recording opens when the turn is requested, not when
 * the listener starts talking: the head can hold leftover prompt bleed and the
 * wait while the beep finishes, and each of those becomes a sentence break for
 * the recogniser.  A margin is kept on both sides so a soft first syllable is
 * not clipped.
 */
static void voice_finalise_capture(uint64_t wall_ms)
{
    const uint32_t per_ms = voice_samples_per_ms();
    const size_t frame_samples = (size_t)per_ms * VOICE_REPORT_FRAME_MS;
    const size_t head_margin = (size_t)per_ms * VOICE_TRIM_HEAD_MS;
    const size_t tail_margin = (size_t)per_ms * VOICE_TRIM_TAIL_MS;
    const int64_t threshold = (int64_t)s_voice.options.silence_threshold;
    const uint32_t captured_ms = (uint32_t)(s_voice.recorded_samples / per_ms);
    uint32_t voiced_ms = 0, longest_gap_ms = 0, gap_ms = 0, long_gaps = 0;
    size_t first_voiced = 0, last_voiced_end = 0;
    bool seen_voice = false;
    for (size_t offset = 0;
         frame_samples != 0 && offset + frame_samples <= s_voice.recorded_samples;
         offset += frame_samples) {
        int64_t sum = 0;
        for (size_t i = 0; i < frame_samples; ++i) {
            int32_t value = s_voice.record_buffer[offset + i];
            sum += value * value;
        }
        if (sum >= threshold * threshold * (int64_t)frame_samples) {
            voiced_ms += VOICE_REPORT_FRAME_MS;
            if (!seen_voice) {
                first_voiced = offset;
                seen_voice = true;
            }
            last_voiced_end = offset + frame_samples;
            if (gap_ms > longest_gap_ms) {
                longest_gap_ms = gap_ms;
            }
            if (gap_ms >= VOICE_REPORT_LONG_GAP_MS) {
                ++long_gaps;
            }
            gap_ms = 0;
        } else {
            gap_ms += VOICE_REPORT_FRAME_MS;
        }
    }
    uint32_t trimmed_ms = 0;
    if (seen_voice) {
        const size_t start =
            first_voiced > head_margin ? first_voiced - head_margin : 0;
        size_t end = last_voiced_end + tail_margin;
        if (end > s_voice.recorded_samples) {
            end = s_voice.recorded_samples;
        }
        if (end > start && (start > 0 || end < s_voice.recorded_samples)) {
            memmove(s_voice.record_buffer, s_voice.record_buffer + start,
                    (end - start) * sizeof(int16_t));
            s_voice.recorded_samples = end - start;
            trimmed_ms = captured_ms - (uint32_t)(s_voice.recorded_samples / per_ms);
        }
    }
    const uint32_t squeezed_ms =
        (uint32_t)(voice_compact_silence() / per_ms);
    ESP_LOGI(TAG,
             "capture report: %u ms of audio over %u ms wall, voiced %u ms, "
             "longest mid-speech gap %u ms, gaps >= %u ms: %u, trimmed %u ms, "
             "squeezed %u ms",
             (unsigned)captured_ms, (unsigned)wall_ms, (unsigned)voiced_ms,
             (unsigned)longest_gap_ms, VOICE_REPORT_LONG_GAP_MS,
             (unsigned)long_gaps, (unsigned)trimmed_ms, (unsigned)squeezed_ms);
}
/*
 * True while the robot still owes the listener audio.  `robot_audio_is_playing`
 * only turns true once the playback task has enough buffered audio to start
 * writing, so a reply that is still filling its pre-buffer looks idle - that
 * gap is what let the next turn begin on top of the previous reply.
 */
static bool voice_playback_pending(void)
{
    return robot_audio_is_playing() ||
           robot_audio_playback_buffered_samples() > 0;
}

static void voice_perform_turn(void)
{
    /*
     * The wake chime may still be sounding, and capture is paused while it
     * plays. Waiting for it to drain (rather than unpausing immediately) keeps
     * the beep out of the front of every utterance.
     */
    int64_t wait_started_us = esp_timer_get_time();
    while (s_voice.running && !s_voice.cancel_requested &&
           voice_playback_pending() &&
           (esp_timer_get_time() - wait_started_us) / 1000 < VOICE_REPLY_DRAIN_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_INTERVAL_MS));
    }
    if (s_voice.cancel_requested) {
        /* Interrupted while the previous reply was still playing: drop what is
         * queued so the robot goes quiet now, and let the restart path in
         * voice_run_turn() open a fresh turn. */
        robot_audio_abort_playback();
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "已取消");
        s_voice.last_outcome = VOICE_OUTCOME_CANCELLED;
        return;
    }
    robot_audio_set_capture_paused(false);

    if (robot_audio_add_pcm_tap(voice_pcm_tap, NULL) != ESP_OK) {
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "无法接入麦克风");
        return;
    }

    s_voice.recorded_samples = 0;
    s_voice.last_outcome = VOICE_OUTCOME_NONE;
    s_voice.speech_seen = false;
    s_voice.trailing_silence_ms = 0;
    s_voice.stop_requested = false;
    s_voice.recording = true;

    int64_t started_us = esp_timer_get_time();
    while (s_voice.running && !s_voice.stop_requested) {
        const int64_t elapsed_ms =
            (esp_timer_get_time() - started_us) / 1000;
        if (elapsed_ms >= (int64_t)s_voice.options.max_record_ms) {
            break;
        }
        /*
         * The "nobody spoke" timeout normally lives in the capture tap, which
         * only runs when frames arrive.  Bound it here as well so a stalled
         * capture cannot burn the whole recording budget before giving up.
         */
        if (s_voice.options.follow_up_ms > 0 && !s_voice.speech_seen &&
            elapsed_ms >= (int64_t)s_voice.options.follow_up_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_INTERVAL_MS));
    }

    uint64_t wall_ms = (uint64_t)((esp_timer_get_time() - started_us) / 1000);
    s_voice.recording = false;
    robot_audio_remove_pcm_tap(voice_pcm_tap);
    voice_finalise_capture(wall_ms);
    if (s_voice.cancel_requested) {
        /* Pressed while the robot was still listening: nothing worth sending. */
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "已取消");
        s_voice.last_outcome = VOICE_OUTCOME_CANCELLED;
        return;
    }

    if (!s_voice.speech_seen) {
        /* The window expired without a word: report it and upload nothing. */
        ESP_LOGI(TAG, "no speech within %u ms; nothing uploaded",
                 (unsigned)s_voice.options.follow_up_ms);
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "没有听到声音");
        s_voice.last_outcome = VOICE_OUTCOME_NO_SPEECH;
        return;
    }

    if (s_voice.recorded_samples * sizeof(int16_t) < 3200) {
        /* Under 100 ms of audio: almost certainly a stray trigger, not speech.
         * Sending it would only buy a round trip and an empty transcript. */
        snprintf(s_voice.last_result, sizeof(s_voice.last_result), "没有听到声音");
        s_voice.last_outcome = VOICE_OUTCOME_NO_SPEECH;
        return;
    }

    ESP_LOGI(TAG, "sending %u bytes of audio", (unsigned)(s_voice.recorded_samples * sizeof(int16_t)));
    voice_turn();
}

/*
 * The start/finish pair is published around every path, including the ones that
 * bail out early: the app moves into CLOUD_VOICE_ACTIVE as soon as a turn is
 * requested, and it would never leave that state if a failure skipped the
 * notification.
 */
static void voice_run_turn(void)
{
    voice_publish_state(true);

    uint32_t turns = 0;
    for (;;) {
        /* A cancel belongs to the turn it was pressed during: clear it here so a
         * press landing just after the previous turn ended cannot kill this
         * one. */
        s_voice.cancel_requested = false;
        voice_perform_turn();
        ++turns;

        if (s_voice.restart_requested) {
            s_voice.restart_requested = false;
            if (!s_voice.running) {
                break;
            }
            /* The wake word cut the reply short on purpose: listen for the new
             * question instead of going back to the wake word or ending here.
             * The turn ceiling starts over because this was the user asking. */
            ESP_LOGI(TAG, "interrupted by the wake word: listening again");
            turns = 0;
            continue;
        }
        /*
         * Continuous conversation: after a reply that finished normally, listen
         * again without going back to the wake word.  Every other outcome -
         * silence, a cancelled turn, an error - ends the conversation here, so
         * the robot cannot sit in a listening loop that nobody is talking to.
         */
        if (s_voice.options.follow_up_ms == 0 ||
            s_voice.last_outcome != VOICE_OUTCOME_DONE) {
            break;
        }
        if (turns >= VOICE_MAX_TURNS_PER_CONVERSATION) {
            ESP_LOGW(TAG, "conversation hit the %u turn ceiling",
                     (unsigned)turns);
            break;
        }
        if (!s_voice.running) {
            break;
        }
        ESP_LOGI(TAG, "listening again: conversation turn %u",
                 (unsigned)(turns + 1));
    }
    if (turns > 1) {
        ESP_LOGI(TAG, "conversation ended after %u turns: %s", (unsigned)turns,
                 s_voice.last_result);
    }
    voice_publish_state(false);
}

static void voice_task(void *arg)
{
    (void)arg;
    while (s_voice.running) {
        if (xSemaphoreTake(s_voice.trigger, pdMS_TO_TICKS(500)) != pdPASS) {
            continue;
        }
        if (!s_voice.running) {
            break;
        }
        voice_run_turn();
        s_voice.busy = false;
        if (s_voice.idle != NULL) {
            xSemaphoreGive(s_voice.idle);
        }
    }
    s_voice.task = NULL;
    vTaskDelete(NULL);
}

/* ---------- public API ---------- */

esp_err_t robot_voice_init(const robot_voice_options_t *options)
{
    if (s_voice.record_buffer != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_voice, 0, sizeof(s_voice));
    if (options != NULL) {
        s_voice.options = *options;
    }
    if (s_voice.options.sample_rate_hz == 0) {
        s_voice.options.sample_rate_hz = VOICE_DEFAULT_SAMPLE_RATE;
    }
    if (s_voice.options.max_record_ms == 0) {
        s_voice.options.max_record_ms = VOICE_DEFAULT_MAX_RECORD_MS;
    }
    if (s_voice.options.silence_stop_ms == 0) {
        s_voice.options.silence_stop_ms = VOICE_DEFAULT_SILENCE_STOP_MS;
    }
    if (s_voice.options.min_record_ms == 0) {
        s_voice.options.min_record_ms = VOICE_DEFAULT_MIN_RECORD_MS;
    }
    if (s_voice.options.silence_threshold == 0) {
        s_voice.options.silence_threshold = VOICE_DEFAULT_SILENCE_THRESHOLD;
    }
    if (s_voice.options.request_timeout_ms == 0) {
        s_voice.options.request_timeout_ms = VOICE_DEFAULT_TIMEOUT_MS;
    }
    if (s_voice.options.max_record_ms > VOICE_MAX_RECORD_MS) {
        s_voice.options.max_record_ms = VOICE_MAX_RECORD_MS;
    }

    /* 16-bit mono. Prefer PSRAM so a long utterance does not eat the internal
     * heap the Wi-Fi and TLS stacks need. */
    size_t samples = (size_t)s_voice.options.max_record_ms * voice_samples_per_ms();
    s_voice.record_buffer = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_voice.record_buffer == NULL) {
        s_voice.record_buffer = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_8BIT);
    }    if (s_voice.record_buffer == NULL) {
        ESP_LOGW(TAG, "no room for a %u-byte recording buffer", (unsigned)(samples * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }
    s_voice.record_capacity_samples = samples;

    s_voice.trigger = xSemaphoreCreateBinary();
    s_voice.idle = xSemaphoreCreateBinary();
    if (s_voice.trigger == NULL || s_voice.idle == NULL) {
        heap_caps_free(s_voice.record_buffer);
        s_voice.record_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Starts idle, so the first caller is not blocked by a stale token. */
    xSemaphoreGive(s_voice.idle);

    snprintf(s_voice.last_result, sizeof(s_voice.last_result), "尚未开始对话");
    ESP_LOGI(TAG, "voice channel ready: up to %u ms per utterance, %u-byte buffer",
             (unsigned)s_voice.options.max_record_ms, (unsigned)(samples * sizeof(int16_t)));
    return ESP_OK;
}

esp_err_t robot_voice_start(void)
{
    if (s_voice.record_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.running) {
        return ESP_OK;
    }
    s_voice.running = true;
    /* Same reasoning as the console channel: an HTTPS handshake needs far more
     * stack than a plain socket, and an undersized stack has already bitten
     * this project once. */
    if (xTaskCreate(voice_task, "robot_voice", 10240, NULL, 4, &s_voice.task) != pdPASS) {
        s_voice.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void robot_voice_stop(void)
{
    s_voice.running = false;
    s_voice.recording = false;
    s_voice.stop_requested = true;
}

void robot_voice_deinit(void)
{
    robot_voice_stop();
    /*
     * The task deletes itself once it notices running == false, and it still
     * touches these semaphores on the way out. Deleting them here would race
     * with that, so they are only released once the task is gone.
     */
    if (s_voice.task != NULL) {
        return;
    }
    if (s_voice.trigger != NULL) {
        vSemaphoreDelete(s_voice.trigger);
        s_voice.trigger = NULL;
    }
    if (s_voice.idle != NULL) {
        vSemaphoreDelete(s_voice.idle);
        s_voice.idle = NULL;
    }
    heap_caps_free(s_voice.record_buffer);
    s_voice.record_buffer = NULL;
    s_voice.record_capacity_samples = 0;
}

void robot_voice_set_state_callback(robot_voice_state_fn callback, void *ctx)
{
    s_voice.state_callback = callback;
    s_voice.state_ctx = ctx;
}

esp_err_t robot_voice_trigger(void)
{
    if (s_voice.record_buffer == NULL || !s_voice.running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_voice.busy = true;
    xSemaphoreTake(s_voice.idle, 0);
    xSemaphoreGive(s_voice.trigger);
    return ESP_OK;
}

void robot_voice_interrupt_listen(void)
{
    if (!s_voice.busy) {
        return;
    }
    /*
     * The same stop/cancel pair the button uses, plus a request to keep the
     * conversation going: the phrase that cut the reply short is the beginning
     * of the next question.
     */
    s_voice.stop_requested = true;
    s_voice.cancel_requested = true;
    s_voice.restart_requested = true;
}

void robot_voice_cancel(void)
{
    if (!s_voice.busy) {
        return;
    }
    /*
     * Both flags on purpose: stop_requested ends the recording loop while the
     * robot is still listening, and cancel_requested cuts the reply stream
     * short.  Without the second one a press only flushes what is already
     * queued, so a long answer "skips a sentence" and keeps talking - which is
     * why one press was never enough.
     */
    s_voice.stop_requested = true;
    s_voice.cancel_requested = true;
}

bool robot_voice_busy(void)
{
    return s_voice.busy;
}

bool robot_voice_ready(void)
{
    return s_voice.record_buffer != NULL && s_voice.running;
}

const char *robot_voice_last_result(void)
{
    return s_voice.last_result;
}
