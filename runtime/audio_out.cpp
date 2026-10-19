#include "audio_out.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "log.h"

namespace {

constexpr int RATE = 32000;
constexpr size_t BATCH_FRAMES = 256;  // 8 ms
constexpr int BYTES_PER_MS = RATE * 4 / 1000;
// The guest produces audio in bursts (a frame's worth while it runs, nothing while it waits for
// the next frame), so playback starts once this much is queued, and pauses to refill after an
// underrun rather than playing a stream of tiny gaps.
constexpr int PREBUFFER_BYTES = 80 * BYTES_PER_MS;
// Queued audio beyond this is dropped (the guest is running ahead of real time); letting the
// queue grow would add latency without bound.
constexpr int MAX_QUEUED_BYTES = 250 * BYTES_PER_MS;

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
    playing_ = false;  // starts once PREBUFFER_BYTES are queued
    pending_.reserve(BATCH_FRAMES * 2);
    LOG_INFO("audio", "output on %s", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_)));
    return true;
}

void AudioOut::close() {
    if (!stream_)
        return;
    LOG_INFO("audio", "%u underruns, %u batches dropped", underruns_, dropped_);
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::push(const u8* b) {
    if (!stream_)
        return;
    auto scale = [this](int v) { return (s16)std::clamp((int)std::lround(v * gain_), -32768, 32767); };
    for (int i = 0; i < 8; i++, b += 4) {
        pending_.push_back(scale((s16)(b[2] << 8 | b[3])));  // left
        pending_.push_back(scale((s16)(b[0] << 8 | b[1])));  // right
    }
    if (pending_.size() < BATCH_FRAMES * 2)
        return;
    int queued = SDL_GetAudioStreamQueued(stream_);
    if (playing_ && queued == 0) {  // underrun: refill before playing on
        SDL_PauseAudioStreamDevice(stream_);
        playing_ = false;
        underruns_++;
    }
    if (queued < MAX_QUEUED_BYTES) {
        SDL_PutAudioStreamData(stream_, pending_.data(), (int)(pending_.size() * sizeof(s16)));
        queued += (int)(pending_.size() * sizeof(s16));
    } else {
        dropped_++;
    }
    pending_.clear();
    if (!playing_ && queued >= PREBUFFER_BYTES) {
        SDL_ResumeAudioStreamDevice(stream_);
        playing_ = true;
    }
}

int AudioOut::queued_ms() const { return stream_ ? SDL_GetAudioStreamQueued(stream_) / BYTES_PER_MS : -1; }
