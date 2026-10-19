// Writes the DSP's audio DMA output (32 kHz stereo) to a WAV file, for checking the mixer.
#pragma once

#include <cstdio>
#include <string>

#include "guest.h"

class AudioDump {
public:
    ~AudioDump() { close(); }
    bool open(const std::string& path);
    void close();

    // A 32-byte audio DMA block: 8 frames of big-endian s16, right channel first.
    static void sink(void* self, const u8* block);

private:
    std::FILE* file_ = nullptr;
    u32 frames_ = 0;
};
