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
#include "robot_music.h"
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
constexpr size_t kMaxMediaTracks = 10;
constexpr size_t kMaxMediaUrlLength = 2048;
constexpr size_t kMaxMediaTitleLength = 160;
constexpr size_t kMaxMediaItemIdLength = 96;
constexpr size_t kMaxMediaQueryLength = 192;
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

struct MediaTrack {
    char url[kMaxMediaUrlLength];
    char title[kMaxMediaTitleLength];
    char item_id[kMaxMediaItemIdLength];
};

struct MediaPlaylist {
    size_t count;
    size_t index;
    MediaTrack tracks[kMaxMediaTracks];
};

struct MediaResolveJob {
    RobotApp *app;
    size_t track_index;
    char item_id[kMaxMediaItemIdLength];
};

struct MediaSearchJob {
    RobotApp *app;
    char query[kMaxMediaQueryLength];
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

void normalize_music_query(const char *text, char *query,
                           size_t query_size)
{
    if (!query || query_size == 0) {
        return;
    }
    query[0] = '\0';
    if (!text) {
        return;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    static const char *const prefixes[] = {
        "请帮我播放", "给我播放", "播放一下", "请播放", "播放",
        "我想听", "我要听", "想听", "来一首", "放一首", "放一下",
        "听一下",
    };
    for (const char *prefix : prefixes) {
        const size_t length = strlen(prefix);
        if (strncmp(text, prefix, length) == 0) {
            text += length;
            break;
        }
    }
    while (*text == ' ' || *text == '\t' || *text == ':' || *text == '\xef') {
        /* The 0xEF case is harmless for normal UTF-8 and avoids keeping a BOM. */
        if ((unsigned char)*text == 0xef &&
            (unsigned char)text[1] == 0xbb &&
            (unsigned char)text[2] == 0xbf) {
            text += 3;
        } else if (*text == ' ' || *text == '\t' || *text == ':') {
            ++text;
        } else {
            break;
        }
    }
    strlcpy(query, text, query_size);
    size_t length = strlen(query);
    while (length > 0 &&
           (query[length - 1] == ' ' || query[length - 1] == '\t' ||
            query[length - 1] == '?' || query[length - 1] == '!')) {
        query[--length] = '\0';
    }
}

bool is_exit_command(const char *text)
{
    if (!text || text[0] == '\0') {
        return false;
    }
    static const char *const commands[] = {
        "\u4f60\u9000\u4e0b\u5427",  // 你退下吧
        "\u9000\u4e0b\u5427",        // 退下吧
        "\u9000\u4e0b",              // 退下
        "\u4f60\u4f11\u606f\u5427",  // 你休息吧
        "\u53bb\u4f11\u606f\u5427",  // 去休息吧
        "\u4f11\u606f\u5427",        // 休息吧
        "\u9000\u51fa\u5427",        // 退出吧
        "\u9000\u51fa",              // 退出
        "\u95ed\u5634",              // 闭嘴
        "\u518d\u89c1",              // 再见
        "\u62dc\u62dc",              // 拜拜
        "\u7ed3\u675f",              // 结束
    };
    for (const char *command : commands) {
        if (strncmp(text, command, strlen(command)) == 0) {
            return true;
        }
    }
    return false;
}

bool is_media_service(const cJSON *root)
{
    const char *service = json_string(root, "service");
    const char *service_pkg = json_string(root, "service_pkg");
    const char *candidate = service ? service : service_pkg;
    return candidate &&
           (strcmp(candidate, "musicX") == 0 ||
            strcmp(candidate, "media") == 0 ||
            strcmp(candidate, "drama") == 0);
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

bool media_item_id_from_item(const cJSON *item, char *value,
                             size_t value_size)
{
    if (!item || !value || value_size == 0) {
        return false;
    }
    static const char *const keys[] = {
        "itemid", "itemId", "songId", "song_id",
    };
    for (const char *key : keys) {
        const cJSON *id =
            cJSON_GetObjectItemCaseSensitive(item, key);
        if (cJSON_IsString(id) && id->valuestring &&
            id->valuestring[0] != '\0') {
            return strlcpy(value, id->valuestring, value_size) <
                   value_size;
        }
        if (cJSON_IsNumber(id)) {
            int length = snprintf(value, value_size, "%.0f",
                                  id->valuedouble);
            return length > 0 && length < (int)value_size;
        }
    }
    /* Some musicX response variants use id on the song object. */
    const bool looks_like_song =
        json_string(item, "name") || json_string(item, "itemname") ||
        json_string(item, "neatsongname") ||
        json_string(item, "songname") || json_string(item, "title");
    if (looks_like_song) {
        const cJSON *id =
            cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(id) && id->valuestring &&
            id->valuestring[0] != '\0') {
            return strlcpy(value, id->valuestring, value_size) <
                   value_size;
        }
        if (cJSON_IsNumber(id)) {
            int length = snprintf(value, value_size, "%.0f",
                                  id->valuedouble);
            return length > 0 && length < (int)value_size;
        }
    }
    return false;
}

bool media_track_exists(const MediaPlaylist *playlist, const char *url,
                        const char *item_id)
{
    if (!playlist) {
        return false;
    }
    for (size_t i = 0; i < playlist->count; ++i) {
        const MediaTrack &track = playlist->tracks[i];
        if (url && url[0] && track.url[0] &&
            strcmp(track.url, url) == 0) {
            return true;
        }
        if (item_id && item_id[0] && track.item_id[0] &&
            strcmp(track.item_id, item_id) == 0) {
            return true;
        }
    }
    return false;
}

void media_collect_tracks(const cJSON *node, MediaPlaylist *playlist,
                          unsigned depth)
{
    if (!node || !playlist || depth == 0 ||
        playlist->count >= kMaxMediaTracks) {
        return;
    }
    if (cJSON_IsObject(node)) {
        const char *url = media_url_from_item(node);
        char item_id[kMaxMediaItemIdLength] = {};
        const bool has_item_id = media_item_id_from_item(
            node, item_id, sizeof(item_id));
        if ((url || has_item_id) &&
            !media_track_exists(playlist, url, item_id)) {
            MediaTrack &track = playlist->tracks[playlist->count++];
            if (url) {
                strlcpy(track.url, url, sizeof(track.url));
            }
            if (has_item_id) {
                strlcpy(track.item_id, item_id, sizeof(track.item_id));
            }
            strlcpy(track.title, media_title_from_item(node),
                    sizeof(track.title));
        }
    }

    const cJSON *child = nullptr;
    cJSON_ArrayForEach(child, node) {
        if (playlist->count >= kMaxMediaTracks) {
            break;
        }
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            media_collect_tracks(child, playlist, depth - 1);
        } else if (cJSON_IsString(child) && child->valuestring &&
                   (child->valuestring[0] == '{' ||
                    child->valuestring[0] == '[')) {
            cJSON *embedded = cJSON_Parse(child->valuestring);
            if (embedded) {
                media_collect_tracks(embedded, playlist, depth - 1);
                cJSON_Delete(embedded);
            }
        }
    }
}

void log_media_result_shape(const cJSON *root, const cJSON *results)
{
    const char *service = json_string(root, "service");
    if (!service) {
        service = json_string(root, "service_pkg");
    }
    const cJSON *rc_item =
        cJSON_GetObjectItemCaseSensitive(root, "rc");
    const int rc = cJSON_IsNumber(rc_item) ? rc_item->valueint : -1;
    const cJSON *first = cJSON_IsArray(results)
                             ? cJSON_GetArrayItem(results, 0)
                             : nullptr;
    char keys[256] = {};
    size_t used = 0;
    if (cJSON_IsObject(first)) {
        const cJSON *field = nullptr;
        cJSON_ArrayForEach(field, first) {
            if (!field->string) {
                continue;
            }
            int written = snprintf(keys + used, sizeof(keys) - used,
                                   "%s%s", used ? "," : "",
                                   field->string);
            if (written <= 0 || written >=
                (int)(sizeof(keys) - used)) {
                break;
            }
            used += static_cast<size_t>(written);
        }
    } else if (cJSON_IsString(first)) {
        strlcpy(keys, "<json-string>", sizeof(keys));
    } else {
        strlcpy(keys, "<non-object>", sizeof(keys));
    }
    ESP_LOGI(TAG,
             "AIUI result shape: service=%s rc=%d count=%d first_keys=%s",
             service ? service : "<none>", rc,
             cJSON_IsArray(results) ? cJSON_GetArraySize(results) : -1,
             keys[0] ? keys : "<empty>");
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

bool is_safe_credential(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
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

bool expression_from_tag(const char *text, robot_expression_t *expression)
{
    if (!text || !expression || text[0] == '\0') {
        return false;
    }
    const robot_expression_t parsed = expression_from_text(text);
    if (parsed != ROBOT_EXPRESSION_NEUTRAL) {
        *expression = parsed;
        return true;
    }
    if (text_has_any(text, {"neutral", "normal", "中性", "平静"})) {
        *expression = ROBOT_EXPRESSION_NEUTRAL;
        return true;
    }
    if (text_has_any(text, {"sleepy", "sleeping", "sleep", "困倦"})) {
        *expression = ROBOT_EXPRESSION_SLEEP;
        return true;
    }
    return false;
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

bool reply_segment_boundary(const char *text, size_t remaining,
                            size_t *boundary_bytes)
{
    if (!text || remaining == 0 || !boundary_bytes) {
        return false;
    }
    if (text[0] == ',' || text[0] == '.' || text[0] == '!' ||
        text[0] == '?' || text[0] == ';') {
        *boundary_bytes = 1;
        return true;
    }
    static const char *const punctuation[] = {
        "，", "。", "！", "？", "；",
    };
    for (const char *mark : punctuation) {
        const size_t length = strlen(mark);
        if (remaining >= length && memcmp(text, mark, length) == 0) {
            *boundary_bytes = length;
            return true;
        }
    }
    return false;
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

bool action_from_text(const char *text, bool infer_response,
                      robot_action_type_t *type)
{
    if (!text || !type) {
        return false;
    }
    if (text_has_any(text, {"眨眼", "眨一下", "blink"})) {
        *type = ROBOT_ACTION_BLINK;
    } else if (text_has_any(text, {"向左看", "看左边", "左转", "左看"})) {
        *type = ROBOT_ACTION_LOOK_LEFT;
    } else if (text_has_any(text, {"向右看", "看右边", "右转", "右看"})) {
        *type = ROBOT_ACTION_LOOK_RIGHT;
    } else if (text_has_any(text, {"抬头", "向上看", "看上面"})) {
        *type = ROBOT_ACTION_LOOK_UP;
    } else if (text_has_any(text, {"低头", "向下看", "看下面"})) {
        *type = ROBOT_ACTION_LOOK_DOWN;
    } else if (text_has_any(text, {"回正", "居中", "回到中间"})) {
        *type = ROBOT_ACTION_CENTER;
    } else if (text_has_any(text, {"点头", "nod"})) {
        *type = ROBOT_ACTION_NOD;
    } else if (text_has_any(text, {"摇头", "shake"})) {
        *type = ROBOT_ACTION_SHAKE;
    } else if (text_has_any(text, {"转圈", "转一圈", "绕圈", "spin"})) {
        *type = ROBOT_ACTION_ROLL;
    } else if (text_has_any(text, {"跳舞", "舞蹈", "跳个舞", "dance"})) {
        *type = ROBOT_ACTION_DANCE;
    } else if (text_has_any(text, {"睡觉", "睡眠", "晚安"})) {
        *type = ROBOT_ACTION_SLEEP;
    } else if (text_has_any(text, {"醒醒", "起床", "唤醒"})) {
        *type = ROBOT_ACTION_WAKEUP;
    } else if (infer_response &&
               text_has_any(text, {"是的", "对的", "当然", "没问题", "可以的"})) {
        *type = ROBOT_ACTION_NOD;
    } else if (infer_response &&
               text_has_any(text, {"不是", "不对", "不行", "不能", "不可以"})) {
        *type = ROBOT_ACTION_SHAKE;
    } else {
        return false;
    }
    return true;
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
    DeinitMediaPlayer();
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
            if (self->media_pending_ && !self->media_active_.load() &&
                !self->media_announcement_pending_.load() &&
                !self->aiui_.tts_playing.load() &&
                !robot_audio_is_playing() &&
                now - self->media_queued_ms_ >= kMediaStartFallbackMs) {
                self->StartPendingMedia();
            }
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
                (!self->media_active_.load() || self->media_paused_) &&
                !self->media_pending_ &&
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
    self->StopMediaPlayback();
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
            if (media_active_.load()) {
                if (media_decoder_done_.load()) {
                    FinishMediaPlayback(media_advance_after_drain_);
                } else {
                    ESP_LOGW(TAG,
                             "speaker drained before media decoder completed");
                }
                break;
            }
            if (media_announcement_pending_.exchange(false)) {
                media_announcement_queued_.store(false);
                ESP_LOGI(TAG,
                         "media announcement complete; starting music");
            }
            if (media_pending_) {
                StartPendingMedia();
                break;
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
        case EventType::MEDIA_DECODER_DONE:
            if (media_release_pending_.load()) {
                ReleaseMediaPlayer();
                if (!media_player_ && media_pending_.load() &&
                    !media_announcement_pending_.load()) {
                    StartPendingMedia();
                }
                break;
            }
            if (!media_active_.load()) {
                break;
            }
            media_decoder_done_.store(true);
            media_advance_after_drain_ =
                event.code == ESP_ASP_STATE_FINISHED ||
                event.code == ESP_ASP_STATE_ERROR;
            if (robot_audio_finish_playback() != ESP_OK) {
                ESP_LOGE(TAG, "unable to drain decoded media audio");
                FinishMediaPlayback(media_advance_after_drain_);
            }
            break;
        case EventType::MEDIA_RESOLVED:
            HandleResolvedMedia(event);
            break;
        case EventType::MEDIA_SEARCHED:
            HandleSearchedMedia(event);
            break;
        case EventType::MEDIA_ANNOUNCE:
            StartMediaAnnouncement();
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
            if (session_exit_pending_.exchange(false)) {
                robot_audio_abort_playback();
                StopSession("idle timeout prompt interrupted");
                break;
            }
            if (media_active_.load() || media_pending_ ||
                aiui_.tts_playing.load() || robot_audio_is_playing()) {
                const bool interrupted_tts = aiui_.tts_playing.load();
                const bool wait_for_tts_final =
                    interrupted_tts &&
                    !aiui_.tts_final_received.load();
                robot_audio_set_capture_paused(true);
                StopMediaPlayback();
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

void RobotApp::HandleResult(const Event &event)
{
    TouchActivity();
    if (strcmp(event.sub, "iat") == 0 && event.dts >= 2) {
        ESP_LOGI(TAG, "AIUI recognized: %.200s",
                 event.text[0] != '\0' ? event.text : "<empty>");
        /* The recognition result starts a new response turn. Do not let the
           previous TTS playback state release this turn's expressions early. */
        ResetReplyPresentation();
        if (is_exit_command(event.text)) {
            EnterSleepState("voice exit guidance");
            return;
        }
        robot_action_t action = {};
        if (action_from_text(event.text, false, &action.type)) {
            ESP_LOGI(TAG, "voice action detected: %d", (int)action.type);
            if (action.type == ROBOT_ACTION_SLEEP) {
                /*
                 * Sleeping is also a session boundary.  Leaving AIUI active
                 * keeps the offline wake detector stopped, so the display
                 * would look asleep but "小飞小飞" could never wake it.
                 */
                EnterSleepState("voice sleep command");
                return;
            }
            robot_action_queue_submit(&action, 0);
        }
        robot_expression_t requested_expression =
            expression_from_text(event.text);
        if (requested_expression != ROBOT_EXPRESSION_NEUTRAL) {
            SubmitExpression(requested_expression);
        }
        return;
    }
    if (strcmp(event.sub, "cbm_semantic") == 0 && event.dts >= 2) {
        /*
         * In the v3 full-chain workflow this field is semantic metadata and
         * may contain the user's query rather than the assistant answer.
         * The audio request already includes parameter.tts, so requesting
         * another TTS here creates a second stream and interleaves both PCM
         * sequences in the same speaker queue.
         */
        ESP_LOGI(TAG, "AIUI semantic metadata: %.200s",
                 event.text[0] != '\0' ? event.text : "<empty>");
        /*
         * A rapid-interaction turn can occasionally omit the final IAT event
         * while still returning the recognized query in cbm_semantic.  Exit
         * guidance is a local device command, so recognize it here as a
         * fallback instead of allowing the cloud farewell/TTS to keep the
         * dialogue session alive.
         */
        if (is_exit_command(event.text)) {
            EnterSleepState("semantic exit guidance");
            return;
        }
        /*
         * The 7911 skill parser defaults each semantic response to session
         * end and changes it only when shouldEndSession is explicitly false.
         */
        aiui_.semantic_session_end_known.store(true);
        aiui_.semantic_should_end_session.store(true);
        if (event.payload) {
            ParseMediaSemantic(static_cast<const char *>(event.payload),
                               event.text, event.sid);
        } else {
            ESP_LOGW(TAG,
                     "cbm_semantic detail unavailable; defaulting "
                     "shouldEndSession=true");
        }
        return;
    }
    if (strcmp(event.sub, "cbm_facial_expression") == 0) {
#if CONFIG_ROBOT_ENABLE_EXPRESSION_TAG
        if (event.sid[0] != '\0' && reply_sid_[0] != '\0' &&
            strcmp(event.sid, reply_sid_) != 0) {
            ResetReplyPresentation();
        }
        if (event.sid[0] != '\0' && reply_sid_[0] == '\0') {
            strlcpy(reply_sid_, event.sid, sizeof(reply_sid_));
        }
        if (text_has_any(event.text, {"winking", "wink", "眨眼"})) {
            ESP_LOGI(TAG,
                     "AI reply action scheduled: blink source=AIUI facial tag");
            QueueReplyPresentation(PresentationKind::ACTION,
                                   ROBOT_ACTION_BLINK);
            return;
        }
        robot_expression_t expression = ROBOT_EXPRESSION_NEUTRAL;
        if (expression_from_tag(event.text, &expression)) {
            ApplyReplyExpression(expression, "AIUI facial tag");
        } else {
            ESP_LOGW(TAG, "unrecognized AIUI facial expression: %.160s",
                     event.text[0] ? event.text : "<empty>");
        }
#endif
        return;
    }
    if (strcmp(event.sub, "nlp") == 0) {
        if (suppress_music_failure_nlp_.load() && event.sid[0] != '\0' &&
            strcmp(event.sid, suppressed_music_sid_) == 0) {
            if (event.dts >= 2) {
                suppress_music_failure_nlp_.store(false);
                ESP_LOGI(TAG,
                         "AIUI music failure answer suppressed: %.200s",
                         event.text[0] != '\0' ? event.text : "<empty>");
            }
            return;
        }
        AppendReplyText(event);
        ProcessReplyExpressionSegments(event.dts >= 2);

        if (event.dts >= 2) {
            const char *answer = reply_text_[0] ? reply_text_ : event.text;
            ESP_LOGI(TAG, "AIUI answer complete: %.480s",
                     answer[0] != '\0' ? answer : "<empty>");
            robot_action_t action = {};
            if (action_from_text(answer, true, &action.type)) {
                ESP_LOGI(TAG, "AI reply action scheduled: %d",
                         (int)action.type);
                QueueReplyPresentation(PresentationKind::ACTION,
                                       (int)action.type);
            }
        }
    }
}

void RobotApp::ResetReplyPresentation()
{
    reply_sid_[0] = '\0';
    reply_text_[0] = '\0';
    reply_expression_offset_ = 0;
    reply_playback_started_ = false;
    reply_expression_flush_pending_ = false;
    reply_pending_expression_valid_ = false;
    reply_pending_expression_ = -1;
    reply_expression_ = -1;
    reply_presentation_count_ = 0;
    reply_next_presentation_ms_ = 0;
}

void RobotApp::AppendReplyText(const Event &event)
{
    if (event.sid[0] != '\0' && reply_sid_[0] != '\0' &&
        strcmp(event.sid, reply_sid_) != 0) {
        ResetReplyPresentation();
    }
    if (event.sid[0] != '\0' && reply_sid_[0] == '\0') {
        strlcpy(reply_sid_, event.sid, sizeof(reply_sid_));
    }
    if (event.text[0] == '\0') {
        return;
    }

    const size_t used = strlen(reply_text_);
    const size_t chunk_length = strlen(event.text);
    if (used == 0) {
        strlcpy(reply_text_, event.text, sizeof(reply_text_));
        return;
    }
    /* Accept both AIUI streaming forms: delta chunks and cumulative chunks. */
    if (chunk_length >= used &&
        strncmp(event.text, reply_text_, used) == 0) {
        strlcpy(reply_text_, event.text, sizeof(reply_text_));
        return;
    }
    if (chunk_length <= used &&
        strcmp(reply_text_ + used - chunk_length, event.text) == 0) {
        return;
    }
    strlcat(reply_text_, event.text, sizeof(reply_text_));
}

void RobotApp::ProcessReplyExpressionSegments(bool flush_tail)
{
    if (!reply_playback_started_) {
        reply_expression_flush_pending_ |= flush_tail;
        return;
    }
    const size_t text_length = strlen(reply_text_);
    while (reply_expression_offset_ < text_length) {
        size_t end = reply_expression_offset_;
        size_t boundary_bytes = 0;
        while (end < text_length &&
               !reply_segment_boundary(reply_text_ + end,
                                       text_length - end,
                                       &boundary_bytes)) {
            ++end;
        }
        if (end == text_length) {
            if (!flush_tail) {
                return;
            }
            boundary_bytes = 0;
        }

        const size_t segment_end = end + boundary_bytes;
        const size_t segment_length =
            segment_end - reply_expression_offset_;
        char segment[256] = {};
        const size_t copy_length =
            std::min(segment_length, sizeof(segment) - 1);
        memcpy(segment, reply_text_ + reply_expression_offset_, copy_length);
        segment[copy_length] = '\0';
        reply_expression_offset_ = segment_end;

        bool has_content = false;
        for (size_t i = 0; i < copy_length; ++i) {
            const unsigned char ch = (unsigned char)segment[i];
            if (ch >= 0x80 || std::isalnum(ch)) {
                has_content = true;
                break;
            }
        }
        if (!has_content) {
            continue;
        }
        const robot_expression_t expression =
            reply_expression_from_text(segment);
        ESP_LOGI(TAG, "AI reply segment: %.120s", segment);
        if (expression == ROBOT_EXPRESSION_NEUTRAL) {
            QueueReplyPresentation(PresentationKind::RANDOM,
                                   ROBOT_EXPRESSION_NEUTRAL);
        } else {
            ApplyReplyExpression(expression, "NLP segment");
        }
    }
}

void RobotApp::ApplyReplyExpression(int expression, const char *source)
{
    if (!reply_playback_started_) {
        reply_pending_expression_ = expression;
        reply_pending_expression_valid_ = true;
        ESP_LOGI(TAG, "AI reply expression queued until playback: %s",
                 expression_name(static_cast<robot_expression_t>(expression)));
        return;
    }
    const robot_expression_t selected =
        static_cast<robot_expression_t>(expression);
    ESP_LOGI(TAG, "AI reply expression scheduled: %s source=%s",
             expression_name(selected), source ? source : "unknown");
    QueueReplyPresentation(PresentationKind::EXPRESSION, expression);
}

void RobotApp::QueueReplyPresentation(PresentationKind kind, int value)
{
    constexpr size_t capacity =
        sizeof(reply_presentations_) / sizeof(reply_presentations_[0]);

    if (kind == PresentationKind::RANDOM) {
        size_t pending_random = 0;
        for (size_t i = 0; i < reply_presentation_count_; ++i) {
            if (reply_presentations_[i].kind == PresentationKind::RANDOM) {
                ++pending_random;
            }
        }
        if (pending_random >= kMaxPendingRandomPresentations) {
            return;
        }
    } else if (kind == PresentationKind::EXPRESSION) {
        for (size_t i = 0; i < reply_presentation_count_; ++i) {
            if (reply_presentations_[i].kind ==
                    PresentationKind::EXPRESSION &&
                reply_presentations_[i].value == value) {
                return;
            }
        }
        if (reply_presentation_count_ == 0 && reply_expression_ == value) {
            return;
        }
    }

    if (reply_presentation_count_ >= capacity) {
        if (kind == PresentationKind::RANDOM) {
            return;
        }
        /* Keep an explicit emotion by replacing the newest random gesture. */
        size_t replace = capacity;
        for (size_t i = reply_presentation_count_; i > 0; --i) {
            if (reply_presentations_[i - 1].kind ==
                PresentationKind::RANDOM) {
                replace = i - 1;
                break;
            }
        }
        if (replace == capacity) {
            ESP_LOGW(TAG, "reply presentation queue full");
            return;
        }
        for (size_t i = replace + 1; i < reply_presentation_count_; ++i) {
            reply_presentations_[i - 1] = reply_presentations_[i];
        }
        --reply_presentation_count_;
    }

    size_t insert_at = reply_presentation_count_;
    if (kind != PresentationKind::RANDOM) {
        for (size_t i = 0; i < reply_presentation_count_; ++i) {
            if (reply_presentations_[i].kind ==
                PresentationKind::RANDOM) {
                insert_at = i;
                break;
            }
        }
    }
    for (size_t i = reply_presentation_count_; i > insert_at; --i) {
        reply_presentations_[i] = reply_presentations_[i - 1];
    }
    reply_presentations_[insert_at] = {kind, value};
    ++reply_presentation_count_;
}

void RobotApp::DispatchReplyPresentation(uint64_t now_ms)
{
    if (!reply_playback_started_ || reply_presentation_count_ == 0 ||
        !aiui_.tts_playing.load() || !robot_audio_is_playing() ||
        (reply_next_presentation_ms_ != 0 &&
         now_ms < reply_next_presentation_ms_)) {
        return;
    }

    const ReplyPresentation presentation = reply_presentations_[0];
    for (size_t i = 1; i < reply_presentation_count_; ++i) {
        reply_presentations_[i - 1] = reply_presentations_[i];
    }
    --reply_presentation_count_;

    if (presentation.kind == PresentationKind::RANDOM) {
        robot_action_t action = {};
        action.type = ROBOT_ACTION_RANDOM;
        ESP_LOGI(TAG, "AI reply presentation: original random action");
        robot_action_queue_submit(&action, 0);
    } else if (presentation.kind == PresentationKind::EXPRESSION) {
        const robot_expression_t selected =
            static_cast<robot_expression_t>(presentation.value);
        reply_expression_ = presentation.value;
        ESP_LOGI(TAG, "AI reply presentation: %s",
                 expression_name(selected));
        SubmitExpression(selected);
    } else {
        robot_action_t action = {};
        action.type =
            static_cast<robot_action_type_t>(presentation.value);
        ESP_LOGI(TAG, "AI reply presentation action: %d",
                 presentation.value);
        robot_action_queue_submit(&action, 0);
    }

    reply_next_presentation_ms_ =
        now_ms + kReplyPresentationMinIntervalMs +
        (esp_random() % kReplyPresentationJitterMs);
}

void RobotApp::MaybeDispatchListeningAnimation(uint64_t now_ms)
{
    if (next_listening_animation_ms_ == 0) {
        next_listening_animation_ms_ =
            now_ms + kListeningAnimationMinIntervalMs +
            (esp_random() % kListeningAnimationJitterMs);
        return;
    }
    if (now_ms < next_listening_animation_ms_) {
        return;
    }

    const bool busy =
        !aiui_.active.load() || !aiui_.connected.load() ||
        session_exit_pending_.load() || aiui_.tts_playing.load() ||
        aiui_.awaiting_reply.load() || aiui_.utterance_active.load() ||
        aiui_.cloud_utterance_active.load() || media_active_.load() ||
        media_pending_.load() || media_announcement_pending_.load() ||
        robot_audio_is_playing();
    if (busy) {
        next_listening_animation_ms_ = now_ms + kPresentationBusyRetryMs;
        return;
    }

    robot_action_t action = {};
    action.type = ROBOT_ACTION_RANDOM;
    if (robot_action_queue_submit(&action, 0) == ESP_OK) {
        ESP_LOGI(TAG, "listening idle animation: original random action");
    }
    /* Deliberately do not TouchActivity(): this must not prevent sleep. */
    next_listening_animation_ms_ =
        now_ms + kListeningAnimationMinIntervalMs +
        (esp_random() % kListeningAnimationJitterMs);
}

esp_err_t RobotApp::InitMediaPlayer()
{
    if (media_player_) {
        return ESP_OK;
    }
    esp_asp_cfg_t config = {};
    config.out.cb = MediaOutput;
    config.out.user_ctx = this;
    config.task_prio = 5;
    config.task_stack = kMediaPlayerTaskStackBytes;
    config.task_core = 1;
    esp_gmf_err_t err =
        esp_audio_simple_player_new(&config, &media_player_);
    if (err != ESP_GMF_ERR_OK) {
        media_player_ = nullptr;
        ESP_LOGE(TAG, "unable to create media player: 0x%x", (unsigned)err);
        return ESP_FAIL;
    }
    err = esp_audio_simple_player_set_event(
        media_player_, MediaEvent, this);
    if (err != ESP_GMF_ERR_OK) {
        esp_audio_simple_player_destroy(media_player_);
        media_player_ = nullptr;
        ESP_LOGE(TAG, "unable to register media events: 0x%x",
                 (unsigned)err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void RobotApp::DeinitMediaPlayer()
{
    StopMediaPlayback();
    ReleaseMediaPlayer();
    if (media_playlist_) {
        heap_caps_free(media_playlist_);
        media_playlist_ = nullptr;
    }
}

void RobotApp::ParseMediaSemantic(const char *detail,
                                  const char *recognized_text,
                                  const char *sid)
{
    if (!detail || detail[0] == '\0') {
        return;
    }
    cJSON *container = cJSON_Parse(detail);
    if (!container) {
        ESP_LOGW(TAG, "unable to parse cbm_semantic detail");
        return;
    }
    const char *semantic_text = json_string(container, "text");
    cJSON *semantic = semantic_text ? cJSON_Parse(semantic_text) : nullptr;
    const cJSON *root = semantic ? semantic : container;
    char fallback_query[kMaxMediaQueryLength];
    const char *query_text = json_string(root, "text");
    normalize_music_query(query_text ? query_text : recognized_text,
                          fallback_query, sizeof(fallback_query));

    bool should_end_session = true;
    const cJSON *session_end =
        json_find_key(root, "shouldEndSession", 5);
    if (session_end && !json_bool(session_end, &should_end_session)) {
        ESP_LOGW(TAG,
                 "invalid shouldEndSession type; defaulting to true");
        should_end_session = true;
    }
    aiui_.semantic_session_end_known.store(true);
    aiui_.semantic_should_end_session.store(should_end_session);
    ESP_LOGI(TAG, "AIUI shouldEndSession=%s%s",
             should_end_session ? "true" : "false",
             session_end ? "" : " (default)");

    const cJSON *semantic_items =
        cJSON_GetObjectItemCaseSensitive(root, "semantic");
    const cJSON *semantic_item = cJSON_IsArray(semantic_items)
        ? cJSON_GetArrayItem(semantic_items, 0)
        : nullptr;
    const char *intent = cJSON_IsObject(semantic_item)
        ? json_string(semantic_item, "intent")
        : nullptr;
    if (intent && strcmp(intent, "INSTRUCTION") == 0) {
        const cJSON *slots =
            cJSON_GetObjectItemCaseSensitive(semantic_item, "slots");
        const char *instruction = nullptr;
        if (cJSON_IsArray(slots)) {
            const int slot_count = cJSON_GetArraySize(slots);
            for (int i = 0; i < slot_count; ++i) {
                const cJSON *slot = cJSON_GetArrayItem(slots, i);
                if (slot && json_string(slot, "name") &&
                    strcmp(json_string(slot, "name"), "insType") == 0) {
                    instruction = json_string(slot, "value");
                    break;
                }
            }
        }
        if (instruction) {
            auto *playlist =
                static_cast<MediaPlaylist *>(media_playlist_);
            ESP_LOGI(TAG, "AIUI media instruction: %s", instruction);
            if (strcmp(instruction, "pause") == 0 ||
                strcmp(instruction, "PAUSE") == 0 ||
                strcmp(instruction, "SHUTDOWN") == 0 ||
                strcmp(instruction, "EXIT") == 0 ||
                strcmp(instruction, "MUTE") == 0) {
                if (media_player_ && media_active_.load() &&
                    esp_audio_simple_player_pause(media_player_) ==
                        ESP_GMF_ERR_OK) {
                    robot_audio_abort_playback();
                    media_paused_ = true;
                    robot_audio_set_capture_paused(false);
                }
            } else if (strcmp(instruction, "replay") == 0 ||
                       strcmp(instruction, "RESUME_PLAY") == 0) {
                if (media_player_ && media_paused_ &&
                    esp_audio_simple_player_resume(media_player_) ==
                        ESP_GMF_ERR_OK) {
                    media_paused_ = false;
                    robot_audio_set_capture_paused(true);
                }
            } else if (strcmp(instruction, "next") == 0 ||
                       strcmp(instruction, "CHOOSE_NEXT") == 0 ||
                       strcmp(instruction, "past") == 0 ||
                       strcmp(instruction, "CHOOSE_PREVIOUS") == 0) {
                if (playlist && playlist->count > 0) {
                    if (media_player_ && media_active_.load()) {
                        esp_audio_simple_player_stop(media_player_);
                    }
                    media_active_.store(false);
                    media_decoder_done_.store(false);
                    media_paused_ = false;
                    robot_audio_abort_playback();
                    const bool previous =
                        strcmp(instruction, "past") == 0 ||
                        strcmp(instruction, "CHOOSE_PREVIOUS") == 0;
                    if (previous) {
                        playlist->index =
                            playlist->index > 0 ? playlist->index - 1 : 0;
                    } else if (playlist->index + 1 < playlist->count) {
                        ++playlist->index;
                    }
                    media_pending_ = true;
                    media_queued_ms_ =
                        (uint64_t)(esp_timer_get_time() / 1000);
                    robot_audio_set_capture_paused(true);
                }
            }
        }
    }

    const cJSON *data =
        cJSON_GetObjectItemCaseSensitive(root, "data");
    const bool media_service = is_media_service(root);
    const cJSON *media_error = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "error")
        : nullptr;
    if (media_service && cJSON_IsObject(media_error)) {
        const cJSON *error_code =
            cJSON_GetObjectItemCaseSensitive(media_error, "code");
        const char *error_message = json_string(media_error, "message");
        const int code = cJSON_IsNumber(error_code)
                             ? error_code->valueint
                             : -1;
        ESP_LOGW(TAG,
                 "AIUI music service error: code=%d message=\"%.160s\"",
                 code, error_message ? error_message : "<none>");
        if (code == 5) {
            ESP_LOGW(TAG,
                     "musicX rejected current device login/activation; "
                     "verify app, scene and serialNumber binding");
        }
    }
    const cJSON *results = data
        ? cJSON_GetObjectItemCaseSensitive(data, "result")
        : nullptr;
    if (!cJSON_IsArray(results)) {
        if (media_service) {
            const cJSON *error = data
                ? cJSON_GetObjectItemCaseSensitive(data, "error")
                : nullptr;
            const char *message = error ? json_string(error, "message")
                                        : nullptr;
            ESP_LOGW(TAG, "AIUI media skill returned no playlist%s%s",
                     message ? ": " : "", message ? message : "");
            aiui_.semantic_session_end_known.store(true);
            aiui_.semantic_should_end_session.store(false);
            ESP_LOGI(TAG,
                     "music search failed; keeping dialogue session active");
            if (!StartMediaSearchFallback(fallback_query, sid)) {
                ESP_LOGW(TAG,
                         "direct NetEase search fallback was not started");
            }
        }
        if (semantic) {
            cJSON_Delete(semantic);
        }
        cJSON_Delete(container);
        return;
    }
    log_media_result_shape(root, results);

    auto *playlist = static_cast<MediaPlaylist *>(heap_caps_calloc(
        1, sizeof(MediaPlaylist), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!playlist) {
        playlist = static_cast<MediaPlaylist *>(calloc(
            1, sizeof(MediaPlaylist)));
    }
    if (!playlist) {
        ESP_LOGE(TAG, "unable to allocate AIUI media playlist");
        if (semantic) {
            cJSON_Delete(semantic);
        }
        cJSON_Delete(container);
        return;
    }

    media_collect_tracks(results, playlist, 6);

    if (semantic) {
        cJSON_Delete(semantic);
    }
    cJSON_Delete(container);
    if (playlist->count == 0) {
        ESP_LOGW(TAG,
                 "AIUI media playlist has neither URL nor itemid");
        if (media_service) {
            aiui_.semantic_session_end_known.store(true);
            aiui_.semantic_should_end_session.store(false);
            ESP_LOGI(TAG,
                     "music search failed; keeping dialogue session active");
            if (!StartMediaSearchFallback(fallback_query, sid)) {
                ESP_LOGW(TAG,
                         "direct NetEase search fallback was not started");
            }
        }
        heap_caps_free(playlist);
        return;
    }

    if (media_active_.load()) {
        StopMediaPlayback();
    }
    if (media_playlist_) {
        heap_caps_free(media_playlist_);
    }
    media_playlist_ = playlist;
    media_pending_ = true;
    media_queued_ms_ = (uint64_t)(esp_timer_get_time() / 1000);
    robot_audio_set_capture_paused(true);
    ESP_LOGI(TAG, "AIUI media playlist ready: tracks=%u first=\"%.120s\"",
             (unsigned)playlist->count,
             playlist->tracks[0].title[0]
                 ? playlist->tracks[0].title
                 : "<untitled>");
}

bool RobotApp::StartMediaSearchFallback(const char *query, const char *sid)
{
    if (!query || query[0] == '\0') {
        return false;
    }
    if (media_search_running_.exchange(true)) {
        ESP_LOGW(TAG, "music search fallback already running");
        return false;
    }
    auto *job = static_cast<MediaSearchJob *>(calloc(
        1, sizeof(MediaSearchJob)));
    if (!job) {
        media_search_running_.store(false);
        return false;
    }
    job->app = this;
    strlcpy(job->query, query, sizeof(job->query));
    media_pending_.store(true);
    media_queued_ms_ = (uint64_t)(esp_timer_get_time() / 1000);
    robot_audio_set_capture_paused(true);
    /*
     * musicX has already generated an error answer for rc=3.  The direct
     * search replaces that result, so consume its TTS through the final frame
     * without sending it to the speaker.  This keeps the AIUI stream aligned.
     */
    strlcpy(suppressed_music_sid_, sid ? sid : "",
            sizeof(suppressed_music_sid_));
    suppress_music_failure_nlp_.store(suppressed_music_sid_[0] != '\0');
    suppress_music_failure_tts_.store(suppressed_music_sid_[0] != '\0');
    media_announcement_pending_.store(true);
    media_announcement_queued_.store(false);
    ESP_LOGI(TAG,
             "musicX returned no track; direct search fallback: \"%.160s\"",
             job->query);
    if (xTaskCreate(MediaSearchEntry, "music_search",
                    kMediaSearchTaskStackBytes, job, 4,
                    nullptr) != pdPASS) {
        free(job);
        media_search_running_.store(false);
        media_pending_.store(false);
        media_announcement_pending_.store(false);
        media_announcement_queued_.store(false);
        suppress_music_failure_nlp_.store(false);
        suppress_music_failure_tts_.store(false);
        return false;
    }
    return true;
}

void RobotApp::MediaSearchEntry(void *ctx)
{
    auto *job = static_cast<MediaSearchJob *>(ctx);
    RobotApp *self = job->app;
    auto *track = static_cast<MediaTrack *>(heap_caps_calloc(
        1, sizeof(MediaTrack), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!track) {
        track = static_cast<MediaTrack *>(calloc(1, sizeof(MediaTrack)));
    }
    esp_err_t result = track
        ? robot_music_search_first(job->query, track->item_id,
                                   sizeof(track->item_id), track->title,
                                   sizeof(track->title))
        : ESP_ERR_NO_MEM;
    Event event = {};
    event.type = EventType::MEDIA_SEARCHED;
    event.code = result;
    event.payload = result == ESP_OK ? track : nullptr;
    if (result != ESP_OK && track) {
        heap_caps_free(track);
    }
    if (self->PostEvent(event) != ESP_OK) {
        if (event.payload) {
            heap_caps_free(event.payload);
        }
        self->media_search_running_.store(false);
        self->media_pending_.store(false);
    }
    free(job);
    vTaskDelete(nullptr);
}

void RobotApp::HandleSearchedMedia(const Event &event)
{
    media_search_running_.store(false);
    if (!media_pending_.load()) {
        ESP_LOGW(TAG, "stale direct music search result ignored");
        return;
    }
    if (event.code != ESP_OK || !event.payload ||
        state_.load() != RobotAppState::SESSION_ACTIVE) {
        ESP_LOGW(TAG, "direct NetEase search failed: %s",
                 esp_err_to_name(static_cast<esp_err_t>(event.code)));
        media_pending_.store(false);
        media_announcement_pending_.store(false);
        media_announcement_queued_.store(false);
        if (!aiui_.tts_playing.load() && !robot_audio_is_playing()) {
            robot_audio_set_capture_paused(!aiui_.connected.load());
        }
        TouchActivity();
        return;
    }

    const auto *matched = static_cast<const MediaTrack *>(event.payload);
    auto *playlist = static_cast<MediaPlaylist *>(heap_caps_calloc(
        1, sizeof(MediaPlaylist), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!playlist) {
        playlist = static_cast<MediaPlaylist *>(calloc(
            1, sizeof(MediaPlaylist)));
    }
    if (!playlist) {
        ESP_LOGE(TAG, "unable to allocate fallback music playlist");
        media_pending_.store(false);
        return;
    }
    playlist->count = 1;
    strlcpy(playlist->tracks[0].item_id, matched->item_id,
            sizeof(playlist->tracks[0].item_id));
    strlcpy(playlist->tracks[0].title, matched->title,
            sizeof(playlist->tracks[0].title));

    if (media_active_.load()) {
        StopMediaPlayback();
    }
    if (media_playlist_) {
        heap_caps_free(media_playlist_);
    }
    media_playlist_ = playlist;
    media_pending_.store(true);
    media_queued_ms_ = (uint64_t)(esp_timer_get_time() / 1000);
    aiui_.semantic_session_end_known.store(true);
    aiui_.semantic_should_end_session.store(false);
    ESP_LOGI(TAG, "fallback music ready: \"%.120s\"",
             playlist->tracks[0].title);
    if (!aiui_.discard_tts_until_final.load() &&
        !aiui_.tts_playing.load() && !robot_audio_is_playing()) {
        StartMediaAnnouncement();
    }
}

esp_err_t RobotApp::CloudSpeak(void *ctx, const char *text)
{
    RobotApp *app = static_cast<RobotApp *>(ctx);
    if (app == nullptr || text == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = strlen(text);
    if (length == 0 || length > 200) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Speaking a console-provided sentence reuses the AIUI session, which is
     * only open while the robot is awake.  Report that state instead of
     * silently failing so the console can tell the user to wake the robot.
     */
    if (app->aiui_.handle == nullptr) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!app->aiui_.connected.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    robot_audio_set_capture_paused(true);
    app->aiui_.awaiting_reply.store(true);
    app->aiui_.awaiting_reply_since_ms.store(
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL));
    if (aiui_send_tts(app->aiui_.handle, text, (int)length) != 0) {
        app->aiui_.awaiting_reply.store(false);
        app->aiui_.awaiting_reply_since_ms.store(0);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "console announcement queued");
    app->TouchActivity();
    return ESP_OK;
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

bool RobotApp::StartMediaAnnouncement()
{
    if (!media_announcement_pending_.load() ||
        aiui_.discard_tts_until_final.load() ||
        aiui_.tts_playing.load() || robot_audio_is_playing()) {
        return false;
    }
    auto *playlist = static_cast<MediaPlaylist *>(media_playlist_);
    if (!playlist || playlist->index >= playlist->count ||
        !aiui_.handle || !aiui_.connected.load()) {
        return false;
    }
    if (media_announcement_queued_.exchange(true)) {
        return false;
    }

    const char *title = playlist->tracks[playlist->index].title[0]
                            ? playlist->tracks[playlist->index].title
                            : "这首歌";
    char prompt[256];
    int length = snprintf(prompt, sizeof(prompt),
                          "好的，为你播放%s", title);
    if (length <= 0 || length >= (int)sizeof(prompt)) {
        media_announcement_queued_.store(false);
        media_announcement_pending_.store(false);
        StartPendingMedia();
        return false;
    }

    robot_audio_set_capture_paused(true);
    aiui_.awaiting_reply.store(true);
    aiui_.awaiting_reply_since_ms.store(
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL));
    if (aiui_send_tts(aiui_.handle, prompt, length) != 0) {
        ESP_LOGW(TAG,
                 "unable to queue media announcement; playing directly");
        aiui_.awaiting_reply.store(false);
        aiui_.awaiting_reply_since_ms.store(0);
        media_announcement_queued_.store(false);
        media_announcement_pending_.store(false);
        StartPendingMedia();
        return false;
    }
    ESP_LOGI(TAG, "media announcement queued: %s", prompt);
    TouchActivity();
    return true;
}

void RobotApp::MediaResolveEntry(void *ctx)
{
    auto *job = static_cast<MediaResolveJob *>(ctx);
    RobotApp *self = job->app;
    char *url = static_cast<char *>(heap_caps_calloc(
        1, kMaxMediaUrlLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!url) {
        url = static_cast<char *>(calloc(1, kMaxMediaUrlLength));
    }
    esp_err_t result = url
        ? robot_music_resolve_play_url(job->item_id, url,
                                       kMaxMediaUrlLength)
        : ESP_ERR_NO_MEM;
    Event event = {};
    event.type = EventType::MEDIA_RESOLVED;
    event.code = result;
    event.dts = static_cast<int>(job->track_index);
    event.payload = result == ESP_OK ? url : nullptr;
    if (result != ESP_OK && url) {
        heap_caps_free(url);
    }
    if (self->PostEvent(event) != ESP_OK) {
        if (event.payload) {
            heap_caps_free(event.payload);
        }
        self->media_resolve_running_.store(false);
    }
    free(job);
    vTaskDelete(nullptr);
}

void RobotApp::HandleResolvedMedia(const Event &event)
{
    media_resolve_running_.store(false);
    auto *playlist = static_cast<MediaPlaylist *>(media_playlist_);
    const size_t index = event.dts >= 0
                             ? static_cast<size_t>(event.dts)
                             : kMaxMediaTracks;
    if (!playlist || index != playlist->index ||
        index >= playlist->count || !media_pending_) {
        ESP_LOGW(TAG, "stale music URL result ignored");
        return;
    }

    if (event.code == ESP_OK && event.payload) {
        const char *url = static_cast<const char *>(event.payload);
        if (strlcpy(playlist->tracks[index].url, url,
                    sizeof(playlist->tracks[index].url)) >=
            sizeof(playlist->tracks[index].url)) {
            ESP_LOGE(TAG, "resolved music URL is too long");
            ++playlist->index;
        } else {
            ESP_LOGI(TAG, "music URL ready for track %u/%u",
                     (unsigned)(index + 1),
                     (unsigned)playlist->count);
        }
    } else {
        ESP_LOGW(TAG, "unable to resolve track %u/%u: %s",
                 (unsigned)(index + 1),
                 (unsigned)playlist->count,
                 esp_err_to_name(static_cast<esp_err_t>(event.code)));
        ++playlist->index;
    }

    if (playlist->index >= playlist->count) {
        ESP_LOGW(TAG, "no playable track remains in AIUI playlist");
        media_pending_ = false;
        heap_caps_free(media_playlist_);
        media_playlist_ = nullptr;
        aiui_.semantic_session_end_known.store(false);
        aiui_.semantic_should_end_session.store(false);
        robot_audio_set_capture_paused(!aiui_.connected.load());
        TouchActivity();
        return;
    }

    media_queued_ms_ = (uint64_t)(esp_timer_get_time() / 1000);
    if (!aiui_.tts_playing.load() && !robot_audio_is_playing()) {
        StartPendingMedia();
    }
}

void RobotApp::StartPendingMedia()
{
    auto *playlist = static_cast<MediaPlaylist *>(media_playlist_);
    if (!media_pending_ || media_announcement_pending_.load() || !playlist ||
        playlist->index >= playlist->count) {
        return;
    }

    while (playlist->index < playlist->count) {
        MediaTrack &track = playlist->tracks[playlist->index];
        if (track.url[0] == '\0') {
            if (track.item_id[0] == '\0') {
                ++playlist->index;
                continue;
            }
            if (media_resolve_running_.exchange(true)) {
                return;
            }
            auto *job = static_cast<MediaResolveJob *>(
                calloc(1, sizeof(MediaResolveJob)));
            if (!job) {
                media_resolve_running_.store(false);
                ++playlist->index;
                continue;
            }
            job->app = this;
            job->track_index = playlist->index;
            strlcpy(job->item_id, track.item_id,
                    sizeof(job->item_id));
            media_queued_ms_ =
                (uint64_t)(esp_timer_get_time() / 1000);
            robot_audio_set_capture_paused(true);
            ESP_LOGI(TAG,
                     "resolving music URL for track %u/%u: \"%.120s\"",
                     (unsigned)(playlist->index + 1),
                     (unsigned)playlist->count,
                     track.title[0] ? track.title : "<untitled>");
            if (xTaskCreate(MediaResolveEntry, "music_url",
                            kMediaResolveTaskStackBytes, job, 4,
                            nullptr) != pdPASS) {
                free(job);
                media_resolve_running_.store(false);
                ++playlist->index;
                continue;
            }
            return;
        }
        if (InitMediaPlayer() != ESP_OK) {
            FinishMediaPlayback(false);
            return;
        }
        robot_audio_set_capture_paused(true);
        media_pending_ = false;
        media_decoder_done_.store(false);
        media_advance_after_drain_ = false;
        media_paused_ = false;
        media_active_.store(true);
        playback_generation_.store(
            robot_audio_start_playback_stream());
        ESP_LOGI(TAG,
                 "media play %u/%u: \"%.120s\" url_bytes=%u",
                 (unsigned)(playlist->index + 1),
                 (unsigned)playlist->count,
                 track.title[0] ? track.title : "<untitled>",
                 (unsigned)strlen(track.url));
        ESP_LOGI(TAG,
                 "media heap: internal=%u largest=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        esp_gmf_err_t err = esp_audio_simple_player_run(
            media_player_, track.url, nullptr);
        if (err == ESP_GMF_ERR_OK) {
            TouchActivity();
            return;
        }
        ESP_LOGE(TAG, "media player rejected track %u: 0x%x",
                 (unsigned)(playlist->index + 1), (unsigned)err);
        media_active_.store(false);
        media_decoder_done_.store(false);
        ++playlist->index;
    }
    FinishMediaPlayback(false);
}

void RobotApp::FinishMediaPlayback(bool allow_next)
{
    auto *playlist = static_cast<MediaPlaylist *>(media_playlist_);
    media_active_.store(false);
    media_decoder_done_.store(false);
    media_advance_after_drain_ = false;
    media_paused_ = false;
    if (allow_next && playlist &&
        playlist->index + 1 < playlist->count) {
        ++playlist->index;
        media_pending_ = true;
        media_queued_ms_ = (uint64_t)(esp_timer_get_time() / 1000);
        /*
         * esp_audio_simple_player cannot safely reuse its HTTP/decoder
         * pipeline for a new URI immediately after FINISHED.  Destroy the
         * completed pipeline and let InitMediaPlayer create a clean one for
         * the next playlist item.  If teardown is still asynchronous, the
         * MEDIA_DECODER_DONE path above resumes playback after release.
         */
        media_release_pending_.store(media_player_ != nullptr);
        ReleaseMediaPlayer();
        if (!media_player_) {
            StartPendingMedia();
        } else {
            ESP_LOGI(TAG,
                     "waiting for media pipeline release before next item");
        }
        return;
    }

    media_pending_ = false;
    if (media_playlist_) {
        heap_caps_free(media_playlist_);
        media_playlist_ = nullptr;
    }
    ReleaseMediaPlayer();
    const bool should_end_session =
        aiui_.semantic_session_end_known.load() &&
        aiui_.semantic_should_end_session.load();
    aiui_.semantic_session_end_known.store(false);
    aiui_.semantic_should_end_session.store(false);
    if (should_end_session) {
        ESP_LOGI(TAG,
                 "media skill turn ended; local dialogue remains active");
    }
    const bool keep_paused =
        state_.load() == RobotAppState::SESSION_ACTIVE &&
        !aiui_.connected.load();
    robot_audio_set_capture_paused(keep_paused);
    TouchActivity();
    ESP_LOGI(TAG, "media playback complete; conversation resumed");
}

void RobotApp::StopMediaPlayback()
{
    media_pending_ = false;
    media_announcement_pending_.store(false);
    media_announcement_queued_.store(false);
    media_advance_after_drain_ = false;
    media_paused_ = false;
    const bool was_active = media_active_.exchange(false);
    media_decoder_done_.store(false);
    media_release_pending_.store(media_player_ != nullptr);
    /*
     * Silence the speaker first. GMF may need up to its stop timeout to tear
     * down an in-flight HTTP read; leaving decoded PCM queued during that
     * wait makes a BOOT interruption sound delayed.
     */
    robot_audio_abort_playback();
    playback_generation_.store(
        robot_audio_get_playback_generation());
    if (media_player_ && was_active) {
        esp_audio_simple_player_stop(media_player_);
    }
    if (media_playlist_) {
        heap_caps_free(media_playlist_);
        media_playlist_ = nullptr;
    }
    ReleaseMediaPlayer();
}

void RobotApp::ReleaseMediaPlayer()
{
    if (!media_player_) {
        media_release_pending_.store(false);
        return;
    }
    esp_asp_state_t state = ESP_ASP_STATE_NONE;
    if (esp_audio_simple_player_get_state(media_player_, &state) !=
        ESP_GMF_ERR_OK) {
        return;
    }
    if (state == ESP_ASP_STATE_NONE &&
        media_release_pending_.load()) {
        /*
         * The async pipeline may still be in OPENING before its first RUNNING
         * event. Destroying it in that window races the GMF worker. The event
         * callback will post MEDIA_DECODER_DONE as soon as RUNNING appears,
         * and this method will stop and release it from the app task.
         */
        ESP_LOGW(TAG,
                 "media pipeline release deferred while opening");
        return;
    }
    if (state == ESP_ASP_STATE_RUNNING ||
        state == ESP_ASP_STATE_PAUSED) {
        media_release_pending_.store(true);
        esp_audio_simple_player_stop(media_player_);
        if (esp_audio_simple_player_get_state(media_player_, &state) !=
                ESP_GMF_ERR_OK ||
            state == ESP_ASP_STATE_RUNNING ||
            state == ESP_ASP_STATE_PAUSED) {
            ESP_LOGW(TAG,
                     "media pipeline release deferred until stop event");
            return;
        }
    }
    esp_asp_handle_t player = media_player_;
    media_player_ = nullptr;
    media_release_pending_.store(false);
    esp_gmf_err_t err = esp_audio_simple_player_destroy(player);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "unable to destroy media pipeline: 0x%x",
                 (unsigned)err);
    }
    ESP_LOGI(TAG,
             "media pipeline released: internal=%u largest=%u psram=%u",
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

int RobotApp::MediaOutput(uint8_t *data, int data_size, void *ctx)
{
    auto *self = static_cast<RobotApp *>(ctx);
    if (!self || !self->media_active_.load() || !data || data_size <= 0 ||
        (data_size & 1) != 0) {
        return -1;
    }
    esp_err_t err = robot_audio_play_pcm(
        reinterpret_cast<const int16_t *>(data),
        static_cast<size_t>(data_size) / sizeof(int16_t));
    return err == ESP_OK ? 0 : -1;
}

int RobotApp::MediaEvent(esp_asp_event_pkt_t *event, void *ctx)
{
    auto *self = static_cast<RobotApp *>(ctx);
    if (!self || !event) {
        return -1;
    }
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload &&
        event->payload_size >= (int)sizeof(esp_asp_music_info_t)) {
        esp_asp_music_info_t info = {};
        memcpy(&info, event->payload, sizeof(info));
        ESP_LOGI(TAG, "media format: %d Hz %u channel %u bit",
                 info.sample_rate, (unsigned)info.channels,
                 (unsigned)info.bits);
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE &&
               event->payload &&
               event->payload_size >= (int)sizeof(esp_asp_state_t)) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        memcpy(&state, event->payload, sizeof(state));
        ESP_LOGI(TAG, "media player state: %s",
                 esp_audio_simple_player_state_to_str(state));
        if (state == ESP_ASP_STATE_FINISHED ||
            state == ESP_ASP_STATE_STOPPED ||
            state == ESP_ASP_STATE_ERROR ||
            (self->media_release_pending_.load() &&
             state == ESP_ASP_STATE_RUNNING)) {
            Event done = {};
            done.type = EventType::MEDIA_DECODER_DONE;
            done.code = state;
            self->PostEvent(done);
        }
    }
    return 0;
}

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

void RobotApp::ResumeListeningAfterNoResult(const char *reason)
{
    if (state_.load() != RobotAppState::SESSION_ACTIVE) {
        return;
    }
    /*
     * Gate AudioSendEntry while the current request is closed. Otherwise a
     * freshly captured PCM frame can overtake the status=2 end frame and be
     * appended to the abandoned utterance.
     */
    robot_audio_set_capture_paused(true);
    aiui_.awaiting_reply.store(true);
    const bool request_was_open =
        aiui_.audio_request_open.exchange(false);
    aiui_.cloud_utterance_active.store(false);
    aiui_.vad_speech_ms.store(0);
    aiui_.vad_silence_ms.store(0);
    aiui_.utterance_active.store(false);
    if (aiui_.vad_handle) {
        vad_reset_trigger(static_cast<vad_handle_t>(aiui_.vad_handle));
    }
    if (aiui_.audio_queue) {
        xQueueReset(aiui_.audio_queue);
    }
    if (request_was_open && aiui_.connected.load() &&
        aiui_send_audio(aiui_.handle, nullptr, 0) != 0) {
        ESP_LOGW(TAG, "unable to close silent AIUI audio request");
        aiui_.connected.store(false);
        Event disconnected = {};
        disconnected.type = EventType::AIUI_DISCONNECTED;
        disconnected.code = -1;
        PostEvent(disconnected);
    }
    aiui_.awaiting_reply.store(false);
    aiui_.awaiting_reply_since_ms.store(0);
    aiui_.turn_result_seen.store(false);
    aiui_.semantic_session_end_known.store(false);
    aiui_.semantic_should_end_session.store(false);
    aiui_.discard_tts_until_final.store(false);
    const bool playback_active =
        aiui_.tts_playing.load() || robot_audio_is_playing();
    robot_audio_set_capture_paused(
        !aiui_.connected.load() || playback_active);
    ESP_LOGW(TAG, "%s; listening again", reason);
}

esp_err_t RobotApp::InitAiui()
{
    if (aiui_.handle) {
        return ESP_OK;
    }
    robot_aiui_credentials_t credentials;
    esp_err_t err = robot_network_load_aiui_credentials(&credentials);
    if (err != ESP_OK) {
        return err;
    }
    if (!is_safe_credential(credentials.appid) ||
        !is_safe_credential(credentials.appkey) ||
        !is_safe_credential(credentials.appsecret)) {
        memset(&credentials, 0, sizeof(credentials));
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char uid[20];
    snprintf(uid, sizeof(uid), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char params[640];
    int length = snprintf(params, sizeof(params),
        "{\"login\":{\"appid\":\"%s\",\"key\":\"%s\",\"api_secret\":\"%s\"},"
        "\"global\":{\"scene\":\"%s\",\"aiui_ver\":\"3\",\"uid\":\"%s\"},"
        "\"tts\":{\"voice_name\":\"%s\",\"data_encoding\":\"opus-wb\"}}",
        credentials.appid, credentials.appkey, credentials.appsecret,
        CONFIG_ROBOT_AIUI_SCENE, uid, CONFIG_ROBOT_AIUI_VCN);
    memset(&credentials, 0, sizeof(credentials));
    if (length <= 0 || length >= (int)sizeof(params)) {
        memset(params, 0, sizeof(params));
        return ESP_ERR_INVALID_SIZE;
    }
    aiui_.audio_queue = xQueueCreate(kAudioQueueDepth, sizeof(AudioFrame));
    if (!aiui_.audio_queue) {
        memset(params, 0, sizeof(params));
        return ESP_ERR_NO_MEM;
    }
    aiui_.handle = aiui_init(params, AiuiEvent, this);
    memset(params, 0, sizeof(params));
    if (!aiui_.handle) {
        vQueueDelete(aiui_.audio_queue);
        aiui_.audio_queue = nullptr;
        return ESP_FAIL;
    }
    aiui_.vad_handle = vad_create(VAD_MODE_3);
    if (!aiui_.vad_handle) {
        ESP_LOGW(TAG, "local utterance VAD unavailable");
    }
    aiui_.stop_audio_task.store(false);
    if (xTaskCreate(AudioSendEntry, "aiui_audio",
                    kAiuiAudioTaskStackBytes, this, 5,
                    &aiui_.audio_task) != pdPASS) {
        DeinitAiui();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AIUI %s initialized (credentials redacted)", aiui_get_version());
    return ESP_OK;
}

void RobotApp::DeinitAiui()
{
    StopAiui();
    aiui_.stop_audio_task.store(true);
    for (int i = 0; aiui_.audio_task && i < 50; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (aiui_.handle) {
        aiui_uninit(aiui_.handle);
        aiui_.handle = nullptr;
    }
    if (aiui_.vad_handle) {
        vad_destroy(static_cast<vad_handle_t>(aiui_.vad_handle));
        aiui_.vad_handle = nullptr;
    }
    if (aiui_.audio_queue) {
        vQueueDelete(aiui_.audio_queue);
        aiui_.audio_queue = nullptr;
    }
    aiui_.audio_task = nullptr;
}

esp_err_t RobotApp::StartAiui()
{
    if (!aiui_.handle || aiui_.active.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    session_exit_pending_.store(false);
    aiui_.connected.store(false);
    aiui_.tts_playing.store(false);
    aiui_.tts_final_received.store(false);
    aiui_retry_at_ms_ = 0;
    aiui_retry_count_ = 0;
    aiui_.captured_frames.store(0);
    aiui_.queued_frames.store(0);
    aiui_.sent_frames.store(0);
    aiui_.dropped_frames.store(0);
    aiui_.vad_speech_ms.store(0);
    aiui_.vad_silence_ms.store(0);
    aiui_.utterance_active.store(false);
    aiui_.cloud_utterance_active.store(false);
    aiui_.audio_request_open.store(false);
    aiui_.audio_gate_epoch.store(0);
    aiui_.awaiting_reply.store(false);
    aiui_.awaiting_reply_since_ms.store(0);
    aiui_.turn_result_seen.store(false);
    aiui_.semantic_session_end_known.store(false);
    aiui_.semantic_should_end_session.store(false);
    aiui_.discard_tts_until_final.store(false);
    aiui_.tts_chunk_count.store(0);
    aiui_.tts_last_chunk_ms.store(0);
    ResetReplyPresentation();
    if (aiui_.vad_handle) {
        vad_reset_trigger(static_cast<vad_handle_t>(aiui_.vad_handle));
    }
    if (aiui_start(aiui_.handle) != 0) {
        aiui_.active.store(false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void RobotApp::StopAiui()
{
    if (!aiui_.handle || !aiui_.active.exchange(false)) {
        return;
    }
    aiui_stop(aiui_.handle);
    aiui_.connected.store(false);
    aiui_.tts_playing.store(false);
    aiui_.tts_final_received.store(false);
    aiui_retry_at_ms_ = 0;
    aiui_retry_count_ = 0;
    aiui_.awaiting_reply.store(false);
    aiui_.awaiting_reply_since_ms.store(0);
    aiui_.turn_result_seen.store(false);
    aiui_.semantic_session_end_known.store(false);
    aiui_.semantic_should_end_session.store(false);
    aiui_.discard_tts_until_final.store(false);
    aiui_.audio_request_open.store(false);
    aiui_.audio_gate_epoch.fetch_add(1);
    aiui_.cloud_utterance_active.store(false);
    aiui_.vad_speech_ms.store(0);
    aiui_.vad_silence_ms.store(0);
    aiui_.utterance_active.store(false);
    aiui_.tts_chunk_count.store(0);
    aiui_.tts_last_chunk_ms.store(0);
    ResetReplyPresentation();
    next_listening_animation_ms_ = 0;
    robot_audio_aiui_opus_abort();
    robot_audio_set_capture_paused(false);
    if (aiui_.audio_queue) {
        xQueueReset(aiui_.audio_queue);
    }
}

void RobotApp::BeginIdleTimeoutExit()
{
    if (state_.load() != RobotAppState::SESSION_ACTIVE ||
        session_exit_pending_.exchange(true)) {
        return;
    }

    ESP_LOGI(TAG,
             "session idle for %d seconds; closing AIUI and playing "
             "7911 exit prompt",
             CONFIG_ROBOT_SESSION_IDLE_TIMEOUT_SECONDS);
    robot_audio_set_capture_paused(true);
    StopMediaPlayback();
    robot_audio_aiui_opus_abort();
    robot_audio_abort_playback();
    StopAiui();
    /*
     * StopAiui() reopens capture for the ordinary session-stop path. Keep it
     * paused until the local farewell has drained, then StopSession() will
     * arm offline wake detection.
     */
    robot_audio_set_capture_paused(true);
    playback_generation_.store(robot_audio_start_playback_stream());
    esp_err_t err =
        robot_audio_play_wav_file("/assets/aiui_exit.wav");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "idle exit prompt failed: %s",
                 esp_err_to_name(err));
        session_exit_pending_.store(false);
        StopSession("idle timeout");
    }
}

void RobotApp::StopSession(const char *reason)
{
    if (state_.load() != RobotAppState::SESSION_ACTIVE) {
        return;
    }
    session_exit_pending_.store(false);
    ESP_LOGW(TAG, "session stop: %s", reason);
    StopMediaPlayback();
    robot_audio_aiui_opus_abort();
    robot_audio_abort_playback();
    playback_generation_.store(robot_audio_get_playback_generation());
    StopAiui();
    /*
     * StopAiui() is intentionally idempotent and returns immediately when
     * AIUI was already stopped.  The idle-farewell path stops AIUI before
     * playing its local prompt and pauses capture again, so always reopen
     * capture here before arming the offline wake detector.
     */
    robot_audio_set_capture_paused(false);
    if (wake_ready_) {
        robot_wakeup_start();
    }
    state_.store(RobotAppState::OFFLINE_WAKE_READY);
    ESP_LOGI(TAG,
             "offline wake re-armed; microphone capture resumed");
    robot_ui_set_idle_animation(true);
    robot_action_t neutral_action = {};
    neutral_action.type = ROBOT_ACTION_EXPRESSION;
    neutral_action.expression = ROBOT_EXPRESSION_NEUTRAL;
    robot_action_queue_submit(&neutral_action, 0);
    last_activity_ms_ = 0;
}

void RobotApp::EnterSleepState(const char *reason)
{
    /*
     * End only the current online dialogue. StopSession restores microphone
     * capture and arms ESP-SR again, so sleep remains wakeable by either the
     * offline phrase or BOOT. Queue sleep after StopSession's neutral frame.
     */
    StopSession(reason);
    robot_ui_set_idle_animation(false);
    robot_action_t sleep_action = {};
    sleep_action.type = ROBOT_ACTION_SLEEP;
    robot_action_queue_submit(&sleep_action, 0);
    ESP_LOGI(TAG,
             "sleep entered by exit intent; offline wake remains armed");
}

void RobotApp::FeedAiui(const int16_t *samples, size_t sample_count)
{
    if (!aiui_.audio_queue || !aiui_.active.load() || !samples) {
        return;
    }
    size_t offset = 0;
    while (offset < sample_count) {
        AudioFrame frame = {};
        frame.sample_count = std::min(kAudioFrameSamples, sample_count - offset);
        for (size_t i = 0; i < frame.sample_count; ++i) {
            int32_t amplified =
                static_cast<int32_t>(samples[offset + i]) * kAiuiCaptureGain;
            amplified = std::max<int32_t>(
                INT16_MIN, std::min<int32_t>(INT16_MAX, amplified));
            frame.samples[i] = static_cast<int16_t>(amplified);
        }
        if (xQueueSend(aiui_.audio_queue, &frame, 0) != pdTRUE) {
            AudioFrame discarded;
            xQueueReceive(aiui_.audio_queue, &discarded, 0);
            aiui_.dropped_frames.fetch_add(1);
            if (xQueueSend(aiui_.audio_queue, &frame, 0) != pdTRUE) {
                aiui_.dropped_frames.fetch_add(1);
                offset += frame.sample_count;
                continue;
            }
        }
        aiui_.queued_frames.fetch_add(1);
        offset += frame.sample_count;
    }
}

void RobotApp::HandleTtsResult(const aiui_event_result_t &result)
{
    const int dts = result.params.dts;
    const size_t chunk_bytes =
        result.params.key_data && result.params.key_data_size > 1
            ? result.params.key_data_size - 1
            : 0;
    const uint32_t now_ms =
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    if (dts == 0 || dts == 3) {
        aiui_.tts_chunk_count.store(0);
        aiui_.tts_last_chunk_ms.store(0);
    }
    const uint32_t previous_ms = aiui_.tts_last_chunk_ms.exchange(now_ms);
    const uint32_t chunk_index = aiui_.tts_chunk_count.fetch_add(1) + 1;
    ESP_LOGI(TAG,
             "TTS chunk=%u status=%d seq=%d bytes=%u delta=%u ms "
             "encoding=%s",
             (unsigned)chunk_index, dts, result.params.dseq,
             (unsigned)chunk_bytes,
             previous_ms ? (unsigned)(now_ms - previous_ms) : 0U,
             result.params.dte);

    const bool opus = strcmp(result.params.dte, "opus-wb") == 0;
    const bool raw = strcmp(result.params.dte, "raw") == 0;
    esp_err_t err = ESP_OK;
    if (opus && (dts == 0 || dts == 3)) {
        playback_generation_.store(
            robot_audio_start_playback_stream());
        err = robot_audio_aiui_opus_begin();
    }
    if (chunk_bytes > 0) {
        if (opus && err == ESP_OK) {
            err = robot_audio_aiui_opus_write(
                static_cast<const uint8_t *>(result.params.key_data),
                chunk_bytes);
        } else if (raw && (chunk_bytes & 1U) == 0) {
            /* Keep raw support as a server-side compatibility fallback. */
            err = robot_audio_play_pcm(
                static_cast<const int16_t *>(result.params.key_data),
                chunk_bytes / sizeof(int16_t));
        } else if (!opus) {
            ESP_LOGE(TAG, "unsupported TTS audio: encoding=%s bytes=%u",
                     result.params.dte, (unsigned)chunk_bytes);
            err = ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (err == ESP_OK && (dts == 2 || dts == 3)) {
        err = opus ? robot_audio_aiui_opus_end()
                   : robot_audio_finish_playback();
    }
    if (err != ESP_OK) {
        Event error = {};
        error.type = EventType::AIUI_ERROR;
        error.code = err;
        PostEvent(error);
    }
}

void RobotApp::AudioSendEntry(void *ctx)
{
    auto *self = static_cast<RobotApp *>(ctx);
    /*
     * Do not place the 1.25 KiB audio frame and 640-byte VAD workspace on
     * this task's stack. Together with Event objects and the AIUI/FreeRTOS
     * call chain they overflowed the original 4 KiB stack immediately after
     * the first audio packet.
     */
    auto *frame = static_cast<AudioFrame *>(heap_caps_malloc(
        sizeof(AudioFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame) {
        frame = static_cast<AudioFrame *>(heap_caps_malloc(
            sizeof(AudioFrame), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    auto *vad_samples = static_cast<int16_t *>(heap_caps_malloc(
        kVadFrameSamples * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!frame || (self->aiui_.vad_handle && !vad_samples)) {
        ESP_LOGE(TAG, "unable to allocate AIUI audio worker buffers");
        if (frame) {
            heap_caps_free(frame);
        }
        if (vad_samples) {
            heap_caps_free(vad_samples);
        }
        self->aiui_.audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    while (!self->aiui_.stop_audio_task.load()) {
        if (!self->aiui_.active.load() || !self->aiui_.connected.load() ||
            self->aiui_.tts_playing.load() ||
            self->aiui_.awaiting_reply.load() ||
            self->media_active_.load() ||
            self->media_pending_.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (xQueueReceive(self->aiui_.audio_queue, frame,
                          pdMS_TO_TICKS(50)) == pdTRUE) {
            /*
             * Recheck after the blocking queue receive. A TTS callback can
             * close the turn during those 50 ms; sending this already-queued
             * frame would reopen audio_request_open in the middle of TTS.
             */
            if (self->aiui_.tts_playing.load() ||
                self->aiui_.awaiting_reply.load() ||
                self->media_active_.load() ||
                self->media_pending_.load()) {
                continue;
            }
            if (frame->sample_count == kAudioFrameSamples) {
                /*
                 * TTS/media callbacks can close the microphone turn while
                 * this task is inside aiui_send_audio(). The epoch makes the
                 * close persistent: a sender that started before the close
                 * must never reopen audio_request_open afterwards.
                 */
                const uint32_t gate_epoch =
                    self->aiui_.audio_gate_epoch.load();
                if (aiui_send_audio(self->aiui_.handle,
                                    reinterpret_cast<const char *>(frame->samples),
                                    frame->sample_count *
                                        sizeof(frame->samples[0])) != 0) {
                    self->aiui_.connected.store(false);
                    robot_audio_set_capture_paused(true);
                    Event disconnected = {};
                    disconnected.type = EventType::AIUI_DISCONNECTED;
                    disconnected.code = -1;
                    self->PostEvent(disconnected);
                } else {
                    if (gate_epoch ==
                            self->aiui_.audio_gate_epoch.load() &&
                        !self->aiui_.awaiting_reply.load() &&
                        !self->aiui_.tts_playing.load() &&
                        !self->media_active_.load() &&
                        !self->media_pending_.load()) {
                        self->aiui_.audio_request_open.store(true);
                    }
                    uint32_t sent = self->aiui_.sent_frames.fetch_add(1) + 1;
                    if (sent == 1 || sent % 25 == 0) {
                        int32_t tx_peak = 0;
                        for (size_t i = 0; i < frame->sample_count; ++i) {
                            int32_t value = frame->samples[i];
                            if (value < 0) {
                                value = -value;
                            }
                            tx_peak = std::max(tx_peak, value);
                        }
                        ESP_LOGI(TAG,
                                 "audio pipeline captured=%u queued=%u sent=%u "
                                 "dropped=%u pending=%u tx_peak=%d",
                                 (unsigned)self->aiui_.captured_frames.load(),
                                 (unsigned)self->aiui_.queued_frames.load(),
                                 (unsigned)sent,
                                 (unsigned)self->aiui_.dropped_frames.load(),
                                 (unsigned)uxQueueMessagesWaiting(
                                     self->aiui_.audio_queue),
                                 (int)tx_peak);
                    }
                    if (self->aiui_.vad_handle && vad_samples) {
                        auto vad = static_cast<vad_handle_t>(
                            self->aiui_.vad_handle);
                        for (size_t offset = 0;
                             offset + kVadFrameSamples <= frame->sample_count;
                             offset += kVadFrameSamples) {
                            for (size_t i = 0; i < kVadFrameSamples; ++i) {
                                vad_samples[i] = static_cast<int16_t>(
                                    frame->samples[offset + i] /
                                    kAiuiCaptureGain);
                            }
                            vad_state_t state = vad_process(
                                vad, vad_samples, 16000, kVadFrameMs);
                            if (!self->aiui_.utterance_active.load()) {
                                if (state == VAD_SPEECH) {
                                    uint32_t speech_ms =
                                        self->aiui_.vad_speech_ms.fetch_add(
                                            kVadFrameMs) + kVadFrameMs;
                                    if (speech_ms >= kVadSpeechStartMs) {
                                        self->aiui_.utterance_active.store(true);
                                        self->aiui_.turn_result_seen.store(false);
                                        self->aiui_.semantic_session_end_known.store(
                                            false);
                                        self->aiui_.semantic_should_end_session.store(
                                            false);
                                        self->aiui_.vad_silence_ms.store(0);
                                        ESP_LOGI(TAG,
                                                 "local VAD: speech started");
                                    }
                                } else {
                                    self->aiui_.vad_speech_ms.store(0);
                                }
                            } else if (state == VAD_SILENCE) {
                                uint32_t silence_ms =
                                    self->aiui_.vad_silence_ms.fetch_add(
                                        kVadFrameMs) + kVadFrameMs;
                                if (silence_ms >= kVadSpeechEndMs) {
                                    self->aiui_.utterance_active.store(false);
                                    self->aiui_.vad_speech_ms.store(0);
                                    self->aiui_.vad_silence_ms.store(0);
                                    vad_reset_trigger(vad);
                                    /*
                                     * External audio input must be terminated
                                     * explicitly. In the RTOS v3 SDK a
                                     * zero-length audio write is serialized as
                                     * status=2, equivalent to CMD_STOP_WRITE
                                     * and the 7911 demo's
                                     * ifly_aiui_send_voice_stop().
                                     *
                                     * Without this frame the cloud can return
                                     * partial NLP/TTS data, but keeps the
                                     * request open and withholds the final TTS
                                     * packet until the WebSocket is closed.
                                     */
                                    if (!self->aiui_.audio_request_open.exchange(
                                            false)) {
                                        break;
                                    }
                                    self->aiui_.awaiting_reply.store(true);
                                    self->aiui_.awaiting_reply_since_ms.store(
                                        (uint32_t)(
                                            (uint64_t)esp_timer_get_time() /
                                            1000ULL));
                                    robot_audio_set_capture_paused(true);
                                    if (self->aiui_.audio_queue) {
                                        xQueueReset(
                                            self->aiui_.audio_queue);
                                    }
                                    if (aiui_send_audio(self->aiui_.handle,
                                                        nullptr, 0) != 0) {
                                        self->aiui_.connected.store(false);
                                        Event disconnected = {};
                                        disconnected.type =
                                            EventType::AIUI_DISCONNECTED;
                                        disconnected.code = -1;
                                        self->PostEvent(disconnected);
                                    } else {
                                        ESP_LOGI(
                                            TAG,
                                            "local VAD: speech ended; "
                                            "AIUI audio end sent");
                                    }
                                    break;
                                }
                            } else {
                                self->aiui_.vad_silence_ms.store(0);
                            }
                        }
                    }
                    if (sent == 1 || sent == 25) {
                        ESP_LOGI(
                            TAG,
                            "aiui_audio stack minimum free=%u bytes",
                            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                    }
                }
            }
        }
    }
    heap_caps_free(frame);
    if (vad_samples) {
        heap_caps_free(vad_samples);
    }
    self->aiui_.audio_task = nullptr;
    vTaskDelete(nullptr);
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
            self->media_active_.load() ||
            self->media_pending_.load()) {
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

void RobotApp::AiuiEvent(const void *ctx, aiui_callback_event_t *callback)
{
    auto *self = const_cast<RobotApp *>(static_cast<const RobotApp *>(ctx));
    if (!self || !callback) {
        return;
    }
    Event event = {};
    if (callback->type == AIUI_EVENT_CONNECTED_TO_SERVER) {
        event.type = EventType::AIUI_CONNECTED;
        self->PostEvent(event);
    } else if (callback->type == AIUI_EVENT_SERVER_DISCONNECTED ||
               callback->type == AIUI_EVENT_CONNECTED_TIMEOUT) {
        event.type = EventType::AIUI_DISCONNECTED;
        self->PostEvent(event);
    } else if (callback->type == AIUI_EVENT_ERROR) {
        event.type = EventType::AIUI_ERROR;
        event.code = callback->general.code;
        self->PostEvent(event);
    } else if (callback->type == AIUI_EVENT_RESULT) {
        if (self->state_.load() != RobotAppState::SESSION_ACTIVE ||
            !self->aiui_.active.load()) {
            return;
        }
        aiui_event_result_t &result = callback->result;
        strlcpy(event.sid, result.session.sid, sizeof(event.sid));
        if (strcmp(result.params.sub, "event") == 0) {
            const char *cloud_event = result.params.key_data
                                          ? static_cast<const char *>(
                                                result.params.key_data)
                                          : "";
            ESP_LOGI(TAG, "AIUI cloud event: %s",
                     cloud_event[0] ? cloud_event : "<empty>");
            if (strcmp(cloud_event, "Vad=Bos") == 0) {
                self->aiui_.cloud_utterance_active.store(true);
            } else if (strcmp(cloud_event, "Vad=Eos") == 0 &&
                       self->aiui_.cloud_utterance_active.exchange(false)) {
                self->aiui_.awaiting_reply.store(true);
                self->aiui_.awaiting_reply_since_ms.store(
                    (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL));
                event.type = EventType::AIUI_SPEECH_END;
                if (self->PostEvent(event) != ESP_OK) {
                    self->aiui_.awaiting_reply.store(false);
                    self->aiui_.awaiting_reply_since_ms.store(0);
                }
            } else if (strstr(cloud_event, "=Silence") != nullptr) {
                event.type = EventType::AIUI_SILENCE;
                self->PostEvent(event);
            }
            return;
        }
        if ((result.params.key_data &&
             result.params.key_data_size > 1) ||
            strcmp(result.params.sub, "tts") == 0) {
            self->aiui_.turn_result_seen.store(true);
        }
        if (strcmp(result.params.sub, "tts") == 0) {
            if (self->suppress_music_failure_tts_.load() &&
                result.session.sid[0] != '\0' &&
                strcmp(result.session.sid,
                       self->suppressed_music_sid_) == 0) {
                if (result.params.dts == 2 || result.params.dts == 3) {
                    self->suppress_music_failure_tts_.store(false);
                    ESP_LOGI(TAG,
                             "suppressed AIUI music failure TTS reached "
                             "final frame: sid=%.47s",
                             result.session.sid);
                }
                return;
            }
            if (self->aiui_.discard_tts_until_final.load()) {
                self->aiui_.tts_last_chunk_ms.store(
                    (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL));
                if (result.params.dts == 2 || result.params.dts == 3) {
                    self->aiui_.discard_tts_until_final.store(false);
                    self->aiui_.tts_playing.store(false);
                    self->aiui_.tts_final_received.store(false);
                    self->aiui_.tts_last_chunk_ms.store(0);
                    self->aiui_.awaiting_reply.store(false);
                    self->aiui_.awaiting_reply_since_ms.store(0);
                    if (self->media_announcement_pending_.load()) {
                        Event announce = {};
                        announce.type = EventType::MEDIA_ANNOUNCE;
                        self->PostEvent(announce);
                    } else if (!self->media_active_.load()) {
                        Event drained = {};
                        drained.type = EventType::PLAYBACK_DRAINED;
                        drained.code = static_cast<int>(
                            self->playback_generation_.load());
                        self->PostEvent(drained);
                    }
                    ESP_LOGI(TAG,
                             "discarded AIUI music failure TTS reached "
                             "final frame");
                }
                return;
            }
            const bool was_playing =
                self->aiui_.tts_playing.exchange(true);
            if (!was_playing || result.params.dts == 0) {
                /*
                 * Invalidate any microphone send already in flight before
                 * closing the audio request below. Without this epoch, that
                 * sender can reopen the request after TTS has started and
                 * mix a second audio turn into the current TTS stream.
                 */
                self->aiui_.audio_gate_epoch.fetch_add(1);
                self->aiui_.tts_final_received.store(false);
            }
            if (result.params.dts == 2 || result.params.dts == 3) {
                /*
                 * Record the final frame before it is handed to the decoder.
                 * Playback can legitimately need several more seconds to
                 * drain; the missing-final watchdog must not tear down that
                 * valid stream while its tail is still in the speaker queue.
                 */
                self->aiui_.tts_final_received.store(true);
            }
            robot_audio_set_capture_paused(true);
            /*
             * Last-resort close for a lost/delayed cloud EOS event. This
             * keeps a valid partial TTS stream from stalling before its
             * mandatory status=2 packet.
             */
            if (self->aiui_.audio_request_open.exchange(false)) {
                if (aiui_send_audio(self->aiui_.handle, nullptr, 0) == 0) {
                    ESP_LOGW(TAG,
                             "TTS began before audio EOS; end frame queued");
                } else {
                    self->aiui_.connected.store(false);
                    Event disconnected = {};
                    disconnected.type = EventType::AIUI_DISCONNECTED;
                    disconnected.code = -1;
                    self->PostEvent(disconnected);
                }
            }
            self->HandleTtsResult(result);
            return;
        }
        event.type = EventType::AIUI_RESULT;
        event.dts = result.params.dts;
        strlcpy(event.sub, result.params.sub, sizeof(event.sub));
        if (result.params.key_data) {
            strlcpy(event.text, static_cast<const char *>(result.params.key_data),
                    sizeof(event.text));
        }
        if (strcmp(result.params.sub, "cbm_semantic") == 0 &&
            result.data && result.data_size > 1) {
            event.payload = heap_caps_malloc(
                result.data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!event.payload) {
                event.payload = heap_caps_malloc(
                    result.data_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (event.payload) {
                memcpy(event.payload, result.data, result.data_size);
                static_cast<char *>(event.payload)[result.data_size - 1] =
                    '\0';
            } else {
                ESP_LOGE(TAG,
                         "unable to copy cbm_semantic detail (%u bytes)",
                         (unsigned)result.data_size);
            }
        }
        if (self->PostEvent(event) != ESP_OK && event.payload) {
            heap_caps_free(event.payload);
        }
    }
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
                    self->media_active_.load() ||
                            self->media_pending_.load() ||
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
