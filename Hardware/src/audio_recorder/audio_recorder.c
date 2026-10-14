#include "audio_recorder/audio_recorder.h"

#include "portaudio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct audio_capture_context_s
{
    PaStream* stream;
    void* user_data;
    on_audio_captured cb;

    int channel;
    int sample;
    int audioFramesPerBuf;
    // short mic4_filter[640 * 6];
} audio_capture_context_t;

static int _handle_pa_input_stream(const void* input, void* output,
                                unsigned long frameCount,
                                const PaStreamCallbackTimeInfo* timeInfo,
                                PaStreamCallbackFlags statusFlags, void* handle)
{
    audio_capture_context_t* audio = (audio_capture_context_t*)handle;
    if (NULL == audio)
    {
        return paAbort;
    }
    audio->cb(audio->user_data, input, frameCount * sizeof(short));

    /*
    const short* data_raw = (const short*)input;
    short* filter = audio->mic4_filter;
    for (int i = 0; i < frameCount; i++)
    {
        filter[i * 6] = data_raw[i * 8 + 2];
        filter[i * 6 + 1] = data_raw[i * 8 + 3];
        filter[i * 6 + 2] = data_raw[i * 8 + 4];
        filter[i * 6 + 3] = data_raw[i * 8 + 5];
        filter[i * 6 + 4] = data_raw[i * 8 + 6];
        filter[i * 6 + 5] = data_raw[i * 8 + 7];
    }
    audio->cb(audio->user_data, filter, frameCount * 2 * 6);
    */
    return paContinue;
}

AUDIO_RECORDER_HANDLE audio_recorder_init(on_audio_captured cb, void* param)
{
    if (NULL == cb)
    {
        printf("err: audio_recorder_init callback function can not be null\n");
        return NULL;
    }
    PaError err = Pa_Initialize();
    if (err != paNoError)
    {
        printf("failed(%d) on Pa_Initialize", err);
        return NULL;
    }
    audio_capture_context_t* capture_context = (audio_capture_context_t*)malloc(sizeof(audio_capture_context_t));
    if (NULL == capture_context)
    {
        printf("failed on alloc audio_capture_context_t\n");
        return NULL;
    }
    memset(capture_context, 0, sizeof(audio_capture_context_t));
    capture_context->cb = cb;
    capture_context->user_data = param;
    capture_context->audioFramesPerBuf = 1 * 16 * 40; // * sizeof(short);//640; // 40ms
    capture_context->sample = 16000;
    return (AUDIO_RECORDER_HANDLE)capture_context;
}

int audio_recorder_start(AUDIO_RECORDER_HANDLE handle)
{
    if (NULL == handle)
    {
        return -1;
    }
    audio_capture_context_t* capture_context = (audio_capture_context_t*)handle;

    PaError err = paNoError;
    PaStreamParameters inputParameters;
    int count = Pa_GetDeviceCount();
    if (count <= 0)
    {
        printf("no default input dev\n");
        return -1;
    }

    PaDeviceIndex dev_index = Pa_GetDefaultInputDevice();
    if (dev_index == -1)
    {
        printf("can not find default input dev\n");
        return -1;
    }
    printf("open input dev: %d\n", dev_index);
    // const PaDeviceInfo* info = Pa_GetDeviceInfo(dev_index);
    inputParameters.device = dev_index;
    inputParameters.channelCount = 1;//info->maxInputChannels;
    inputParameters.sampleFormat = paInt16;
    inputParameters.suggestedLatency =
        Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;
    err = Pa_OpenStream(&capture_context->stream, &inputParameters, NULL,
                        capture_context->sample, capture_context->audioFramesPerBuf,
                        paClipOff,
                        _handle_pa_input_stream, capture_context);
    if (err != paNoError)
    {
        printf("err: failed(%d) on open pa\n", err);
        return err;
    }
    if (paNoError != (err = Pa_StartStream(capture_context->stream)))
    {
        printf("err: failed(%d) on start pa\n", err);
        return err;
    }
    return err;
}

int audio_recorder_uninit(AUDIO_RECORDER_HANDLE handle)
{
    if (NULL == handle)
    {
       return 0;
    }
    audio_capture_context_t* capture_context = (audio_capture_context_t*)handle;
    if (capture_context->stream != NULL)
    {
        Pa_StopStream(capture_context->stream);
        Pa_CloseStream(capture_context->stream);
        capture_context->stream = NULL;
    }
    Pa_Terminate();

    printf("audio_recorder uninit done\n");
    return 0;
}