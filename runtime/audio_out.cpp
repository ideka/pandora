#include "audio_out.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
// The remote speaker's stream is high-passed at ~40 Hz (as the remote's small speaker would): its
// ADPCM leaves a slowly wandering offset that would otherwise click at every sound's start and end.
const float SPEAKER_HIGH_PASS = std::exp(-2.0f * 3.14159265f * 40.0f / RATE);

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
        // The remote speaker's stream, linearly resampled (it is mono and ~3-6 kHz). It comes in
        // bursts (only while remote sounds play, a packet at a time): while none is waiting, the
        // last value holds, so a late packet or the end of a sound is no step to 0.
        if (speaker_read_ + 1 < speaker_.size()) {
            const size_t k = speaker_read_;
            const double f = speaker_pos_;
            speaker_hold_ = (float)(speaker_[k] * (1.0 - f) + speaker_[k + 1] * f);
            speaker_pos_ += speaker_step_;
            while (speaker_pos_ >= 1.0) {
                speaker_pos_ -= 1.0;
                speaker_read_++;
            }
        }
        speaker_hp_ = SPEAKER_HIGH_PASS * (speaker_hp_ + speaker_hold_ - speaker_last_in_);
        speaker_last_in_ = speaker_hold_;
        const int spk = (int)std::lround(speaker_hp_);
        pending_.push_back(scale((s16)(b[2] << 8 | b[3]) + spk));  // left
        pending_.push_back(scale((s16)(b[0] << 8 | b[1]) + spk));  // right
    }
    if (speaker_read_ > 4096) {
        speaker_.erase(speaker_.begin(), speaker_.begin() + (ptrdiff_t)speaker_read_);
        speaker_read_ = 0;
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

// 4-bit Yamaha ADPCM (as ffmpeg and Dolphin decode it), high nibble first; the remote plays the
// game's configured volume 0x40 of 0x7F.
void AudioOut::push_speaker(const u8* packet, size_t len, float rate) {
    static const s32 DIFF[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
    static const s32 SCALE[16] = {230, 230, 230, 230, 307, 409, 512, 614, 230, 230, 230, 230, 307, 409, 512, 614};
    auto expand = [&](u8 nibble) {
        adpcm_predictor_ = std::clamp(adpcm_predictor_ + adpcm_step_ * DIFF[nibble] / 8, -32768, 32767);
        adpcm_step_ = std::clamp((adpcm_step_ * SCALE[nibble]) >> 8, 127, 24576);
        return (s16)(adpcm_predictor_ * 0x40 / 0x7F);
    };
    if (speaker_.size() - speaker_read_ > 24000)  // half a second behind: the output is not consuming
        return;
    const size_t first = speaker_.size();
    for (size_t i = 0; i < len; i++) {
        speaker_.push_back(expand(packet[i] >> 4));
        speaker_.push_back(expand(packet[i] & 0xF));
    }
    // PANDORA_SPEAKER_DUMP=<file>: the decoded stream as raw mono s16 (test runs).
    static FILE* dump = std::getenv("PANDORA_SPEAKER_DUMP") ? std::fopen(std::getenv("PANDORA_SPEAKER_DUMP"), "wb") : nullptr;
    if (dump) {
        std::fwrite(speaker_.data() + first, sizeof(s16), speaker_.size() - first, dump);
        std::fflush(dump);
    }
    speaker_step_ = std::clamp((double)rate, 1000.0, 16000.0) / RATE;
}

void AudioOut::reset_speaker() {
    adpcm_predictor_ = 0;
    adpcm_step_ = 127;
}
