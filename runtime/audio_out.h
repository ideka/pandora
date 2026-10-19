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

private:
    SDL_AudioStream* stream_ = nullptr;
    std::vector<s16> pending_;  // batched before handing to SDL
};
