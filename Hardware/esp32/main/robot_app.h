#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "robot_gesture.h"

/*
 * The app used to run two speech stacks: the built-in AIUI assistant and the
 * console's cloud voice channel.  Only the cloud channel is left - the wake
 * word engine opens a turn, the server does recognition, the model call and
 * synthesis, and the reply streams back as PCM - so the states and events below
 * no longer carry any AIUI session.
 */
enum class RobotAppState {
    NEW,
    STARTING,
    PROVISIONING,
    OFFLINE_WAKE_READY,
    /* A cloud voice turn owns the microphone, so PcmConsumer feeds neither the
     * wake word engine nor anything else until the turn ends. */
    CLOUD_VOICE_ACTIVE,
    STOPPING,
    STOPPED,
    FAILED,
};

class RobotApp {
public:
    esp_err_t Init();
    esp_err_t Start();
    void Stop();
    void Deinit();

    static RobotApp &Instance();

private:
    enum class EventType {
        WAKE_TRIGGER,
        NETWORK_GOT_IP,
        NETWORK_TIME_READY,
        NETWORK_DISCONNECTED,
        PLAYBACK_STARTED,
        PLAYBACK_DRAINED,
        MEDIA_STOP_REQUEST,
        GESTURE,
        STOP,
        /* Posted from the voice task when a cloud voice turn starts and ends. */
        CLOUD_VOICE_STARTED,
        CLOUD_VOICE_FINISHED,
        /* Posted by the BOOT long press: open the setup page, or restart to
         * leave it. */
        PROVISION_REQUEST,
    };

    struct Event {
        EventType type;
        int code;
        robot_gesture_t gesture;
        void *payload;
    };

    RobotApp() = default;
    RobotApp(const RobotApp &) = delete;
    RobotApp &operator=(const RobotApp &) = delete;

    esp_err_t PostEvent(const Event &event);
    esp_err_t SetupOnlineServices();
    void HandleEvent(const Event &event);
    void SubmitGesture(robot_gesture_t gesture);
    void SubmitExpression(int expression);
    void TouchActivity();

    static void WorkerEntry(void *ctx);
    static void BootEntry(void *ctx);
    static void PcmConsumer(void *ctx, const int16_t *samples,
                            size_t sample_count, uint64_t capture_ms);
    static void WakeCallback(void *ctx);
    static void PlaybackStarted(void *ctx, uint32_t generation);
    static void PlaybackDrained(void *ctx, uint32_t generation);
    static void NetworkEvent(void *ctx, int event_id);
    static void ProvisioningUi(void *ctx, const char *ssid, const char *url);
    static void GestureCallback(void *ctx, robot_gesture_t gesture);

#if CONFIG_ROBOT_CLOUD_VOICE
    /* Bridges the voice task's start/finish notifications onto the event loop,
     * so the UI and the wake word engine are only touched from the app task. */
    static void CloudVoiceState(void *ctx, bool listening);
#endif

    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    TaskHandle_t boot_task_ = nullptr;
    std::atomic<uint32_t> playback_generation_{0};
    std::atomic<RobotAppState> state_{RobotAppState::NEW};
    std::atomic<bool> running_{false};
    std::atomic<bool> boot_running_{false};
    bool ui_ready_ = false;
    bool gesture_ready_ = false;
    bool wake_ready_ = false;
    bool network_ready_ = false;
};