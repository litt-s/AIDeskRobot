#include "robot_app.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_vad.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "robot_audio.h"
#include "robot_cloud.h"
#if CONFIG_ROBOT_CLOUD_VOICE
#include "robot_voice.h"
#endif
#include "robot_motion.h"
#include "robot_network.h"
#include "robot_ui.h"
#include "robot_wakeup.h"
#include "sdkconfig.h"

namespace {

constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr gpio_num_t kVolumeUpPin = GPIO_NUM_40;
constexpr gpio_num_t kVolumeDownPin = GPIO_NUM_39;
constexpr uint32_t kShortPressMinMs = 50;
constexpr uint32_t kShortPressMaxMs = 1500;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kVolumeLongPressMs = 800;
/*
 * BOOT long press opens the setup page even while the robot is online.  The
 * page is the only place to change the Wi-Fi or the device code, and before
 * this the only ways in were to catch the connect window (a few seconds after
 * power-on) or to break the saved Wi-Fi on purpose.
 */
constexpr uint32_t kProvisionLongPressMs = 800;
constexpr int kVolumeStep = 10;
constexpr uint8_t kDefaultVolume = 80;
constexpr int kWakeSourceVoice = 0;
constexpr int kWakeSourceBoot = 1;
/*
 * The I2S callback delivers 640 samples (40 ms), but AIUI RTOS v3 accepts at
 * most 1024 payload bytes per audio frame. Match the 7911 reference flow:
 * split each capture block into two 320-sample / 640-byte / 20-ms frames.
 */
constexpr size_t kAudioFrameSamples = 320;
constexpr size_t kVadFrameSamples = 320;
constexpr uint32_t kVadFrameMs = 20;
constexpr uint32_t kVadSpeechStartMs = 520;
/* 7911 demo: speech >25 frames, then silence >20 frames (20 ms/frame). */
constexpr uint32_t kVadSpeechEndMs = 420;
constexpr int32_t kAiuiCaptureGain = 1;
/*
 * A drained local playback queue does not mean the cloud stream has ended:
 * AIUI may deliver the final status=2 packet several seconds after the
 * preceding audio packet. Only recover the transport after a prolonged period
 * with no TTS data at all.
 */
constexpr uint32_t kTtsChunkStallTimeoutMs = 8000;
constexpr uint8_t kAiuiMaxConnectRetries = 3;
constexpr uint32_t kAiuiConnectRetryBaseMs = 1000;
constexpr uint32_t kNoSpeechResultTimeoutMs = 5000;
constexpr size_t kEventQueueDepth = 20;
/* Preserve roughly 400 ms of buffering after switching to 20-ms frames. */
constexpr size_t kAudioQueueDepth = 20;
constexpr uint32_t kAiuiAudioTaskStackBytes = 8 * 1024;
constexpr uint32_t kBootTaskStackBytes = 4096;
constexpr uint32_t kProvisioningPromptStackBytes = 16 * 1024;
constexpr uint32_t kReadyPromptStackBytes = 16 * 1024;
constexpr uint32_t kMediaPlayerTaskStackBytes = 12 * 1024;
constexpr uint32_t kMediaSearchTaskStackBytes = 12 * 1024;
constexpr uint32_t kMediaResolveTaskStackBytes = 12 * 1024;
constexpr uint32_t kMediaStartFallbackMs = 600;
/* Keep reply gestures calm: the first one is immediate, later ones are
 * separated by a human-like 6-8 second pause. */
constexpr uint32_t kReplyPresentationMinIntervalMs = 6000;
constexpr uint32_t kReplyPresentationJitterMs = 2000;
constexpr uint32_t kListeningAnimationMinIntervalMs = 6500;
constexpr uint32_t kListeningAnimationJitterMs = 5500;
constexpr uint32_t kPresentationBusyRetryMs = 1500;
constexpr size_t kMaxPendingRandomPresentations = 4;
const char *TAG = "robot_app";
std::atomic<bool> s_provisioning_prompt_running{false};
std::atomic<bool> s_ready_prompt_running{false};

enum class ButtonEvent {
    NONE,
    SHORT_PRESS,
    LONG_PRESS,
};

struct ButtonTracker {
    gpio_num_t pin;
    bool raw_pressed;
    bool stable_pressed;
    bool long_press_sent;
    uint64_t raw_changed_ms;
    uint64_t pressed_at_ms;
};


const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring &&
                   item->valuestring[0] != '\0'
               ? item->valuestring
               : nullptr;
}

const cJSON *json_find_key(const cJSON *node, const char *key,
                           unsigned depth)
{
    if (!node || !key || depth == 0) {
        return nullptr;
    }
    if (cJSON_IsObject(node)) {
        const cJSON *direct =
            cJSON_GetObjectItemCaseSensitive(node, key);
        if (direct) {
            return direct;
        }
    }
    const cJSON *child = nullptr;
    cJSON_ArrayForEach(child, node) {
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            const cJSON *found = json_find_key(child, key, depth - 1);
            if (found) {
                return found;
            }
        }
    }
    return nullptr;
}

bool json_bool(const cJSON *item, bool *value)
{
    if (!item || !value) {
        return false;
    }
    if (cJSON_IsBool(item)) {
        *value = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcmp(item->valuestring, "true") == 0) {
            *value = true;
            return true;
        }
        if (strcmp(item->valuestring, "false") == 0) {
            *value = false;
            return true;
        }
    }
    return false;
}




const char *media_url_from_item(const cJSON *item)
{
    static const char *const keys[] = {
        "uni_url", "url", "playUrl", "play_url", "audioUrl", "audio_url",
    };
    for (const char *key : keys) {
        const char *value = json_string(item, key);
        if (value &&
            (strncmp(value, "http://", 7) == 0 ||
             strncmp(value, "https://", 8) == 0)) {
            return value;
        }
    }
    return nullptr;
}

const char *media_title_from_item(const cJSON *item)
{
    static const char *const keys[] = {
        "name", "itemname", "neatsongname", "title", "songname",
        "album",
    };
    for (const char *key : keys) {
        const char *value = json_string(item, key);
        if (value) {
            return value;
        }
    }
    return "";
}





/*
 * True while a cloud voice turn owns the microphone or the speaker.  Always
 * false on builds without the cloud voice channel, which keeps the call sites
 * free of conditionals.
 */
bool cloud_voice_turn_busy()
{
#if CONFIG_ROBOT_CLOUD_VOICE
    return robot_voice_busy();
#else
    return false;
#endif
}

/* Ends a cloud voice turn that is in progress.  Returns true when there was one
 * to cancel, so the caller can stop handling the press itself. */
bool cancel_cloud_voice_turn()
{
    if (!cloud_voice_turn_busy()) {
        return false;
    }
#if CONFIG_ROBOT_CLOUD_VOICE
    robot_voice_cancel();
#endif
    return true;
}

ButtonEvent poll_button(ButtonTracker &button, uint64_t now_ms,
                        uint32_t long_press_ms)
{
    const bool raw_pressed = gpio_get_level(button.pin) == 0;
    if (raw_pressed != button.raw_pressed) {
        button.raw_pressed = raw_pressed;
        button.raw_changed_ms = now_ms;
    }
    if (button.raw_pressed != button.stable_pressed &&
        now_ms - button.raw_changed_ms >= kButtonDebounceMs) {
        button.stable_pressed = button.raw_pressed;
        if (button.stable_pressed) {
            button.pressed_at_ms = now_ms;
            button.long_press_sent = false;
        } else {
            const uint64_t duration = now_ms - button.pressed_at_ms;
            if (!button.long_press_sent &&
                duration >= kShortPressMinMs &&
                duration <= kShortPressMaxMs) {
                return ButtonEvent::SHORT_PRESS;
            }
        }
    }
    if (button.stable_pressed && !button.long_press_sent &&
        long_press_ms != 0 &&
        now_ms - button.pressed_at_ms >= long_press_ms) {
        button.long_press_sent = true;
        return ButtonEvent::LONG_PRESS;
    }
    return ButtonEvent::NONE;
}

uint8_t load_saved_volume()
{
    uint8_t volume = kDefaultVolume;
    nvs_handle_t nvs;
    if (nvs_open("robot_audio", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t saved = 0;
        if (nvs_get_u8(nvs, "volume", &saved) == ESP_OK && saved <= 100) {
            volume = saved;
        }
        nvs_close(nvs);
    }
    return volume;
}

void save_volume(uint8_t volume)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("robot_audio", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "volume", volume);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to save volume: %s", esp_err_to_name(err));
    }
}

void set_and_save_volume(int requested)
{
    const uint8_t volume = static_cast<uint8_t>(
        std::max(0, std::min(100, requested)));
    if (robot_audio_set_volume(volume) == ESP_OK) {
        save_volume(volume);
        ESP_LOGI(TAG, "volume changed to %u", (unsigned)volume);
    }
}

void provisioning_prompt_task(void *arg)
{
    (void)arg;
    /* Pre-rendered XiaoFei prompt: keep provisioning and ready prompts on the
       same voice family. It must remain a local asset because the device has
       no usable network when provisioning starts. */
    esp_err_t err =
        robot_audio_play_wav_file("/assets/provisioning.wav");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provisioning prompt failed: %s", esp_err_to_name(err));
    }
    s_provisioning_prompt_running.store(false);
    vTaskDelete(nullptr);
}

void ready_prompt_task(void *arg)
{
    (void)arg;
    esp_err_t err = robot_audio_play_wav_file("/assets/network_ready.wav");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ready prompt failed: %s", esp_err_to_name(err));
        robot_audio_set_capture_paused(false);
    }
    s_ready_prompt_running.store(false);
    vTaskDelete(nullptr);
}



bool text_has_any(const char *text,
                  std::initializer_list<const char *> keywords)
{
    if (!text) {
        return false;
    }
    for (const char *keyword : keywords) {
        if (strstr(text, keyword)) {
            return true;
        }
    }
    return false;
}

robot_expression_t expression_from_text(const char *text)
{
    if (!text) {
        return ROBOT_EXPRESSION_NEUTRAL;
    }
    if (text_has_any(text, {"大笑", "笑死", "哈哈", "laughing", "laugh"})) {
        return ROBOT_EXPRESSION_LAUGHING;
    }
    if (text_has_any(text, {"爱你", "喜欢你", "爱心", "loving", "love",
                            "affectionate"})) {
        return ROBOT_EXPRESSION_LOVING;
    }
    if (text_has_any(text, {"哭", "眼泪", "流泪", "crying", "cry"})) {
        return ROBOT_EXPRESSION_CRY;
    }
    if (text_has_any(text, {"尴尬", "不好意思", "害羞", "embarrassed"})) {
        return ROBOT_EXPRESSION_EMBARRASSED;
    }
    if (text_has_any(text, {"awkward", "disgust", "窘迫", "厌恶"})) {
        return ROBOT_EXPRESSION_AWKWARD;
    }
    if (text_has_any(text, {"震惊", "吓一跳", "shocked", "fearful", "fear"})) {
        return ROBOT_EXPRESSION_SHOCKED;
    }
    if (text_has_any(text, {"困惑", "不明白", "迷惑", "confused"})) {
        return ROBOT_EXPRESSION_CONFUSED;
    }
    if (text_has_any(text, {"好吃", "美味", "香", "delicious"})) {
        return ROBOT_EXPRESSION_DELICIOUS;
    }
    if (text_has_any(text, {"亲亲", "亲吻", "么么哒", "kiss"})) {
        return ROBOT_EXPRESSION_KISSY;
    }
    if (text_has_any(text, {"酷", "帅", "cool"})) {
        return ROBOT_EXPRESSION_COOL;
    }
    if (text_has_any(text, {"自信", "当然", "没问题", "confident", "proud"})) {
        return ROBOT_EXPRESSION_CONFIDENT;
    }
    if (text_has_any(text, {"放松", "轻松", "relaxed", "calm"})) {
        return ROBOT_EXPRESSION_RELAXED;
    }
    if (text_has_any(text, {"搞笑", "逗乐", "funny"})) {
        return ROBOT_EXPRESSION_FUNNY;
    }
    if (text_has_any(text, {"调皮", "搞怪", "滑稽", "silly"})) {
        return ROBOT_EXPRESSION_SILLY;
    }
    if (text_has_any(text, {"happy", "happiness", "smile", "开心", "高兴",
                            "快乐", "好的"})) {
        return ROBOT_EXPRESSION_HAPPY;
    }
    if (text_has_any(text, {"sad", "sadness", "难过", "悲伤", "伤心", "遗憾"})) {
        return ROBOT_EXPRESSION_SAD;
    }
    if (text_has_any(text, {"anger", "angry", "生气", "愤怒", "气死"})) {
        return ROBOT_EXPRESSION_ANGRY;
    }
    if (text_has_any(text, {"surprise", "惊讶", "竟然", "哇"})) {
        return ROBOT_EXPRESSION_SURPRISED;
    }
    if (text_has_any(text, {"think", "想一想", "思考", "让我想想"})) {
        return ROBOT_EXPRESSION_THINKING;
    }
    return ROBOT_EXPRESSION_NEUTRAL;
}


robot_expression_t reply_expression_from_text(const char *text)
{
    robot_expression_t expression = expression_from_text(text);
    if (expression != ROBOT_EXPRESSION_NEUTRAL) {
        return expression;
    }
    if (text_has_any(text, {"抱歉", "对不起", "没找到", "走丢了",
                            "做不到", "无法", "很遗憾"})) {
        return ROBOT_EXPRESSION_AWKWARD;
    }
    if (text_has_any(text, {"为什么", "怎么", "可能", "也许", "分析",
                            "让我看看", "我想想", "建议"})) {
        return ROBOT_EXPRESSION_THINKING;
    }
    if (text_has_any(text, {"恭喜", "太棒", "真棒", "成功", "为你播放",
                            "交给我", "可以", "当然", "谢谢"})) {
        return ROBOT_EXPRESSION_HAPPY;
    }
    if (text_has_any(text, {"！", "!"})) {
        return ROBOT_EXPRESSION_SURPRISED;
    }
    return ROBOT_EXPRESSION_NEUTRAL;
}


const char *expression_name(robot_expression_t expression)
{
    switch (expression) {
        case ROBOT_EXPRESSION_HAPPY: return "happy";
        case ROBOT_EXPRESSION_SAD: return "sad";
        case ROBOT_EXPRESSION_ANGRY: return "angry";
        case ROBOT_EXPRESSION_SURPRISED: return "surprised";
        case ROBOT_EXPRESSION_SLEEP: return "sleep";
        case ROBOT_EXPRESSION_THINKING: return "thinking";
        case ROBOT_EXPRESSION_CONFUSED: return "confused";
        case ROBOT_EXPRESSION_AWKWARD: return "awkward";
        case ROBOT_EXPRESSION_CRY: return "cry";
        case ROBOT_EXPRESSION_LAUGHING: return "laughing";
        case ROBOT_EXPRESSION_FUNNY: return "funny";
        case ROBOT_EXPRESSION_LOVING: return "loving";
        case ROBOT_EXPRESSION_EMBARRASSED: return "embarrassed";
        case ROBOT_EXPRESSION_SHOCKED: return "shocked";
        case ROBOT_EXPRESSION_COOL: return "cool";
        case ROBOT_EXPRESSION_RELAXED: return "relaxed";
        case ROBOT_EXPRESSION_DELICIOUS: return "delicious";
        case ROBOT_EXPRESSION_KISSY: return "kissy";
        case ROBOT_EXPRESSION_CONFIDENT: return "confident";
        case ROBOT_EXPRESSION_SILLY: return "silly";
        case ROBOT_EXPRESSION_NEUTRAL:
        default: return "neutral";
    }
}


}  // namespace

RobotApp &RobotApp::Instance()
{
    static RobotApp instance;
    return instance;
}

esp_err_t RobotApp::Init()
{
    if (state_.load() != RobotAppState::NEW) {
        return ESP_ERR_INVALID_STATE;
    }
    event_queue_ = xQueueCreate(kEventQueueDepth, sizeof(Event));
    if (!event_queue_) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[init 1/5] OLED");
    esp_err_t err = robot_ui_init();
    ui_ready_ = err == ESP_OK;
    if (!ui_ready_) {
        ESP_LOGW(TAG, "OLED unavailable: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "[init 2/5] motion queue");
    err = robot_action_queue_init();
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Reserve Wi-Fi's internal-memory resources before ESP-SR creates its
     * high-performance AFE. On ESP32-S3 this avoids a boot-time NO_MEM abort.
     */
    ESP_LOGI(TAG, "[init 3/5] NVS and Wi-Fi");
    robot_network_config_t network = {
        .sta_timeout_seconds = CONFIG_ROBOT_STA_CONNECT_TIMEOUT_SECONDS,
        .event_cb = NetworkEvent,
        .event_ctx = this,
        .provisioning_cb = ProvisioningUi,
        .provisioning_ctx = this,
    };
    if ((err = robot_network_init(&network)) != ESP_OK) {
        ESP_LOGE(TAG, "network init failed: %s", esp_err_to_name(err));
        return err;
    }
    network_ready_ = true;

    ESP_LOGI(TAG, "[init 4/5] I2S audio");
    robot_audio_config_t audio = {
        /* Capture rate only; playback stays fixed at 16 kHz. */
        .sample_rate_hz = CONFIG_ROBOT_MIC_SAMPLE_RATE,
        .mic_bclk_gpio = 5,
        .mic_ws_gpio = 4,
        .mic_data_gpio = 6,
        .speaker_bclk_gpio = 15,
        .speaker_ws_gpio = 16,
        .speaker_data_gpio = 7,
        .playback_started = PlaybackStarted,
        .playback_started_ctx = this,
        .playback_drained = PlaybackDrained,
        .playback_drained_ctx = this,
    };
    if ((err = robot_audio_init(&audio)) != ESP_OK ||
        (err = robot_audio_register_pcm_consumer(PcmConsumer, this)) != ESP_OK) {
        return err;
    }
    const uint8_t initial_volume = load_saved_volume();
    if ((err = robot_audio_set_volume(initial_volume)) != ESP_OK ||
        (err = robot_audio_start()) != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "[init 5/5] buttons");
    gpio_config_t buttons = {
        .pin_bit_mask = (1ULL << kBootPin) |
                        (1ULL << kVolumeUpPin) |
                        (1ULL << kVolumeDownPin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&buttons)) != ESP_OK) {
        return err;
    }
    state_.store(RobotAppState::STARTING);
    ESP_LOGI(TAG, "initialized, heap=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t RobotApp::Start()
{
    if (state_.load() != RobotAppState::STARTING || running_.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    boot_running_.store(true);
    if (xTaskCreate(WorkerEntry, "robot_app", 8192, this, 5,
                    &worker_task_) != pdPASS ||
        xTaskCreate(BootEntry, "boot_button", kBootTaskStackBytes, this, 4,
                    &boot_task_) != pdPASS) {
        running_.store(false);
        boot_running_.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void RobotApp::Stop()
{
    if (!running_.load()) {
        return;
    }
    Event event = {};
    event.type = EventType::STOP;
    PostEvent(event);
}

void RobotApp::Deinit()
{
    boot_running_.store(false);

    DeinitAiui();
    if (gesture_ready_) {
        robot_gesture_deinit();
    }
    if (wake_ready_) {
        robot_wakeup_deinit();
    }
    if (network_ready_) {
        robot_network_deinit();
    }
    robot_audio_deinit();
    robot_action_queue_deinit();
    if (ui_ready_) {
        robot_ui_deinit();
    }
    if (event_queue_) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
    state_.store(RobotAppState::STOPPED);
}

esp_err_t RobotApp::PostEvent(const Event &event)
{
    if (!event_queue_) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(event_queue_, &event, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

void RobotApp::WorkerEntry(void *ctx)
{
    auto *self = static_cast<RobotApp *>(ctx);
    esp_err_t network_result = robot_network_start();
    if (network_result != ESP_OK &&
        self->state_.load() != RobotAppState::PROVISIONING) {
        ESP_LOGE(TAG, "network start failed: %s", esp_err_to_name(network_result));
        self->state_.store(RobotAppState::FAILED);
    }
    if (robot_network_time_ready()) {
        self->SetupOnlineServices();
    } else if (network_result == ESP_OK) {
        self->state_.store(RobotAppState::PROVISIONING);
    }

    while (self->running_.load()) {
        Event event = {};
        if (xQueueReceive(self->event_queue_, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            self->HandleEvent(event);
            if (event.payload) {
                heap_caps_free(event.payload);
            }
        }
        if (self->state_.load() == RobotAppState::SESSION_ACTIVE &&
            self->last_activity_ms_ != 0) {
            uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
            self->DispatchReplyPresentation(now);
            self->MaybeDispatchListeningAnimation(now);

            if (self->aiui_retry_at_ms_ != 0 &&
                now >= self->aiui_retry_at_ms_) {
                self->aiui_retry_at_ms_ = 0;
                ++self->aiui_retry_count_;
                ESP_LOGI(TAG, "AIUI connect retry %u/%u",
                         (unsigned)self->aiui_retry_count_,
                         (unsigned)kAiuiMaxConnectRetries);
                if (aiui_start(self->aiui_.handle) != 0) {
                    if (self->aiui_retry_count_ >=
                        kAiuiMaxConnectRetries) {
                        self->StopSession("AIUI connect retries exhausted");
                        continue;
                    }
                    uint32_t retry_delay =
                        (self->aiui_retry_count_ + 1U) *
                        kAiuiConnectRetryBaseMs;
                    self->aiui_retry_at_ms_ = now + retry_delay;
                    ESP_LOGW(TAG,
                             "AIUI retry start failed; next retry in %u ms",
                             (unsigned)retry_delay);
                }
            }
            uint32_t last_tts_chunk =
                self->aiui_.tts_last_chunk_ms.load();
            /*
             * The TTS callback can update last_tts_chunk after WorkerEntry
             * sampled `now`. Clamp that short cross-task race to zero instead
             * of turning -N ms into a ~49-day unsigned timeout.
             */
            const uint32_t tts_now_ms =
                (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
            const int32_t signed_tts_gap_ms =
                (int32_t)(tts_now_ms - last_tts_chunk);
            const uint32_t tts_gap_ms =
                last_tts_chunk == 0 || signed_tts_gap_ms < 0
                    ? 0
                    : (uint32_t)signed_tts_gap_ms;
            if (self->aiui_.tts_playing.load() &&
                !self->aiui_.tts_final_received.load() &&
                last_tts_chunk != 0 &&
                tts_gap_ms >= kTtsChunkStallTimeoutMs) {
                /*
                 * Do not resume audio on this WebSocket: until status=2 the
                 * cloud still owns the previous request and will not treat
                 * subsequent PCM as a new turn. Drain the local stream, close
                 * the stale transport and let AIUI_DISCONNECTED reconnect it.
                 */
                self->aiui_.tts_last_chunk_ms.store(0);
                self->aiui_.connected.store(false);
                self->aiui_.audio_request_open.store(false);
                self->aiui_.awaiting_reply.store(false);
                self->aiui_.awaiting_reply_since_ms.store(0);
                robot_audio_set_capture_paused(true);
                if (self->aiui_.audio_queue) {
                    xQueueReset(self->aiui_.audio_queue);
                }
                ESP_LOGE(TAG,
                         "TTS final frame missing for %u ms; draining audio "
                         "and reconnecting AIUI",
                         (unsigned)tts_gap_ms);
                if (robot_audio_aiui_opus_end() != ESP_OK) {
                    self->StopSession("TTS stream recovery failed");
                    continue;
                }
                if (aiui_stop(self->aiui_.handle) != 0) {
                    self->StopSession("AIUI transport recovery failed");
                    continue;
                }
            }
            const uint32_t awaiting_since =
                self->aiui_.awaiting_reply_since_ms.load();
            if (self->aiui_.awaiting_reply.load() &&
                !self->aiui_.turn_result_seen.load() &&
                !self->aiui_.tts_playing.load() &&
                awaiting_since != 0) {
                const uint32_t reply_now_ms =
                    (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
                const int32_t reply_wait_ms =
                    (int32_t)(reply_now_ms - awaiting_since);
                if (reply_wait_ms >=
                    (int32_t)kNoSpeechResultTimeoutMs) {
                    self->ResumeListeningAfterNoResult(
                        "AIUI returned no valid speech result");
                }
            }
            if (!self->session_exit_pending_.load() &&

                !self->aiui_.tts_playing.load() &&
                !robot_audio_is_playing() &&
                !self->aiui_.awaiting_reply.load() &&
                !self->aiui_.utterance_active.load() &&
                !self->aiui_.cloud_utterance_active.load() &&
                now - self->last_activity_ms_ >=
                    CONFIG_ROBOT_SESSION_IDLE_TIMEOUT_SECONDS * 1000ULL) {
                self->BeginIdleTimeoutExit();
            }
        }
    }
    self->state_.store(RobotAppState::STOPPING);
    self->boot_running_.store(false);
    if (self->gesture_ready_) {
        robot_gesture_stop();
    }
    if (self->wake_ready_) {
        robot_wakeup_stop();
    }

    self->StopAiui();
    robot_network_stop();
    self->state_.store(RobotAppState::STOPPED);
    vTaskDelete(nullptr);
}

esp_err_t RobotApp::SetupOnlineServices()
{
    if (!robot_network_time_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "[online 1/3] AIUI credentials and runtime");
    esp_err_t err = InitAiui();
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_ARG) {
#if CONFIG_ROBOT_ALLOW_VOICELESS
        /*
         * Device-management-only build.  The voice stack is optional here, so a
         * missing credential must not send the device back to provisioning:
         * expressions, servos, buttons and the console channel keep working,
         * while wake word, conversation and speech synthesis stay unavailable
         * until a credential is provided.
         */
        ESP_LOGW(TAG, "AIUI credentials absent; running without the voice stack");
        state_.store(RobotAppState::OFFLINE_WAKE_READY);
        robot_ui_set_idle_animation(true);
        return ESP_OK;
#else
        ESP_LOGW(TAG, "AIUI credentials absent; returning to local provisioning");
        robot_network_enter_provisioning();
        state_.store(RobotAppState::PROVISIONING);
        return err;
#endif
    }
    if (err != ESP_OK) {
        state_.store(RobotAppState::FAILED);
        return err;
    }

#if CONFIG_ROBOT_ENABLE_ESP_SR
    if (!wake_ready_) {
        ESP_LOGI(TAG, "[online 2/3] ESP-SR MultiNet custom wake");
        err = robot_wakeup_init(WakeCallback, this);
        wake_ready_ = err == ESP_OK;
        if (!wake_ready_) {
            ESP_LOGE(TAG, "offline wake unavailable: %s", esp_err_to_name(err));
            DeinitAiui();
            state_.store(RobotAppState::FAILED);
            return err;
        }
    }
#endif

#if CONFIG_ROBOT_ENABLE_GESTURE
    if (!gesture_ready_ && ui_ready_) {
        ESP_LOGI(TAG, "[online 3/3] gesture sensor");
        err = robot_gesture_init(GestureCallback, this);
        gesture_ready_ = err == ESP_OK;
        if (!gesture_ready_) {
            ESP_LOGW(TAG, "gesture sensor unavailable: %s", esp_err_to_name(err));
        }
    }
#endif

    const bool play_ready_prompt = !s_ready_prompt_running.exchange(true);
    if (play_ready_prompt) {
        /*
         * Keep microphone frames away from offline recognition while the
         * local 7911 prompt is playing.
         */
        robot_audio_set_capture_paused(true);
    }
    if (wake_ready_) {
        robot_wakeup_start();
    }
    if (gesture_ready_) {
        robot_gesture_start();
    }
    state_.store(RobotAppState::OFFLINE_WAKE_READY);
    robot_ui_set_idle_animation(true);
    ESP_LOGI(TAG, "offline wake ready: 小飞小飞 or BOOT short press");
    if (ui_ready_) {
        robot_action_t action = {};
        action.type = ROBOT_ACTION_EXPRESSION;
        action.expression = ROBOT_EXPRESSION_HAPPY;
        robot_action_queue_submit(&action, 0);
    }
    if (play_ready_prompt) {
        if (xTaskCreate(ready_prompt_task, "ready_prompt",
                        kReadyPromptStackBytes, nullptr, 3, nullptr) != pdPASS) {
            s_ready_prompt_running.store(false);
            robot_audio_set_capture_paused(false);
            ESP_LOGW(TAG, "unable to create ready prompt task");
        } else {
            ESP_LOGI(TAG, "ready prompt: network connected, wake word enabled");
        }
    }
    return ESP_OK;
}

void RobotApp::HandleEvent(const Event &event)
{
    switch (event.type) {
        /*
         * BOOT long press.  This gesture has exactly one meaning: open the
         * setup page.  It used to restart the robot when the app was already in
         * its provisioning state, which read as "the long press did nothing"
         * (or as an unexplained reboot) whenever the app state and the network
         * state had drifted apart - the *network* still serving the access
         * point while the app had moved on.  Now the same request is simply
         * made again: robot_network re-announces provisioning even when it is
         * already up, so the screen and the prompt come back and the app state
         * is re-synchronised with reality.  Leaving the page is done by saving
         * something (the device restarts by itself) or by power-cycling.
         */
        case EventType::PROVISION_REQUEST:
            if (state_.load() == RobotAppState::SESSION_ACTIVE) {
                StopSession("provisioning requested from BOOT long press");
            }
            ESP_LOGI(TAG, "provisioning requested from BOOT long press");
            robot_network_enter_provisioning();
            break;
        case EventType::WAKE_TRIGGER:
            if (state_.load() == RobotAppState::OFFLINE_WAKE_READY &&
                robot_network_time_ready()) {
#if CONFIG_ROBOT_CLOUD_VOICE
                /*
                 * A console-triggered turn already holds the microphone. Waking
                 * on top of it would start an AIUI session in the middle of a
                 * recording, so drop this wake instead.
                 */
                if (robot_voice_busy()) {
                    break;
                }
#endif
                robot_wakeup_stop();
                robot_ui_set_idle_animation(false);
                robot_action_t wake_action = {};
                wake_action.type = ROBOT_ACTION_WAKEUP;
                robot_action_queue_submit(&wake_action, 0);
                robot_audio_set_capture_paused(true);
                playback_generation_.store(
                    robot_audio_start_playback_stream());
                esp_err_t prompt_err =
                    event.code == kWakeSourceBoot
                        ? robot_audio_play_wav_file("/assets/key_wakeup.wav")
                        : robot_audio_play_wakeup_tone();
                if (prompt_err != ESP_OK) {
                    ESP_LOGW(TAG, "wake acknowledgement failed: %s",
                             esp_err_to_name(prompt_err));
                }
#if CONFIG_ROBOT_CLOUD_VOICE
                /*
                 * With the cloud voice channel on, the wake word opens a turn
                 * against the role chosen in the console rather than the
                 * built-in skill. Falling through to StartAiui when the turn
                 * cannot start keeps the robot usable if the console has not
                 * been given voice credentials yet.
                 */
                if (robot_voice_trigger() == ESP_OK) {
                    state_.store(RobotAppState::CLOUD_VOICE_ACTIVE);
                    break;
                }
#endif
                if (StartAiui() == ESP_OK) {
                    state_.store(RobotAppState::SESSION_ACTIVE);
                    TouchActivity();
                } else {
                    robot_audio_set_capture_paused(false);
                    robot_wakeup_start();
                    robot_ui_set_idle_animation(true);
                    robot_action_t neutral_action = {};
                    neutral_action.type = ROBOT_ACTION_EXPRESSION;
                    neutral_action.expression = ROBOT_EXPRESSION_NEUTRAL;
                    robot_action_queue_submit(&neutral_action, 0);
                }
            }
            break;
        case EventType::NETWORK_GOT_IP:
            if (!robot_network_time_ready()) {
                robot_network_sync_time();
            }
#if CONFIG_ROBOT_CLOUD_ENABLE
            /*
             * The console channel only needs IP connectivity, so it starts here
             * rather than in SetupOnlineServices: managing the device must not
             * depend on the voice stack initialising successfully.  Disabled by
             * default, so the stock behaviour is unchanged unless the channel is
             * enabled in menuconfig with a server URL and device key.
             */
            {
                /*
                 * Both values can come from the setup page, because the machine
                 * running the console changes address every time it joins a
                 * different network and reflashing the robot to follow it is not
                 * a reasonable thing to ask.  Anything the page did not supply
                 * falls back to the value compiled in here, so a robot that has
                 * never been provisioned behaves exactly as before.
                 *
                 * The storage has to be static: robot_cloud_init keeps the
                 * pointers rather than copying the strings, and this block is
                 * long gone by the time the channel makes its first request.
                 */
                static robot_cloud_credentials_t provisioned;
                robot_network_load_cloud_credentials(&provisioned);
                robot_cloud_config_t cloud_config = {};
                cloud_config.base_url = provisioned.base_url[0]
                    ? provisioned.base_url : CONFIG_ROBOT_CLOUD_URL;
                cloud_config.device_key = provisioned.device_key[0]
                    ? provisioned.device_key : CONFIG_ROBOT_CLOUD_KEY;
                cloud_config.poll_interval_ms =
                    (uint32_t)CONFIG_ROBOT_CLOUD_POLL_INTERVAL_S * 1000U;
                cloud_config.status_interval_ms =
                    (uint32_t)CONFIG_ROBOT_CLOUD_STATUS_INTERVAL_S * 1000U;
                cloud_config.speak = CloudSpeak;
                cloud_config.speak_ctx = this;
                /*
                 * Log which source each half came from.  Without this there is
                 * no way to tell "the setup page value took effect" from "the
                 * setup page value was ignored and the old compiled-in one is
                 * still in use" - and the device key is deliberately not logged.
                 */
                ESP_LOGI(TAG, "device channel target: %s (key from %s)",
                         cloud_config.base_url,
                         provisioned.device_key[0] ? "setup page" : "firmware");
                if (robot_cloud_init(&cloud_config) == ESP_OK) {
                    robot_cloud_start();
                }
            }
#if CONFIG_ROBOT_CLOUD_VOICE
            /*
             * Voice reuses the server URL and device key already configured for
             * the console channel, so there is only one endpoint to set up.
             * It gets its own task because a turn keeps the microphone open and
             * must never block the poll loop that keeps the device online.
             */
            {
                robot_voice_options_t voice_options = {};
                voice_options.sample_rate_hz = CONFIG_ROBOT_MIC_SAMPLE_RATE;
                voice_options.max_record_ms = (uint32_t)CONFIG_ROBOT_CLOUD_VOICE_MAX_RECORD_MS;
                voice_options.silence_stop_ms = (uint32_t)CONFIG_ROBOT_CLOUD_VOICE_SILENCE_MS;
                voice_options.silence_threshold = (uint32_t)CONFIG_ROBOT_CLOUD_VOICE_THRESHOLD;
                if (robot_voice_init(&voice_options) == ESP_OK) {
                    robot_voice_set_state_callback(CloudVoiceState, this);
                    robot_voice_start();
                }
            }
#endif
#endif
            break;
        case EventType::NETWORK_TIME_READY:
            if (!aiui_.handle) {
                SetupOnlineServices();
            }
            break;
        case EventType::NETWORK_DISCONNECTED:
            StopSession("network disconnected");
            break;
        case EventType::AIUI_CONNECTED: {
            if (state_.load() != RobotAppState::SESSION_ACTIVE ||
                session_exit_pending_.load() ||
                !aiui_.active.load()) {
                break;
            }
            aiui_.connected.store(true);
            aiui_.awaiting_reply.store(false);
            aiui_.awaiting_reply_since_ms.store(0);
            aiui_.turn_result_seen.store(false);
            aiui_.semantic_session_end_known.store(false);
            aiui_.semantic_should_end_session.store(false);
            aiui_.discard_tts_until_final.store(false);
            aiui_.audio_request_open.store(false);
            aiui_.cloud_utterance_active.store(false);
            aiui_retry_at_ms_ = 0;
            aiui_retry_count_ = 0;
            if (aiui_.audio_queue) {
                xQueueReset(aiui_.audio_queue);
            }
            const bool playback_active = robot_audio_is_playing();
            robot_audio_set_capture_paused(playback_active);
            TouchActivity();
            const uint64_t now =
                (uint64_t)(esp_timer_get_time() / 1000ULL);
            next_listening_animation_ms_ =
                now + kListeningAnimationMinIntervalMs +
                (esp_random() % kListeningAnimationJitterMs);
            ESP_LOGI(TAG, "AIUI connected");
            if (playback_active) {
                ESP_LOGI(TAG,
                         "capture remains paused until playback drains");
            }
            break;
        }
        case EventType::AIUI_DISCONNECTED:
            if (state_.load() != RobotAppState::SESSION_ACTIVE) {
                break;
            }
            if (session_exit_pending_.load() || !aiui_.active.load()) {
                ESP_LOGI(TAG,
                         "AIUI disconnected for pending session exit");
                break;
            }
            aiui_.connected.store(false);
            aiui_.tts_playing.store(false);
            aiui_.tts_final_received.store(false);
            aiui_.awaiting_reply.store(false);
            aiui_.awaiting_reply_since_ms.store(0);
            aiui_.turn_result_seen.store(false);
            aiui_.semantic_session_end_known.store(false);
            aiui_.semantic_should_end_session.store(false);
            aiui_.discard_tts_until_final.store(false);
            aiui_.audio_request_open.store(false);
            aiui_.cloud_utterance_active.store(false);
            robot_audio_aiui_opus_abort();
            robot_audio_set_capture_paused(true);
            if (aiui_.audio_queue) {
                xQueueReset(aiui_.audio_queue);
            }
            if (aiui_retry_at_ms_ != 0) {
                break;
            }
            if (aiui_retry_count_ >= kAiuiMaxConnectRetries) {
                StopSession("AIUI connect retries exhausted");
                break;
            }
            {
                uint32_t retry_delay =
                    (aiui_retry_count_ + 1U) *
                    kAiuiConnectRetryBaseMs;
                aiui_retry_at_ms_ =
                    (uint64_t)(esp_timer_get_time() / 1000) + retry_delay;
                ESP_LOGW(TAG,
                         "AIUI disconnected; retry %u/%u in %u ms",
                         (unsigned)(aiui_retry_count_ + 1U),
                         (unsigned)kAiuiMaxConnectRetries,
                         (unsigned)retry_delay);
            }
            break;
        case EventType::AIUI_ERROR:
            ESP_LOGE(TAG, "AIUI error code=%d", event.code);
            if (event.code == 10024 || event.code == 500) {
                /*
                 * 7911 treats an empty recognition/result turn as a request
                 * failure rather than a broken WebSocket session.  Code 500
                 * with "res empty" is the same condition after IAT succeeds:
                 * discard this turn and immediately resume continuous
                 * listening instead of leaving capture paused.
                 */
                ResumeListeningAfterNoResult(
                    event.code == 500 ? "AIUI cloud returned empty response"
                                      : "AIUI empty recognition");
            } else if (event.code == 900018 || event.code == 10012) {
                StopSession("AIUI cloud session timeout");
            } else {
                StopSession("AIUI error");
            }
            break;
        case EventType::AIUI_RESULT:
            HandleResult(event);
            break;
        case EventType::AIUI_SPEECH_END:
            /*
             * The rapid-interaction service reports its own VAD EOS before
             * the local VAD on some utterances. Stop the external audio
             * request at that point. Otherwise the server keeps the request
             * open and may withhold the final TTS status=2 packet.
             */
            if (aiui_.audio_request_open.exchange(false)) {
                robot_audio_set_capture_paused(true);
                if (aiui_.audio_queue) {
                    xQueueReset(aiui_.audio_queue);
                }
                if (aiui_send_audio(aiui_.handle, nullptr, 0) != 0) {
                    aiui_.connected.store(false);
                    Event disconnected = {};
                    disconnected.type = EventType::AIUI_DISCONNECTED;
                    disconnected.code = -1;
                    PostEvent(disconnected);
                } else {
                    ESP_LOGI(TAG,
                             "cloud VAD: speech ended; AIUI audio end sent");
                }
            }
            break;
        case EventType::AIUI_SILENCE:
            /*
             * Silence belongs to one audio request, not to the WebSocket
             * session. The 7911 demo filters a late Silence after clearing
             * cur_audio_stmid. Keep a valid result/TTS turn untouched; when
             * no result exists, cancel only this recognition attempt and
             * immediately return to continuous listening.
             */
            if (aiui_.turn_result_seen.load() ||
                aiui_.tts_playing.load() ||
                robot_audio_is_playing()) {
                ESP_LOGI(TAG,
                         "stale cloud Silence ignored after speech result");
            } else {
                ResumeListeningAfterNoResult("AIUI cloud Silence");
            }
            break;
        case EventType::PLAYBACK_STARTED:
            if (state_.load() != RobotAppState::SESSION_ACTIVE ||
                static_cast<uint32_t>(event.code) !=
                    playback_generation_.load() ||
                !aiui_.tts_playing.load()) {
                break;
            }
            reply_playback_started_ = true;
            next_listening_animation_ms_ = 0;
            ESP_LOGI(TAG, "AI reply presentation synchronized to playback");
            if (reply_pending_expression_valid_) {
                const int pending_expression = reply_pending_expression_;
                reply_pending_expression_valid_ = false;
                reply_pending_expression_ = -1;
                ApplyReplyExpression(pending_expression,
                                     "AIUI facial tag at playback");
            }
            ProcessReplyExpressionSegments(reply_expression_flush_pending_);
            reply_expression_flush_pending_ = false;
            DispatchReplyPresentation(
                (uint64_t)(esp_timer_get_time() / 1000ULL));
            break;
        case EventType::PLAYBACK_DRAINED: {
            if (state_.load() == RobotAppState::SESSION_ACTIVE &&
                static_cast<uint32_t>(event.code) !=
                    playback_generation_.load()) {
                ESP_LOGW(TAG,
                         "stale playback drain ignored: generation=%u "
                         "current=%u",
                         (unsigned)static_cast<uint32_t>(event.code),
                         (unsigned)playback_generation_.load());
                break;
            }
            if (session_exit_pending_.load()) {
                StopSession("idle timeout");
                break;
            }
            /*
             * A DRAIN marker belongs to the stream that queued it. During
             * continuous dialogue a delayed marker from an older prompt can
             * arrive after the next TTS has already started. Never let that
             * stale event clear tts_playing or reopen the microphone.
             */
            if (aiui_.tts_playing.load()) {
                const size_t buffered_samples =
                    robot_audio_playback_buffered_samples();
                if (!aiui_.tts_final_received.load() ||
                    buffered_samples != 0) {
                    ESP_LOGW(TAG,
                             "stale playback drain ignored: tts_final=%d "
                             "buffered=%u ms",
                             aiui_.tts_final_received.load() ? 1 : 0,
                             (unsigned)((buffered_samples * 1000ULL) /
                                        16000ULL));
                    break;
                }
            }
            const bool semantic_session_end_known =
                aiui_.semantic_session_end_known.load();
            const bool semantic_should_end_session =
                aiui_.semantic_should_end_session.load();
            const bool completed_tts = aiui_.tts_playing.load();
            aiui_.tts_playing.store(false);
            aiui_.tts_final_received.store(false);
            aiui_.discard_tts_until_final.store(false);
            aiui_.tts_last_chunk_ms.store(0);
            aiui_.awaiting_reply.store(false);
            aiui_.awaiting_reply_since_ms.store(0);
            aiui_.turn_result_seen.store(false);
            aiui_.audio_request_open.store(false);
            aiui_.cloud_utterance_active.store(false);
            aiui_.vad_speech_ms.store(0);
            aiui_.vad_silence_ms.store(0);
            aiui_.utterance_active.store(false);
            if (completed_tts) {
                ResetReplyPresentation();
                const uint64_t now =
                    (uint64_t)(esp_timer_get_time() / 1000ULL);
                next_listening_animation_ms_ =
                    now + kListeningAnimationMinIntervalMs +
                    (esp_random() % kListeningAnimationJitterMs);
            }
            if (aiui_.vad_handle) {
                vad_reset_trigger(
                    static_cast<vad_handle_t>(aiui_.vad_handle));
            }
            if (aiui_.audio_queue) {
                /*
                 * Capture is paused while TTS is playing, but frames queued
                 * between endpointing and the first TTS packet belong to the
                 * previous request. Never send them as the beginning of the
                 * next continuous-dialogue turn.
                 */
                xQueueReset(aiui_.audio_queue);
            }

            aiui_.semantic_session_end_known.store(false);
            aiui_.semantic_should_end_session.store(false);
            if (semantic_session_end_known &&
                semantic_should_end_session) {
                /*
                 * shouldEndSession closes the cloud skill turn, not the
                 * device's wake session.  Keep local continuous dialogue
                 * active so a clarification question can be answered without
                 * requiring another wake word.  Explicit exit commands and
                 * the idle-timeout farewell still close the connection.
                 */
                ESP_LOGI(TAG,
                         "AIUI skill turn ended; local dialogue remains active");
            }
            if (state_.load() == RobotAppState::SESSION_ACTIVE &&
                !aiui_.connected.load()) {
                robot_audio_set_capture_paused(true);
            } else {
                robot_audio_set_capture_paused(false);
            }
            TouchActivity();
            break;
        }
        case EventType::MEDIA_STOP_REQUEST:
            /*
             * A cloud voice turn owns the speaker while it streams.  Aborting
             * the queued audio alone only skips what is already buffered and
             * the rest of the answer keeps arriving, so the turn itself has to
             * be cancelled; that path drops the queued audio and un-pauses
             * capture, so there is nothing left to do here.
             */
            if (cancel_cloud_voice_turn()) {
                /* Silence it now: the turn task cuts the remaining stream as
                 * soon as its pending read returns, but the audio already
                 * queued would otherwise keep playing until then. */
                robot_audio_abort_playback();
                break;
            }
            if (session_exit_pending_.exchange(false)) {
                robot_audio_abort_playback();
                StopSession("idle timeout prompt interrupted");
                break;
            }
            if (aiui_.tts_playing.load() || robot_audio_is_playing()) {
                const bool interrupted_tts = aiui_.tts_playing.load();
                const bool wait_for_tts_final =
                    interrupted_tts &&
                    !aiui_.tts_final_received.load();
                robot_audio_set_capture_paused(true);
            
                robot_audio_aiui_opus_abort();
                robot_audio_abort_playback();
                playback_generation_.store(
                    robot_audio_get_playback_generation());
                aiui_.discard_tts_until_final.store(wait_for_tts_final);
                aiui_.tts_playing.store(wait_for_tts_final);
                aiui_.tts_final_received.store(false);
                if (!wait_for_tts_final) {
                    aiui_.tts_last_chunk_ms.store(0);
                }
                aiui_.awaiting_reply.store(wait_for_tts_final);
                aiui_.awaiting_reply_since_ms.store(
                    wait_for_tts_final
                        ? (uint32_t)((uint64_t)esp_timer_get_time() /
                                     1000ULL)
                        : 0);
                aiui_.turn_result_seen.store(false);
                aiui_.semantic_session_end_known.store(false);
                aiui_.semantic_should_end_session.store(false);
                aiui_.audio_request_open.store(false);
                aiui_.cloud_utterance_active.store(false);
                aiui_.vad_speech_ms.store(0);
                aiui_.vad_silence_ms.store(0);
                aiui_.utterance_active.store(false);
                if (aiui_.vad_handle) {
                    vad_reset_trigger(
                        static_cast<vad_handle_t>(aiui_.vad_handle));
                }
                if (aiui_.audio_queue) {
                    xQueueReset(aiui_.audio_queue);
                }
                if (!wait_for_tts_final &&
                    ((state_.load() == RobotAppState::SESSION_ACTIVE &&
                      aiui_.connected.load()) ||
                     state_.load() == RobotAppState::OFFLINE_WAKE_READY)) {
                    robot_audio_set_capture_paused(false);
                }
                TouchActivity();
                if (wait_for_tts_final) {
                    ESP_LOGI(TAG,
                             "TTS interrupted by BOOT; waiting for "
                             "cloud final frame");
                } else if (interrupted_tts) {
                    ESP_LOGI(TAG,
                             "TTS tail interrupted by BOOT; "
                             "conversation resumed");
                } else {
                    ESP_LOGI(TAG,
                             "media/output stopped by BOOT; "
                             "conversation resumed");
                }
            }
            break;
        case EventType::GESTURE:
            SubmitGesture(event.gesture);
            if (event.gesture == ROBOT_GESTURE_FORWARD &&
                state_.load() == RobotAppState::OFFLINE_WAKE_READY) {
                Event trigger = {};
                trigger.type = EventType::WAKE_TRIGGER;
                PostEvent(trigger);
            }
            break;
#if CONFIG_ROBOT_CLOUD_VOICE
        case EventType::CLOUD_VOICE_STARTED:
            /* The voice task owns the microphone for the whole turn. */
            state_.store(RobotAppState::CLOUD_VOICE_ACTIVE);
            TouchActivity();
            break;
        case EventType::CLOUD_VOICE_FINISHED: {
            if (state_.load() != RobotAppState::CLOUD_VOICE_ACTIVE) {
                break;
            }
            /* A failed turn produces no audio at all, so the reason has to be
             * visible somewhere; the serial log is the only place today. */
            ESP_LOGI(TAG, "cloud voice turn finished: %s",
                     robot_voice_last_result());
            /* The voice module restores capture itself; this only puts the
             * robot back in the state a wake would have left it in. */
            robot_action_t cloud_neutral = {};
            cloud_neutral.type = ROBOT_ACTION_EXPRESSION;
            cloud_neutral.expression = ROBOT_EXPRESSION_NEUTRAL;
            robot_action_queue_submit(&cloud_neutral, 0);
            state_.store(RobotAppState::OFFLINE_WAKE_READY);
            robot_wakeup_start();
            robot_ui_set_idle_animation(true);
            break;
        }
#endif
        case EventType::STOP:
            running_.store(false);
            break;
    }
}









#if CONFIG_ROBOT_CLOUD_VOICE
void RobotApp::CloudVoiceState(void *ctx, bool listening)
{
    auto *self = static_cast<RobotApp *>(ctx);
    Event event = {};
    event.type = listening ? EventType::CLOUD_VOICE_STARTED
                           : EventType::CLOUD_VOICE_FINISHED;
    self->PostEvent(event);
}
#endif

void RobotApp::SubmitExpression(int expression)
{
    robot_action_t action = {};
    action.type = ROBOT_ACTION_EXPRESSION;
    action.expression = static_cast<robot_expression_t>(expression);
    robot_action_queue_submit(&action, 0);
}

void RobotApp::SubmitGesture(robot_gesture_t gesture)
{
    robot_action_t action = {};
    switch (gesture) {
        case ROBOT_GESTURE_LEFT: action.type = ROBOT_ACTION_LOOK_LEFT; break;
        case ROBOT_GESTURE_RIGHT: action.type = ROBOT_ACTION_LOOK_RIGHT; break;
        case ROBOT_GESTURE_UP: action.type = ROBOT_ACTION_LOOK_UP; break;
        case ROBOT_GESTURE_DOWN: action.type = ROBOT_ACTION_LOOK_DOWN; break;
        case ROBOT_GESTURE_NOD: action.type = ROBOT_ACTION_NOD; break;
        case ROBOT_GESTURE_SHAKE: action.type = ROBOT_ACTION_SHAKE; break;
        case ROBOT_GESTURE_CLOCKWISE: action.type = ROBOT_ACTION_ROLL; break;
        case ROBOT_GESTURE_COUNTERCLOCKWISE: action.type = ROBOT_ACTION_SHAKE; break;
        case ROBOT_GESTURE_WAVE:
            action.type = ROBOT_ACTION_DANCE;
            break;
        case ROBOT_GESTURE_FORWARD:
            action.type = ROBOT_ACTION_EXPRESSION;
            action.expression = ROBOT_EXPRESSION_SURPRISED;
            break;
        case ROBOT_GESTURE_BACKWARD:
            action.type = ROBOT_ACTION_BLINK;
            break;
        default:
            action.type = ROBOT_ACTION_CENTER;
            break;
    }
    esp_err_t err = robot_action_queue_submit(&action, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "gesture action queued: gesture=%d action=%d",
                 static_cast<int>(gesture), static_cast<int>(action.type));
    } else {
        ESP_LOGW(TAG, "gesture action queue full: gesture=%d err=%s",
                 static_cast<int>(gesture), esp_err_to_name(err));
    }
}

void RobotApp::TouchActivity()
{
    last_activity_ms_ = (uint64_t)(esp_timer_get_time() / 1000);
}












/*
 * Microphone level, throttled to roughly one line per second.
 *
 * This used to be logged only while an AIUI session was open. On a build
 * without AIUI credentials nothing consumes the frames at all - the wake word
 * engine is not initialised either, because setup returns early - so there was
 * no way whatsoever to tell whether the microphone was capturing. "The robot
 * did not hear me" then has to be diagnosed from the microphone instead of
 * from the cloud, and this line is what makes that possible.
 */
static void log_microphone_level(const int16_t *samples, size_t sample_count)
{
    static uint32_t frames = 0;
    frames += 1;
    if (frames != 1 && frames % 25 != 0) {
        return;
    }
    int32_t peak = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t value = samples[i];
        if (value < 0) {
            value = -value;
        }
        if (value > peak) {
            peak = value;
        }
    }
    ESP_LOGI(TAG, "microphone peak=%d samples=%u%s",
             (int)peak, (unsigned)sample_count, peak == 0 ? " (silent)" : "");
}

void RobotApp::PcmConsumer(void *ctx, const int16_t *samples,
                           size_t sample_count, uint64_t capture_ms)
{
    auto *self = static_cast<RobotApp *>(ctx);
    RobotAppState state = self->state_.load();
    if (state == RobotAppState::OFFLINE_WAKE_READY) {
        log_microphone_level(samples, sample_count);
        robot_wakeup_feed_pcm(samples, sample_count, capture_ms);
    } else if (state == RobotAppState::SESSION_ACTIVE) {
        /*
         * capture_paused is the primary gate. Keep this second gate here so
         * an old playback callback can never enqueue microphone data into a
         * TTS/media turn during a cross-task state transition.
         */
        if (self->aiui_.tts_playing.load() ||
            self->aiui_.awaiting_reply.load() ||
            false) {
            return;
        }
        log_microphone_level(samples, sample_count);
        self->FeedAiui(samples, sample_count);
    } else {
        /* A cloud voice turn records through a tap rather than here, but the
         * level is still worth seeing: it is the only evidence available when
         * recognition reports that the audio contained no speech. */
        log_microphone_level(samples, sample_count);
    }
}

void RobotApp::WakeCallback(void *ctx)
{
    auto *self = static_cast<RobotApp *>(ctx);
    Event event = {};
    event.type = EventType::WAKE_TRIGGER;
    event.code = kWakeSourceVoice;
    self->PostEvent(event);
}

void RobotApp::PlaybackDrained(void *ctx, uint32_t generation)
{
    auto *self = static_cast<RobotApp *>(ctx);
    Event event = {};
    event.type = EventType::PLAYBACK_DRAINED;
    event.code = static_cast<int>(generation);
    self->PostEvent(event);
}

void RobotApp::PlaybackStarted(void *ctx, uint32_t generation)
{
    auto *self = static_cast<RobotApp *>(ctx);
    Event event = {};
    event.type = EventType::PLAYBACK_STARTED;
    event.code = static_cast<int>(generation);
    self->PostEvent(event);
}

void RobotApp::NetworkEvent(void *ctx, int event_id)
{
    auto *self = static_cast<RobotApp *>(ctx);
    Event event = {};
    if (event_id == ROBOT_NETWORK_GOT_IP) {
        event.type = EventType::NETWORK_GOT_IP;
    } else if (event_id == ROBOT_NETWORK_TIME_READY) {
        event.type = EventType::NETWORK_TIME_READY;
    } else if (event_id == ROBOT_NETWORK_DISCONNECTED) {
        event.type = EventType::NETWORK_DISCONNECTED;
    } else {
        return;
    }
    self->PostEvent(event);
}

void RobotApp::ProvisioningUi(void *ctx, const char *ssid, const char *url)
{
    auto *self = static_cast<RobotApp *>(ctx);
    self->state_.store(RobotAppState::PROVISIONING);
    robot_action_t action = {};
    action.type = ROBOT_ACTION_SHOW_PROVISIONING;
    strlcpy(action.text_a, ssid, sizeof(action.text_a));
    strlcpy(action.text_b, url, sizeof(action.text_b));
    robot_action_queue_submit(&action, 0);
    if (!s_provisioning_prompt_running.exchange(true)) {
        if (xTaskCreate(provisioning_prompt_task, "provision_prompt",
                        kProvisioningPromptStackBytes, nullptr, 3, nullptr) != pdPASS) {
            s_provisioning_prompt_running.store(false);
            ESP_LOGW(TAG, "unable to create provisioning prompt task");
        }
    }
}

void RobotApp::GestureCallback(void *ctx, robot_gesture_t gesture)
{
    auto *self = static_cast<RobotApp *>(ctx);
    Event event = {};
    event.type = EventType::GESTURE;
    event.gesture = gesture;
    self->PostEvent(event);
}


void RobotApp::BootEntry(void *ctx)
{
    auto *self = static_cast<RobotApp *>(ctx);
    ButtonTracker boot{};
    boot.pin = kBootPin;
    ButtonTracker volume_up{};
    volume_up.pin = kVolumeUpPin;
    ButtonTracker volume_down{};
    volume_down.pin = kVolumeDownPin;
    while (self->boot_running_.load()) {
        const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
        /*
         * The short press keeps its meaning: request re-provisioning while a
         * connection attempt is still running, otherwise wake the robot (or
         * stop playback).  The long press is the deliberate "open the setup
         * page" gesture, so it must work while the device is online as well.
         */
        const ButtonEvent boot_event =
            poll_button(boot, now, kProvisionLongPressMs);
        if (boot_event == ButtonEvent::SHORT_PRESS) {
            if (!robot_network_request_reprovision()) {
                Event event = {};
                /*
                 * A cloud voice turn counts as "busy talking" too: pressing
                 * BOOT while the robot is recording or streaming used to be
                 * swallowed (it became a wake the state machine ignores).
                 */
                event.type =

                            self->aiui_.tts_playing.load() ||
                            robot_audio_is_playing() ||
                            cloud_voice_turn_busy()
                        ? EventType::MEDIA_STOP_REQUEST
                        : EventType::WAKE_TRIGGER;
                if (event.type == EventType::WAKE_TRIGGER) {
                    event.code = kWakeSourceBoot;
                }
                self->PostEvent(event);
            }
        } else if (boot_event == ButtonEvent::LONG_PRESS) {
            /*
             * Logged here as well as on the worker task: when the setup page
             * fails to show up, the serial log has to say whether the gesture
             * was even detected, because "nothing happened" otherwise covers
             * three very different faults (button not detected, event not
             * handled, access point not started).
             */
            ESP_LOGI(TAG, "BOOT long press detected");
            /* Handled on the worker task: leaving a session touches the AIUI
             * runtime and the playback stack, which only that task owns. */
            Event event = {};
            event.type = EventType::PROVISION_REQUEST;
            if (self->PostEvent(event) != ESP_OK) {
                ESP_LOGW(TAG, "unable to queue the provisioning request");
            }
        }
        ButtonEvent up_event =
            poll_button(volume_up, now, kVolumeLongPressMs);
        if (up_event == ButtonEvent::SHORT_PRESS) {
            set_and_save_volume(robot_audio_get_volume() + kVolumeStep);
        } else if (up_event == ButtonEvent::LONG_PRESS) {
            set_and_save_volume(100);
        }

        ButtonEvent down_event =
            poll_button(volume_down, now, kVolumeLongPressMs);
        if (down_event == ButtonEvent::SHORT_PRESS) {
            set_and_save_volume(robot_audio_get_volume() - kVolumeStep);
        } else if (down_event == ButtonEvent::LONG_PRESS) {
            set_and_save_volume(0);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}
