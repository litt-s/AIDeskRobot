#include "audio_player/audio_player.h"
#include <queue>
#include <memory>
#include <cstring>
#include <portaudio.h>
#include <chrono>
#include <mutex>
#include <iostream>

class AudioFrame {
public:
    AudioFrame(const char* data, int length)
        : left_(length), data_(new char[length]), offset_(0) {
        if (length > 0) {
            std::memcpy(data_.get(), data, length);
        }
    }

    std::shared_ptr<char> data() const { return data_; }
    int left() const { return left_; }
    int offset() const { return offset_; }

    void cost(int len) {
        left_ -= len;
        offset_ += len;
    }

private:
    int left_;
    int offset_;
    std::shared_ptr<char> data_;
};

class AudioPlay {
public:
    AudioPlay();
    ~AudioPlay();

    bool Start(int channel, int bit, int sample);
    bool Stop();
    bool AddFrame(const void* data, int len);
    void AudioStreamEnded() { stream_end_ = true; }

private:
    static int playCallback(const void* inputBuffer, void* outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags, void* userData);

    PaStream* stream_;
    int channel_;
    int bit_;
    bool stream_end_;
    AudioFrame current_frame_;
    std::queue<AudioFrame> audio_buffer_;
    const int kPlayFramesPerBuffer = 256;

    bool play_flag_;
    std::mutex buffer_mutex_;  // 用于保护audio_buffer_的锁
};


AudioPlay::AudioPlay()
    : stream_(nullptr), stream_end_(false), current_frame_(nullptr, 0), play_flag_(false)
{
    Pa_Initialize();
}

AudioPlay::~AudioPlay()
{
    Pa_Terminate();
}

bool AudioPlay::Start(int channel, int bit, int sampleRate)
{
    if (nullptr != stream_)
    {
        std::cout << "player stream already started!!!" << std::endl;
        return false;
    }
    channel_ = channel;
    bit_ = bit;
    PaStreamParameters outParams;
    outParams.device = Pa_GetDefaultOutputDevice(); // Default output device
    outParams.channelCount = channel;               // Number of channels

    // Set sample format based on bit depth
    switch (bit)
    {
    case 16:
        outParams.sampleFormat = paInt16;
        break;
    case 32:
        outParams.sampleFormat = paFloat32;
        break;
    default:
        std::cerr << "Unsupported bit depth: " << bit << std::endl;
        return false;
    }

    outParams.suggestedLatency = Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    // Open output stream
    PaError err = Pa_OpenStream(&stream_,
                                nullptr,
                                &outParams,
                                sampleRate,
                                kPlayFramesPerBuffer,
                                paClipOff,
                                playCallback,
                                this);
    if (err != paNoError)
    {
        std::cerr << "Error: Open AudioPlay stream failed. Error Message: "
                  << Pa_GetErrorText(err) << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        // Clear the audio buffer
        std::queue<AudioFrame> empty_queue;
        std::swap(audio_buffer_, empty_queue);
    }

    stream_end_ = false;
    play_flag_ = true;
    std::cout << "Audio play started." << std::endl;
    return true;
}

bool AudioPlay::Stop()
{
    // Stop and close PortAudio stream
    stream_end_ = true;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        std::queue<AudioFrame> empty_queue;
        std::swap(audio_buffer_, empty_queue);
    }

    current_frame_ = AudioFrame(nullptr, 0);
    if (stream_)
    {
        if (Pa_IsStreamActive(stream_))
        {
            std::cout << " --> Pa_StopStream " << std::endl;
            Pa_StopStream(stream_);
            std::cout << " <-- Pa_StopStream " << std::endl;
        }
        std::cout << " --> Pa_CloseStream " << std::endl;
        Pa_CloseStream(stream_);
        std::cout << " <-- Pa_CloseStream " << std::endl;
        stream_ = nullptr;
        std::cout << "Audio play stopped done." << std::endl;
    }
    play_flag_ = false;
    return true;
}

bool AudioPlay::AddFrame(const void *data, int len)
{
    if (len <= 0 || !play_flag_)
    {
        return false;
    }

    AudioFrame frame(static_cast<const char *>(data), len);
    {
        // Lock the buffer before modifying
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        audio_buffer_.push(std::move(frame));
    }
    // If stream is not active, start it
    if (!Pa_IsStreamActive(stream_))
    {
        PaError err = Pa_StartStream(stream_);
        if (err != paNoError)
        {
            std::cerr << "Error: Start AudioPlay stream failed. Error Message: "
                      << Pa_GetErrorText(err) << std::endl;
            return false;
        }
    }

    return true;
}

// Play callback function
int AudioPlay::playCallback(const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo *timeInfo,
                            PaStreamCallbackFlags statusFlags, void *userData)
{
    AudioPlay *audio_play = static_cast<AudioPlay *>(userData);
    char *out = static_cast<char *>(outputBuffer);
    int once_size = framesPerBuffer * audio_play->channel_ * audio_play->bit_ / 8;
    std::memset(outputBuffer, 0, once_size); // Clear the output buffer

    int left = 0;
    int need_size = once_size - left;

    // Lock the buffer before modifying it
    std::lock_guard<std::mutex> lock(audio_play->buffer_mutex_);

    while (need_size > 0)
    {
        left = audio_play->current_frame_.left();
        if (left >= need_size)
        {
            // Copy data to the output buffer
            std::memcpy(out + once_size - need_size,
                        audio_play->current_frame_.data().get() + audio_play->current_frame_.offset(),
                        need_size);
            audio_play->current_frame_.cost(need_size);
            break;
        }

        std::memcpy(out + (once_size - need_size),
                    audio_play->current_frame_.data().get() + audio_play->current_frame_.offset(),
                    left);
        need_size -= left;


        if (audio_play->stream_end_)
        {
            std::cout << "Audio playback complete." << std::endl;
            return paComplete;
        }

        /*
        // If audio buffer is empty, check if stream should end
        static std::chrono::steady_clock::time_point start_time;
        static bool timer_started = false;
        if (audio_play->audio_buffer_.empty())
        {
            if (audio_play->stream_end_)
            {
                std::cout << "Audio playback complete." << std::endl;
                return paComplete;
            }

            // Start timer if it's not already running
            if (!timer_started)
            {
                start_time = std::chrono::steady_clock::now();
                timer_started = true;
            }
            else
            {
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();

                // End stream after 2 seconds of no audio
                if (elapsed_time >= 2)
                {
                    audio_play->stream_end_ = true;
                }
            }

            break;
        }
        else
        {
            timer_started = false;
        }
        */

        // Get next audio frame from the buffer
        if (!audio_play->audio_buffer_.empty())
        {
            audio_play->current_frame_ = audio_play->audio_buffer_.front();
            audio_play->audio_buffer_.pop();
        }
    }

    return paContinue;
}

int audio_player_init(AUDIO_PLAYER_HANDLE *handle_p)
{
    AudioPlay *player = new AudioPlay();
     *handle_p = (void *)player;
    return 0;
}

int audio_player_start(AUDIO_PLAYER_HANDLE handle)
{
    AudioPlay *player = (AudioPlay *)handle;
    bool succeed = player->Start(1, 16, 16000);
    return succeed ? 0 : -1;
}

int audio_player_write(AUDIO_PLAYER_HANDLE handle, const void *audio, int len)
{
    AudioPlay *player = (AudioPlay *)handle;
    bool succeed = player->AddFrame(audio, len);
    return succeed ? 0 : -1;
}

int audio_player_stop(AUDIO_PLAYER_HANDLE handle)
{
    AudioPlay *player = (AudioPlay *)handle;
    bool succeed = player->Stop();
    return succeed ? 0 : -1;
}

int audio_player_uninit(AUDIO_PLAYER_HANDLE handle)
{
    AudioPlay *player = (AudioPlay *)handle;

    delete player;
    return 0;
}