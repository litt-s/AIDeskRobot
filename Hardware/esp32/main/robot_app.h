#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "aiui_api.h"
#include "esp_audio_simple_player.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "robot_gesture.h"

enum class RobotAppState {
    NEW,
    STARTING,
    PROVISIONING,
    OFFLINE_WAKE_READY,
    SESSION_ACTIVE,
    /* A cloud voice turn owns the microphone. Kept distinct from
     * SESSION_ACTIVE so PcmConsumer neither feeds the wake word engine nor
     * enqueues microphone frames into the AIUI request that is not open. */
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
        AIUI_CONNECTED,
        AIUI_DISCONNECTED,
        AIUI_ERROR,
        AIUI_RESULT,
        AIUI_SPEECH_END,
        AIUI_SILENCE,
        PLAYBACK_STARTED,
        PLAYBACK_DRAINED,
        MEDIA_DECODER_DONE,
        MEDIA_RESOLVED,
        MEDIA_SEARCHED,
        MEDIA_ANNOUNCE,
        MEDIA_STOP_REQUEST,
        GESTURE,
        STOP,
        /* Posted from the voice task when a cloud voice turn starts and ends. */
        CLOUD_VOICE_STARTED,
        CLOUD_VOICE_FINISHED,
        /* Posted by the BOOT long press: open the setup page, or restart to
         * leave it.  Handled on the worker task, which owns the AIUI runtime. */
        PROVISION_REQUEST,
    };

    struct Event {
        EventType type;
        int code;
        int dts;
        robot_gesture_t gesture;
        char sub[24];
        char sid[64];
        char text[512];
        void *payload;
    };

    struct AudioFrame {
        size_t sample_count;
        int16_t samples[640];
    };

    struct AiuiRuntime {
        AIUI_HANDLE handle;
        QueueHandle_t audio_queue;
        TaskHandle_t audio_task;
        std::atomic<bool> active;
        std::atomic<bool> connected;
        std::atomic<bool> tts_playing;
        std::atomic<bool> tts_final_received;
        std::atomic<bool> stop_audio_task;
        std::atomic<uint32_t> captured_frames;
        std::atomic<uint32_t> queued_frames;
        std::atomic<uint32_t> sent_frames;
        std::atomic<uint32_t> dropped_frames;
        void *vad_handle;
        std::atomic<uint32_t> vad_speech_ms;
        std::atomic<uint32_t> vad_silence_ms;
        std::atomic<bool> utterance_active;
        std::atomic<bool> cloud_utterance_active;
        std::atomic<bool> audio_request_open;
        std::atomic<uint32_t> audio_gate_epoch;
        std::atomic<bool> awaiting_reply;
        std::atomic<uint32_t> awaiting_reply_since_ms;
        std::atomic<bool> turn_result_seen;
        std::atomic<bool> semantic_session_end_known;
        std::atomic<bool> semantic_should_end_session;
        std::atomic<bool> discard_tts_until_final;
        std::atomic<uint32_t> tts_chunk_count;
        std::atomic<uint32_t> tts_last_chunk_ms;
    };

    enum class PresentationKind {
        RANDOM,
        EXPRESSION,
        ACTION,
    };

    struct ReplyPresentation {
        PresentationKind kind;
        int value;
    };

    RobotApp() = default;
    RobotApp(const RobotApp &) = delete;
    RobotApp &operator=(const RobotApp &) = delete;

    esp_err_t PostEvent(const Event &event);
    esp_err_t SetupOnlineServices();
    void BeginIdleTimeoutExit();
    void StopSession(const char *reason);
    void EnterSleepState(const char *reason);
    void HandleEvent(const Event &event);
    void HandleResult(const Event &event);
    void ResetReplyPresentation();
    void AppendReplyText(const Event &event);
    void ProcessReplyExpressionSegments(bool flush_tail);
    void ApplyReplyExpression(int expression, const char *source);
    void QueueReplyPresentation(PresentationKind kind, int value);
    void DispatchReplyPresentation(uint64_t now_ms);
    void MaybeDispatchListeningAnimation(uint64_t now_ms);
    void SubmitGesture(robot_gesture_t gesture);
    void SubmitExpression(int expression);
    void TouchActivity();
    void ResumeListeningAfterNoResult(const char *reason);
    esp_err_t InitAiui();
    void DeinitAiui();
    esp_err_t StartAiui();
    void StopAiui();
    void FeedAiui(const int16_t *samples, size_t sample_count);
    void HandleTtsResult(const aiui_event_result_t &result);
    esp_err_t InitMediaPlayer();
    void DeinitMediaPlayer();
    void ParseMediaSemantic(const char *detail, const char *recognized_text,
                            const char *sid);
    bool StartMediaSearchFallback(const char *query, const char *sid);
    void HandleSearchedMedia(const Event &event);
    bool StartMediaAnnouncement();
    void HandleResolvedMedia(const Event &event);
    void StartPendingMedia();
    void FinishMediaPlayback(bool allow_next);
    void StopMediaPlayback();
    void ReleaseMediaPlayer();

    static void WorkerEntry(void *ctx);
    static void AudioSendEntry(void *ctx);
    static void BootEntry(void *ctx);
    static void PcmConsumer(void *ctx, const int16_t *samples,
                            size_t sample_count, uint64_t capture_ms);
    static void WakeCallback(void *ctx);
    static void PlaybackStarted(void *ctx, uint32_t generation);
    static void PlaybackDrained(void *ctx, uint32_t generation);
    static void NetworkEvent(void *ctx, int event_id);
    static void ProvisioningUi(void *ctx, const char *ssid, const char *url);
    static void GestureCallback(void *ctx, robot_gesture_t gesture);
    static void AiuiEvent(const void *ctx, aiui_callback_event_t *event);
    /* Bridges the console's speak command to the local speech stack; kept as a
     * callback so robot_cloud stays independent of the AIUI client. */
    static esp_err_t CloudSpeak(void *ctx, const char *text);
#if CONFIG_ROBOT_CLOUD_VOICE
    /* Bridges the voice task's start/finish notifications onto the event loop,
     * so the UI and the wake word engine are only touched from the app task. */
    static void CloudVoiceState(void *ctx, bool listening);
#endif
    static int MediaOutput(uint8_t *data, int data_size, void *ctx);
    static int MediaEvent(esp_asp_event_pkt_t *event, void *ctx);
    static void MediaSearchEntry(void *ctx);
    static void MediaResolveEntry(void *ctx);

    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    TaskHandle_t boot_task_ = nullptr;
    AiuiRuntime aiui_{};
    esp_asp_handle_t media_player_ = nullptr;
    void *media_playlist_ = nullptr;
    std::atomic<bool> media_active_{false};
    std::atomic<bool> media_decoder_done_{false};
    std::atomic<bool> media_pending_{false};
    std::atomic<bool> media_release_pending_{false};
    std::atomic<bool> media_search_running_{false};
    std::atomic<bool> media_announcement_pending_{false};
    std::atomic<bool> media_announcement_queued_{false};
    std::atomic<bool> media_resolve_running_{false};
    std::atomic<bool> suppress_music_failure_nlp_{false};
    std::atomic<bool> suppress_music_failure_tts_{false};
    char suppressed_music_sid_[64] = {};
    std::atomic<bool> session_exit_pending_{false};
    std::atomic<uint32_t> playback_generation_{0};
    bool media_advance_after_drain_ = false;
    bool media_paused_ = false;
    uint64_t media_queued_ms_ = 0;
    std::atomic<RobotAppState> state_{RobotAppState::NEW};
    std::atomic<bool> running_{false};
    std::atomic<bool> boot_running_{false};
    uint64_t last_activity_ms_ = 0;
    uint64_t aiui_retry_at_ms_ = 0;
    uint8_t aiui_retry_count_ = 0;
    char reply_sid_[64] = {};
    char reply_text_[1024] = {};
    size_t reply_expression_offset_ = 0;
    bool reply_playback_started_ = false;
    bool reply_expression_flush_pending_ = false;
    bool reply_pending_expression_valid_ = false;
    int reply_pending_expression_ = -1;
    int reply_expression_ = -1;
    ReplyPresentation reply_presentations_[8] = {};
    size_t reply_presentation_count_ = 0;
    uint64_t reply_next_presentation_ms_ = 0;
    uint64_t next_listening_animation_ms_ = 0;
    bool ui_ready_ = false;
    bool gesture_ready_ = false;
    bool wake_ready_ = false;
    bool network_ready_ = false;
};
