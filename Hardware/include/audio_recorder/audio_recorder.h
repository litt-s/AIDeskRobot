#ifndef __AUDIO_RECORDER_H__
#define __AUDIO_RECORDER_H__

typedef void *AUDIO_RECORDER_HANDLE;
typedef void (*on_audio_captured)(const void *user_param, const void *audio, int length);

#ifdef __cplusplus
extern "C" {
#endif

AUDIO_RECORDER_HANDLE audio_recorder_init(on_audio_captured cb, void *param);

int audio_recorder_start(AUDIO_RECORDER_HANDLE handle);

int audio_recorder_uninit(AUDIO_RECORDER_HANDLE handle);

#ifdef __cplusplus
}
#endif

#endif
