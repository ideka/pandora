// Plays the DSP's audio DMA output (32 kHz stereo) on the host's default audio device via SDL.
#pragma once

#include <vector>

#include "guest.h"

struct SDL_AudioStream;

class AudioOut {
public:
    ~AudioOut() { close(); }
    bool open();
    void close();

    // A 32-byte audio DMA block: 8 frames of big-endian s16, right channel first.
    void push(const u8* block);
    // Output gain, 0..1 (applied to the guest's samples).
    void set_volume(float volume) { gain_ = volume; }
    // The Wii Remote speaker's stream when no remote plays it: packets of 4-bit Yamaha ADPCM at
    // the given sample rate, decoded and mixed into both channels.
    void push_speaker(const u8* packet, size_t len, float rate);
    void reset_speaker();

    // Milliseconds of audio queued for the device, or -1 when there is no output.
    int queued_ms() const;

private:
    SDL_AudioStream* stream_ = nullptr;
    std::vector<s16> pending_;  // batched before handing to SDL
    bool playing_ = false;
    float gain_ = 1.0f;
    u32 underruns_ = 0, dropped_ = 0;
    // The remote speaker's decoded samples, resampled into the output as it is pushed.
    std::vector<s16> speaker_;
    size_t speaker_read_ = 0;
    double speaker_pos_ = 0, speaker_step_ = 6000.0 / 32000.0;
    s32 adpcm_predictor_ = 0, adpcm_step_ = 127;
};
