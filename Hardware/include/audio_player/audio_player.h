#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#ifdef __cplusplus
extern "C" {
#endif

    typedef void *AUDIO_PLAYER_HANDLE;

    int audio_player_init(AUDIO_PLAYER_HANDLE *handle_p);

    int audio_player_start(AUDIO_PLAYER_HANDLE handle);

    int audio_player_write(AUDIO_PLAYER_HANDLE handle, const void *audio, int len);

    int audio_player_stop(AUDIO_PLAYER_HANDLE handle);

    int audio_player_uninit(AUDIO_PLAYER_HANDLE handle);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_PLAYER_H_
