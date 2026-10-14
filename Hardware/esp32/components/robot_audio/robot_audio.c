#include "robot_audio.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AUDIO_FRAME_SAMPLES 640
#define PLAYBACK_QUEUE_DEPTH 256
#define FILE_PLAYBACK_QUEUE_TIMEOUT_MS 1000
#define STREAM_PLAYBACK_QUEUE_TIMEOUT_MS 500
/*
 * Decoded speech is 16 kHz mono S16_LE. Use an adaptive first-play gate:
 * when the stream opens with an initial burst, 400 ms is enough to start;
 * on a slower connection, wait for 600 ms instead. This keeps the fast path
 * close to the 7911 response time without bringing back word-by-word stalls.
 * Count decoded PCM rather than WebSocket messages because chunk sizes vary.
 *
 * A short utterance starts as soon as its DRAIN marker is queued. The timeout
 * prevents a malformed/incomplete stream from waiting forever.
 */
#define PLAYBACK_FAST_PREBUFFER_SAMPLES 6400
#define PLAYBACK_SAFE_PREBUFFER_SAMPLES 9600
#define PLAYBACK_FAST_ARRIVAL_WINDOW_MS 250
#define PLAYBACK_PREBUFFER_TIMEOUT_MS 750
#define PLAYBACK_DEFAULT_VOLUME 70
/*
 * MAX98357A GAIN is fixed by the board wiring, so provide a modest digital
 * boost. 1.414x is approximately +3 dB; samples that would exceed the I2S
 * 32-bit range are saturated instead of wrapping and producing loud noise.
 */
#define PLAYBACK_MAX_GAIN_Q16 92682U
#define I2S_CAPTURE_TIMEOUT_MS 100
#define I2S_PLAYBACK_TIMEOUT_MS 200
#define CAPTURE_EXIT_BIT BIT0
#define PLAYBACK_EXIT_BIT BIT1

/* Microphone taps besides the primary consumer. One is enough today (the
 * cloud voice recorder); the array keeps room for a second without a redesign. */
#define AUDIO_PCM_TAP_SLOTS 2

typedef enum {
    PLAYBACK_PCM,
    PLAYBACK_DRAIN,
    PLAYBACK_STOP,
} playback_message_type_t;

typedef struct {
    playback_message_type_t type;
    uint32_t generation;
    size_t sample_count;
    int16_t samples[];
} playback_message_t;


typedef struct {
    robot_audio_config_t config;
    i2s_chan_handle_t rx;
    i2s_chan_handle_t tx;
    QueueHandle_t playback_queue;
    EventGroupHandle_t events;
    TaskHandle_t capture_task;
    TaskHandle_t playback_task;
    /*
     * Keep the 3.75 KiB capture workspace out of the 4 KiB FreeRTOS task
     * stack. Putting these arrays in capture_task() overflowed that stack
     * during boot and corrupted the adjacent playback queue.
     */
    int32_t capture_raw[AUDIO_FRAME_SAMPLES];
    int16_t capture_pcm[AUDIO_FRAME_SAMPLES];
    int32_t playback_output[320];
    robot_pcm_consumer_fn consumer;
    void *consumer_ctx;
    robot_pcm_consumer_fn taps[AUDIO_PCM_TAP_SLOTS];
    void *tap_ctxs[AUDIO_PCM_TAP_SLOTS];
    volatile bool running;
    volatile bool capture_paused;
    volatile bool playing;
    size_t queued_samples;
    bool drain_queued;
    uint32_t playback_generation;
    uint8_t volume_percent;
    uint32_t volume_gain_q16;
    bool initialized;
} robot_audio_state_t;

static const char *TAG = "robot_audio";
static robot_audio_state_t s_audio;
static portMUX_TYPE s_playback_lock = portMUX_INITIALIZER_UNLOCKED;


static uint32_t volume_to_gain_q16(uint8_t volume)
{
    /*
     * Match XiaoZhi's perceptual squared curve while allowing the top of the
     * range to reach the board-specific +3 dB software boost.
     */
    return (uint32_t)(((uint64_t)volume * volume *
                       PLAYBACK_MAX_GAIN_Q16) / 10000ULL);
}

static void playback_buffer_state(size_t *queued_samples, bool *drain_queued)
{
    portENTER_CRITICAL(&s_playback_lock);
    if (queued_samples) {
        *queued_samples = s_audio.queued_samples;
    }
    if (drain_queued) {
        *drain_queued = s_audio.drain_queued;
    }
    portEXIT_CRITICAL(&s_playback_lock);
}

static void capture_task(void *arg)
{
    (void)arg;
    while (s_audio.running) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_audio.rx, s_audio.capture_raw,
                                         sizeof(s_audio.capture_raw),
                                         &bytes_read, I2S_CAPTURE_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "microphone I2S read failed: %s (0x%x), read=%u",
                     esp_err_to_name(err), (unsigned int)err,
                     (unsigned int)bytes_read);
            continue;
        }
        size_t sample_count = bytes_read / sizeof(s_audio.capture_raw[0]);
        /*
         * The I2S peripheral hands over one 32-bit slot per sample while the
         * microphone only drives the top bits of it, so the raw value is
         * shifted right before it is clamped into 16 bits.  That shift is the
         * capture gain, and it is the one number that decides whether the
         * captured audio is usable: too small and ordinary speech lands past
         * full scale, where the clamp below flattens it into a square wave that
         * no recogniser can read, while the reported level still looks healthy.
         * See CONFIG_ROBOT_MIC_SHIFT for the value and how to pick it.
         */
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t sample =
                s_audio.capture_raw[i] >> CONFIG_ROBOT_MIC_SHIFT;
            if (sample > INT16_MAX) {
                sample = INT16_MAX;
            } else if (sample < INT16_MIN) {
                sample = INT16_MIN;
            }
            s_audio.capture_pcm[i] = (int16_t)sample;
        }
        robot_pcm_consumer_fn consumer = s_audio.consumer;
        if (!s_audio.capture_paused && consumer) {
            consumer(s_audio.consumer_ctx, s_audio.capture_pcm, sample_count,
                     (uint64_t)(esp_timer_get_time() / 1000));
        }
        /* Taps run after the primary consumer so the wake word engine keeps
         * its place at the head of the frame. */
        for (size_t tap = 0; tap < AUDIO_PCM_TAP_SLOTS; ++tap) {
            robot_pcm_consumer_fn fn = s_audio.taps[tap];
            if (!s_audio.capture_paused && fn) {
                fn(s_audio.tap_ctxs[tap], s_audio.capture_pcm, sample_count,
                   (uint64_t)(esp_timer_get_time() / 1000));
            }
        }
    }
    xEventGroupSetBits(s_audio.events, CAPTURE_EXIT_BIT);
    vTaskDelete(NULL);
}

static void write_pcm(const int16_t *samples, size_t sample_count,
                      uint32_t generation)
{
    while (sample_count && s_audio.running) {
        uint32_t current_generation;
        portENTER_CRITICAL(&s_playback_lock);
        current_generation = s_audio.playback_generation;
        portEXIT_CRITICAL(&s_playback_lock);
        if (generation != current_generation) {
            break;
        }
        size_t batch = sample_count > 320 ? 320 : sample_count;
        uint32_t volume_gain_q16;
        portENTER_CRITICAL(&s_playback_lock);
        volume_gain_q16 = s_audio.volume_gain_q16;
        portEXIT_CRITICAL(&s_playback_lock);
        for (size_t i = 0; i < batch; ++i) {
            int64_t amplified =
                (int64_t)samples[i] * volume_gain_q16;
            if (amplified > INT32_MAX) {
                amplified = INT32_MAX;
            } else if (amplified < INT32_MIN) {
                amplified = INT32_MIN;
            }
            s_audio.playback_output[i] = (int32_t)amplified;
        }
        size_t bytes_written = 0;
        const size_t bytes_to_write =
            batch * sizeof(s_audio.playback_output[0]);
        esp_err_t err = i2s_channel_write(s_audio.tx, s_audio.playback_output,
                                          bytes_to_write, &bytes_written,
                                          I2S_PLAYBACK_TIMEOUT_MS);
        if (err != ESP_OK || bytes_written != bytes_to_write) {
            ESP_LOGW(TAG,
                     "speaker I2S write failed: %s (0x%x), requested=%u written=%u",
                     esp_err_to_name(err), (unsigned int)err,
                     (unsigned int)bytes_to_write,
                     (unsigned int)bytes_written);
            break;
        }
        samples += batch;
        sample_count -= batch;
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    while (s_audio.running) {
        playback_message_t *message = NULL;
        if (xQueueReceive(s_audio.playback_queue, &message, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!message) {
            continue;
        }
        if (message->type == PLAYBACK_STOP) {
            free(message);
            break;
        }
        uint32_t current_generation;
        portENTER_CRITICAL(&s_playback_lock);
        current_generation = s_audio.playback_generation;
        portEXIT_CRITICAL(&s_playback_lock);
        if (message->generation != current_generation) {
            free(message);
            continue;
        }
        if (message->type == PLAYBACK_PCM) {
            portENTER_CRITICAL(&s_playback_lock);
            if (s_audio.queued_samples >= message->sample_count) {
                s_audio.queued_samples -= message->sample_count;
            } else {
                s_audio.queued_samples = 0;
            }
            portEXIT_CRITICAL(&s_playback_lock);
            if (!s_audio.playing) {
                /* Start once with enough real audio to absorb network jitter. */
                TickType_t started = xTaskGetTickCount();
                while (s_audio.running) {
                    portENTER_CRITICAL(&s_playback_lock);
                    current_generation = s_audio.playback_generation;
                    portEXIT_CRITICAL(&s_playback_lock);
                    if (message->generation != current_generation) {
                        break;
                    }
                    size_t queued_samples = 0;
                    bool drain_queued = false;
                    playback_buffer_state(&queued_samples, &drain_queued);
                    const size_t buffered_samples =
                        message->sample_count + queued_samples;
                    const TickType_t elapsed_ticks =
                        xTaskGetTickCount() - started;
                    const bool fast_initial_burst =
                        buffered_samples >=
                            PLAYBACK_FAST_PREBUFFER_SAMPLES &&
                        elapsed_ticks <= pdMS_TO_TICKS(
                            PLAYBACK_FAST_ARRIVAL_WINDOW_MS);
                    if (buffered_samples >=
                            PLAYBACK_SAFE_PREBUFFER_SAMPLES ||
                        fast_initial_burst ||
                        drain_queued ||
                        elapsed_ticks >=
                            pdMS_TO_TICKS(PLAYBACK_PREBUFFER_TIMEOUT_MS)) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (message->generation != current_generation) {
                    free(message);
                    continue;
                }
                size_t queued_samples = 0;
                playback_buffer_state(&queued_samples, NULL);
                ESP_LOGI(TAG,
                         "playback stream start: buffered=%u ms waited=%u ms",
                         (unsigned)(((message->sample_count + queued_samples) *
                                     1000ULL) / 16000ULL),
                         (unsigned)((xTaskGetTickCount() - started) *
                                    portTICK_PERIOD_MS));
                s_audio.playing = true;
                if (s_audio.config.playback_started) {
                    s_audio.config.playback_started(
                        s_audio.config.playback_started_ctx,
                        message->generation);
                }
            }
            s_audio.playing = true;
            write_pcm(message->samples, message->sample_count,
                      message->generation);
            /*
             * Keep the stream open while waiting for the next chunk.
             * Re-entering prebuffer after every transient gap caused the
             * several-second pauses heard between otherwise valid chunks.
             */
        } else if (message->type == PLAYBACK_DRAIN) {
            portENTER_CRITICAL(&s_playback_lock);
            s_audio.drain_queued = false;
            portEXIT_CRITICAL(&s_playback_lock);
            s_audio.playing = false;
            if (s_audio.config.playback_drained) {
                s_audio.config.playback_drained(
                    s_audio.config.playback_drained_ctx,
                    message->generation);
            }
        }
        free(message);
    }
    xEventGroupSetBits(s_audio.events, PLAYBACK_EXIT_BIT);
    vTaskDelete(NULL);
}

static esp_err_t queue_message(playback_message_type_t type,
                               const int16_t *samples,
                               size_t sample_count,
                               TickType_t wait_ticks)
{
    if (!s_audio.running || !s_audio.playback_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count >
            (SIZE_MAX - sizeof(playback_message_t)) / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t size = sizeof(playback_message_t) +
                  sample_count * sizeof(int16_t);
    playback_message_t *message = NULL;
    if (type == PLAYBACK_PCM && sample_count > 0) {
        /*
         * Decoded streaming TTS can queue tens of kilobytes of small PCM
         * blocks. Keep those blocks in PSRAM so TLS/WebSocket callbacks
         * retain enough internal RAM to receive the final TTS frames.
         */
        message = heap_caps_malloc(size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!message) {
        message = malloc(size);
    }
    if (!message) {
        return ESP_ERR_NO_MEM;
    }
    message->type = type;
    message->sample_count = sample_count;
    if (sample_count) {
        if (!samples) {
            free(message);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(message->samples, samples,
               sample_count * sizeof(int16_t));
    }
    portENTER_CRITICAL(&s_playback_lock);
    message->generation = s_audio.playback_generation;
    if (type == PLAYBACK_PCM) {
        s_audio.queued_samples += sample_count;
    } else if (type == PLAYBACK_DRAIN) {
        s_audio.drain_queued = true;
    }
    portEXIT_CRITICAL(&s_playback_lock);
    if (xQueueSend(s_audio.playback_queue, &message, wait_ticks) != pdTRUE) {
        portENTER_CRITICAL(&s_playback_lock);
        if (type == PLAYBACK_PCM) {
            if (s_audio.queued_samples >= sample_count) {
                s_audio.queued_samples -= sample_count;
            } else {
                s_audio.queued_samples = 0;
            }
        } else if (type == PLAYBACK_DRAIN) {
            s_audio.drain_queued = false;
        }
        portEXIT_CRITICAL(&s_playback_lock);
        ESP_LOGW(TAG, "playback queue full: type=%d samples=%u queued=%u",
                 (int)type, (unsigned)sample_count,
                 (unsigned)uxQueueMessagesWaiting(s_audio.playback_queue));
        free(message);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t robot_audio_init(const robot_audio_config_t *config)
{
    esp_err_t ret = ESP_OK;
    /*
     * sample_rate_hz configures the CAPTURE channel only.  Playback keeps a
     * fixed rate because the server synthesises speech at 16 kHz and both ends
     * have to agree; capture and playback are separate I2S controllers here, so
     * one can move without the other.
     */
    if (!config || s_audio.initialized ||
        config->sample_rate_hz < 8000 || config->sample_rate_hz > 48000) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.config = *config;
    s_audio.volume_percent = PLAYBACK_DEFAULT_VOLUME;
    s_audio.volume_gain_q16 =
        volume_to_gain_q16(PLAYBACK_DEFAULT_VOLUME);

    esp_vfs_spiffs_conf_t spiffs = {
        .base_path = "/assets",
        .partition_label = "assets",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&spiffs), TAG, "mount assets");

    i2s_chan_config_t rx_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_channel.dma_desc_num = 6;
    rx_channel.dma_frame_num = AUDIO_FRAME_SAMPLES;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&rx_channel, NULL, &s_audio.rx), fail, TAG, "new rx");

    i2s_std_config_t rx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->mic_bclk_gpio,
            .ws = config->mic_ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = config->mic_data_gpio,
            .invert_flags = {0},
        },
    };
    /*
     * Which half of the I2S frame the microphone drives.  It follows the L/R
     * strap on the microphone itself, and reading the wrong half gives silence
     * rather than an error, so it is worth being able to flip.
     */
#if CONFIG_ROBOT_MIC_RIGHT_CHANNEL
    rx_config.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    rx_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.rx, &rx_config), fail, TAG, "init rx");
    /*
     * Log the whole capture configuration.  Every one of these three can be
     * changed from menuconfig and every one of them changes what the audio
     * sounds like, so a level reading is only interpretable next to the build
     * it came from.
     */
    ESP_LOGI(TAG, "microphone capture: %u Hz, %s channel, raw>>%d",
             (unsigned)config->sample_rate_hz,
#if CONFIG_ROBOT_MIC_RIGHT_CHANNEL
             "right",
#else
             "left",
#endif
             CONFIG_ROBOT_MIC_SHIFT);

    i2s_chan_config_t tx_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_channel.dma_desc_num = 6;
    tx_channel.dma_frame_num = 320;
    tx_channel.auto_clear_after_cb = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&tx_channel, &s_audio.tx, NULL), fail, TAG, "new tx");

    i2s_std_config_t tx_config = {
        /* Playback rate is fixed: the server synthesises speech at 16 kHz and
         * the wake-up assets are 16 kHz too.  Only capture follows the config. */
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->speaker_bclk_gpio,
            .ws = config->speaker_ws_gpio,
            .dout = config->speaker_data_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    tx_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.tx, &tx_config), fail, TAG, "init tx");

    s_audio.playback_queue =
        xQueueCreate(PLAYBACK_QUEUE_DEPTH, sizeof(playback_message_t *));
    s_audio.events = xEventGroupCreate();
    if (!s_audio.playback_queue || !s_audio.events) {
        goto fail;
    }
    s_audio.initialized = true;
    return ESP_OK;

fail:
    robot_audio_deinit();
    return ret == ESP_OK ? ESP_FAIL : ret;
}

esp_err_t robot_audio_start(void)
{
    if (!s_audio.initialized || s_audio.running) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.rx), TAG, "enable rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.tx), TAG, "enable tx");
    s_audio.running = true;
    xEventGroupClearBits(s_audio.events, CAPTURE_EXIT_BIT | PLAYBACK_EXIT_BIT);
    if (xTaskCreate(capture_task, "audio_capture", 4096, NULL, 7,
                    &s_audio.capture_task) != pdPASS ||
        xTaskCreate(playback_task, "audio_playback", 4096, NULL, 6,
                    &s_audio.playback_task) != pdPASS) {
        robot_audio_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void robot_audio_stop(void)
{
    if (!s_audio.running) {
        return;
    }
    playback_message_t *stop = calloc(1, sizeof(*stop));
    if (stop) {
        stop->type = PLAYBACK_STOP;
        xQueueSend(s_audio.playback_queue, &stop, 0);
    }
    s_audio.running = false;
    i2s_channel_disable(s_audio.rx);
    i2s_channel_disable(s_audio.tx);
    xEventGroupWaitBits(s_audio.events,
                        CAPTURE_EXIT_BIT | PLAYBACK_EXIT_BIT,
                        pdFALSE, pdTRUE, pdMS_TO_TICKS(1500));
    s_audio.playing = false;
    portENTER_CRITICAL(&s_playback_lock);
    s_audio.queued_samples = 0;
    s_audio.drain_queued = false;
    portEXIT_CRITICAL(&s_playback_lock);
}

void robot_audio_deinit(void)
{
    robot_audio_stop();
    if (s_audio.rx) {
        i2s_del_channel(s_audio.rx);
    }
    if (s_audio.tx) {
        i2s_del_channel(s_audio.tx);
    }
    if (s_audio.playback_queue) {
        playback_message_t *message = NULL;
        while (xQueueReceive(s_audio.playback_queue, &message, 0) == pdTRUE) {
            free(message);
        }
        vQueueDelete(s_audio.playback_queue);
    }
    if (s_audio.events) {
        vEventGroupDelete(s_audio.events);
    }
    if (s_audio.initialized) {
        esp_vfs_spiffs_unregister("assets");
    }
    memset(&s_audio, 0, sizeof(s_audio));
}

esp_err_t robot_audio_register_pcm_consumer(robot_pcm_consumer_fn consumer, void *ctx)
{
    if (!s_audio.initialized || !consumer) {
        return ESP_ERR_INVALID_ARG;
    }
    s_audio.consumer_ctx = ctx;
    s_audio.consumer = consumer;
    return ESP_OK;
}

esp_err_t robot_audio_add_pcm_tap(robot_pcm_consumer_fn tap, void *ctx)
{
    if (!s_audio.initialized || !tap) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t slot = 0; slot < AUDIO_PCM_TAP_SLOTS; ++slot) {
        if (s_audio.taps[slot] == tap) {
            s_audio.tap_ctxs[slot] = ctx;
            return ESP_OK;
        }
    }
    for (size_t slot = 0; slot < AUDIO_PCM_TAP_SLOTS; ++slot) {
        if (s_audio.taps[slot] == NULL) {
            s_audio.tap_ctxs[slot] = ctx;
            s_audio.taps[slot] = tap;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t robot_audio_remove_pcm_tap(robot_pcm_consumer_fn tap)
{
    if (!tap) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t slot = 0; slot < AUDIO_PCM_TAP_SLOTS; ++slot) {
        if (s_audio.taps[slot] == tap) {
            s_audio.taps[slot] = NULL;
            s_audio.tap_ctxs[slot] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t robot_audio_set_volume(uint8_t volume)
{
    if (!s_audio.initialized || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_playback_lock);
    s_audio.volume_percent = volume;
    s_audio.volume_gain_q16 = volume_to_gain_q16(volume);
    portEXIT_CRITICAL(&s_playback_lock);
    ESP_LOGI(TAG, "speaker volume=%u gain=%u.%02ux",
             (unsigned)volume,
             (unsigned)(s_audio.volume_gain_q16 / 65536U),
             (unsigned)(((s_audio.volume_gain_q16 % 65536U) * 100U) /
                        65536U));
    return ESP_OK;
}

uint8_t robot_audio_get_volume(void)
{
    uint8_t volume;
    portENTER_CRITICAL(&s_playback_lock);
    volume = s_audio.volume_percent;
    portEXIT_CRITICAL(&s_playback_lock);
    return volume;
}

esp_err_t robot_audio_play_pcm(const int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return queue_message(PLAYBACK_PCM, samples, sample_count,
                         pdMS_TO_TICKS(STREAM_PLAYBACK_QUEUE_TIMEOUT_MS));
}

esp_err_t robot_audio_finish_playback(void)
{
    return queue_message(PLAYBACK_DRAIN, NULL, 0,
                         pdMS_TO_TICKS(STREAM_PLAYBACK_QUEUE_TIMEOUT_MS));
}

uint32_t robot_audio_start_playback_stream(void)
{
    robot_audio_abort_playback();
    return robot_audio_get_playback_generation();
}

uint32_t robot_audio_get_playback_generation(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_playback_lock);
    generation = s_audio.playback_generation;
    portEXIT_CRITICAL(&s_playback_lock);
    return generation;
}

void robot_audio_abort_playback(void)
{
    if (!s_audio.playback_queue) {
        return;
    }
    portENTER_CRITICAL(&s_playback_lock);
    ++s_audio.playback_generation;
    s_audio.queued_samples = 0;
    s_audio.drain_queued = false;
    s_audio.playing = false;
    portEXIT_CRITICAL(&s_playback_lock);

    playback_message_t *message = NULL;
    while (xQueueReceive(s_audio.playback_queue, &message, 0) == pdTRUE) {
        free(message);
        message = NULL;
    }
}


esp_err_t robot_audio_play_wav_file(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[44];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0 ||
        memcmp(header + 12, "fmt ", 4) != 0 ||
        memcmp(header + 36, "data", 4) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint16_t format =
        (uint16_t)header[20] | ((uint16_t)header[21] << 8);
    const uint16_t channels =
        (uint16_t)header[22] | ((uint16_t)header[23] << 8);
    const uint32_t sample_rate =
        (uint32_t)header[24] | ((uint32_t)header[25] << 8) |
        ((uint32_t)header[26] << 16) | ((uint32_t)header[27] << 24);
    const uint16_t bits_per_sample =
        (uint16_t)header[34] | ((uint16_t)header[35] << 8);
    uint32_t remaining =
        (uint32_t)header[40] | ((uint32_t)header[41] << 8) |
        ((uint32_t)header[42] << 16) | ((uint32_t)header[43] << 24);
    if (format != 1 || channels != 1 || sample_rate != 16000 ||
        bits_per_sample != 16 || (remaining & 1U) != 0) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int16_t *samples = malloc(960 * sizeof(*samples));
    if (!samples) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        size_t bytes = remaining < 960 * sizeof(*samples)
                           ? remaining
                           : 960 * sizeof(*samples);
        if (fread(samples, 1, bytes, file) != bytes) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        result = queue_message(
            PLAYBACK_PCM, samples, bytes / sizeof(*samples),
            pdMS_TO_TICKS(FILE_PLAYBACK_QUEUE_TIMEOUT_MS));
        if (result != ESP_OK) {
            break;
        }
        remaining -= (uint32_t)bytes;
    }
    free(samples);
    fclose(file);
    if (result == ESP_OK) {
        result = queue_message(PLAYBACK_DRAIN, NULL, 0,
                               pdMS_TO_TICKS(FILE_PLAYBACK_QUEUE_TIMEOUT_MS));
    }
    return result;
}

esp_err_t robot_audio_play_wakeup_tone(void)
{
    /* 7911 voice-wakeup acknowledgement (WakeupAck.mp3). */
    return robot_audio_play_wav_file("/assets/wakeup_ack.wav");
}

void robot_audio_set_capture_paused(bool paused)
{
    s_audio.capture_paused = paused;
}

bool robot_audio_is_playing(void)
{
    return s_audio.playing;
}

size_t robot_audio_playback_buffered_samples(void)
{
    size_t queued_samples = 0;
    playback_buffer_state(&queued_samples, NULL);
    return queued_samples;
}
