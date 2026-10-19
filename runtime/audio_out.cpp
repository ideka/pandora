#include "audio_out.h"

#include <SDL3/SDL.h>

#include "log.h"

namespace {

constexpr int RATE = 32000;
constexpr size_t BATCH_FRAMES = 256;  // 8 ms
// Queued audio beyond this is dropped: the guest is running ahead of real time, and letting the
// queue grow would add latency without bound.
constexpr int MAX_QUEUED_BYTES = RATE * 4 * 120 / 1000;  // 120 ms

}  // namespace

bool AudioOut::open() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR("audio", "cannot initialise SDL audio: %s", SDL_GetError());
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, RATE};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        LOG_ERROR("audio", "cannot open audio device: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    pending_.reserve(BATCH_FRAMES * 2);
    LOG_INFO("audio", "output on %s", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_)));
    return true;
}

void AudioOut::close() {
    if (!stream_)
        return;
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::push(const u8* b) {
    if (!stream_)
        return;
    for (int i = 0; i < 8; i++, b += 4) {
        pending_.push_back((s16)(b[2] << 8 | b[3]));  // left
        pending_.push_back((s16)(b[0] << 8 | b[1]));  // right
    }
    if (pending_.size() < BATCH_FRAMES * 2)
        return;
    if (SDL_GetAudioStreamQueued(stream_) < MAX_QUEUED_BYTES)
        SDL_PutAudioStreamData(stream_, pending_.data(), (int)(pending_.size() * sizeof(s16)));
    pending_.clear();
}
