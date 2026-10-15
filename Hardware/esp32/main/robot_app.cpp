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
constexpr size_t kEventQueueDepth = 20;
constexpr uint32_t kBootTaskStackBytes = 4096;
constexpr uint32_t kProvisioningPromptStackBytes = 16 * 1024;
constexpr uint32_t kReadyPromptStackBytes = 16 * 1024;
/*
 * The ready prompt mutes the microphone, and the mute has to be lifted by hand
 * once the prompt has drained.  Poll it, but keep the wait bounded in case
 * playback never reports idle.
 */
constexpr uint32_t kPromptDrainPollMs = 20;
constexpr int kPromptDrainWaitSteps = 500;
/* 开机提示音用云端合成，但语音通道可能在开机瞬间还没起来（GOT_IP 里创建
 * 任务的 xTaskCreate 会因内存不足失败），所以给几次重试机会。 */
constexpr int kReadyPromptAttempts = 3;
constexpr uint32_t kReadyPromptRetryMs = 800;
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
    /*
     * 联网提示要念用户自己设的那个唤醒词，所以不能再放那段写死「小飞小飞」的
     * 预录音频——唤醒词是可以改的，预录的那句话只会越来越不对。改用云端语音
     * 合成（控制台播报走的就是这条链路）：文案里带用户写的中文词；读不出来时
     * 退回不含唤醒词的本地提示音，免得念出一个已经失效的旧词。
     */
    const char *label = robot_wakeup_label();
    bool spoke = false;
#if CONFIG_ROBOT_CLOUD_VOICE
    if (label[0] != '\0') {
        char line[96];
        snprintf(line, sizeof(line), "网络连接成功，喊%s就能唤醒我", label);
        for (int attempt = 0; attempt < kReadyPromptAttempts && !spoke; ++attempt) {
            esp_err_t err = robot_voice_say(line);
            if (err == ESP_OK) {
                spoke = true;
                break;
            }
            /* 错误码必须一起打：只打 last_result 时，早期返回的路径还没有它，
             * 日志上就只剩一个空冒号，等于什么都没说。 */
            ESP_LOGW(TAG, "cloud ready prompt attempt %d failed: %s (%s)",
                     attempt + 1, esp_err_to_name(err),
                     robot_voice_last_result());
            /* 通道没起来就顺手再启动一次：start 是幂等的，GOT_IP 那次可能已经失败。 */
            esp_err_t started = robot_voice_start();
            if (started != ESP_OK) {
                ESP_LOGW(TAG, "voice channel start retry failed: %s",
                         esp_err_to_name(started));
            }
            vTaskDelay(pdMS_TO_TICKS(kReadyPromptRetryMs));
        }
        if (!spoke) {
            ESP_LOGW(TAG, "cloud ready prompt unavailable; falling back to the cue");
        }
        /* 提示音里就含着唤醒词，别让它自己把机器人叫醒（合成结束时采集已被恢复）。 */
        robot_audio_set_capture_paused(true);
    }
#endif
    if (!spoke) {
        const char *asset = label[0] == '\0' ? "/assets/network_ready.wav"
                                             : "/assets/key_wakeup.wav";
        esp_err_t err = robot_audio_play_wav_file(asset);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ready prompt failed: %s", esp_err_to_name(err));
        }
    }
    /*
     * The microphone was muted for this prompt and nothing else lifts that:
     * resuming capture used to live in the error branch above, so a prompt that
     * played fine left the microphone paused until the first button-triggered
     * turn.  The wake engine was armed the whole time but starved of frames, so
     * the wake word stayed deaf after every reboot until BOOT was pressed once.
     *
     * Wait for the prompt to drain before resuming: unmuting straight away
     * would feed the tail of the prompt into recognition.
     */
    for (int step = 0; step < kPromptDrainWaitSteps && robot_audio_is_playing(); ++step) {
        vTaskDelay(pdMS_TO_TICKS(kPromptDrainPollMs));
    }
    robot_audio_set_capture_paused(false);
    ESP_LOGI(TAG, "ready prompt drained; microphone resumed for offline wake");
    s_ready_prompt_running.store(false);
    vTaskDelete(nullptr);
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
    }
    self->state_.store(RobotAppState::STOPPING);
    self->boot_running_.store(false);
    if (self->gesture_ready_) {
        robot_gesture_stop();
    }
    if (self->wake_ready_) {
        robot_wakeup_stop();
    }

    robot_network_stop();
    self->state_.store(RobotAppState::STOPPED);
    vTaskDelete(nullptr);
}

esp_err_t RobotApp::SetupOnlineServices()
{
    if (!robot_network_time_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * There is no local speech stack any more: the wake word engine is the
     * only thing to bring up here, and the console owns recognition, the
     * model call and synthesis from then on.
     */
    esp_err_t err = ESP_OK;

#if CONFIG_ROBOT_ENABLE_ESP_SR
    if (!wake_ready_) {
        ESP_LOGI(TAG, "[online 2/3] ESP-SR MultiNet custom wake");
        err = robot_wakeup_init(WakeCallback, this);
        wake_ready_ = err == ESP_OK;
        if (!wake_ready_) {
            ESP_LOGE(TAG, "offline wake unavailable: %s", esp_err_to_name(err));
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
    if (wake_ready_) {
        ESP_LOGI(TAG, "offline wake ready: %s or BOOT short press",
                 robot_wakeup_phrase());
    } else {
        /*
         * Without this the log looked healthy while the wake word could never
         * fire, and only the button appeared to work.
         */
        ESP_LOGW(TAG,
                 "offline wake UNAVAILABLE: only BOOT can start a turn "
                 "(wake engine failed to initialise - see robot_wakeup errors "
                 "above)");
    }
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
            ESP_LOGI(TAG, "provisioning requested from BOOT long press");
            robot_network_enter_provisioning();
            break;
        case EventType::WAKE_TRIGGER:
#if CONFIG_ROBOT_CLOUD_VOICE && CONFIG_ROBOT_CLOUD_VOICE_BARGE_IN
            /*
             * Barge-in: the wake word is also the interrupt.  Only a reply that
             * is actually playing needs interrupting - while the robot is
             * listening, everything said is already being recorded, and a
             * second trigger would only restart that recording.
             */
            if (state_.load() == RobotAppState::CLOUD_VOICE_ACTIVE) {
                if (robot_voice_busy() && robot_audio_is_playing()) {
                    ESP_LOGI(TAG, "wake word while speaking: interrupting");
                    robot_voice_interrupt_listen();
                }
                break;
            }
#endif
            if (state_.load() == RobotAppState::OFFLINE_WAKE_READY &&
                robot_network_time_ready()) {
#if CONFIG_ROBOT_CLOUD_VOICE
                /*
                 * A console-triggered turn already holds the microphone. Waking
                 * on top of it would start a second turn in the middle of a
                 * recording, so drop this wake instead.
                 */
                if (robot_voice_busy()) {
                    break;
                }
#endif
#if !(CONFIG_ROBOT_CLOUD_VOICE && CONFIG_ROBOT_CLOUD_VOICE_BARGE_IN)
                /* With barge-in the engine stays armed: that is what lets the
                 * next wake word interrupt the reply. */
                robot_wakeup_stop();
#endif
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
                 * a cloud turn when it
                 * cannot start keeps the robot usable if the console has not
                 * been given voice credentials yet.
                 */
                if (robot_voice_trigger() == ESP_OK) {
                    state_.store(RobotAppState::CLOUD_VOICE_ACTIVE);
                    break;
                }
#endif
                /* No local speech stack to fall back to: if the cloud turn
                 * could not start, re-arm the wake engine and stay ready. */
                {
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
#if CONFIG_ROBOT_CLOUD_VOICE_FOLLOW_UP
                /*
                 * Say the wake word once, then keep asking questions: after each
                 * reply the robot listens again instead of arming the wake word.
                 * The same number decides how long it waits for the first word
                 * of a turn, which is also what ends the conversation.
                 */
                voice_options.follow_up_ms =
                    (uint32_t)CONFIG_ROBOT_CLOUD_VOICE_FOLLOW_UP_MS;
#endif
                if (robot_voice_init(&voice_options) == ESP_OK) {
                    robot_voice_set_state_callback(CloudVoiceState, this);
                    /* start 可能因内存不足（10 KB 栈的任务）失败；静默失败会让
                     * 整个"会说话"能力消失，而日志里什么都看不到。 */
                    esp_err_t started = robot_voice_start();
                    if (started != ESP_OK) {
                        ESP_LOGW(TAG, "voice channel start failed: %s",
                                 esp_err_to_name(started));
                    }
                } else {
                    ESP_LOGW(TAG,
                             "voice channel init failed; the robot will not speak");
                }
            }
#endif
#endif
            break;
        case EventType::NETWORK_TIME_READY:
            if (!wake_ready_) {
                SetupOnlineServices();
            }
            break;
        case EventType::NETWORK_DISCONNECTED:
            /* A cloud turn has its own failure path. */
            ESP_LOGW(TAG, "network disconnected");
            break;
        case EventType::PLAYBACK_STARTED:
        case EventType::PLAYBACK_DRAINED:
            /* Only the wake chime and streamed answers use the playback
             * stream now; the module that plays them owns their lifetime. */
            break;
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
    /* Nothing to keep alive locally: a cloud turn owns its own lifetime. */
}












/*
 * Microphone level, throttled to roughly one line per second.
 *
 * This is logged while a cloud turn owns the microphone. At other
 * times nothing consumes the frames at all - the wake word
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
    bool wake_feed = state == RobotAppState::OFFLINE_WAKE_READY;
#if CONFIG_ROBOT_CLOUD_VOICE && CONFIG_ROBOT_CLOUD_VOICE_BARGE_IN
    /* The wake word doubles as the interrupt, so the engine keeps receiving the
     * microphone while a turn runs - it has to hear the phrase over the reply. */
    wake_feed = wake_feed || state == RobotAppState::CLOUD_VOICE_ACTIVE;
#endif
    if (wake_feed) {
        log_microphone_level(samples, sample_count);
        robot_wakeup_feed_pcm(samples, sample_count, capture_ms);
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
            /* Handled on the worker task: the turn and the wake engine are owned
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
