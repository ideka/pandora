#include "audio_dump.h"

namespace {

constexpr u32 SAMPLE_RATE = 32000;

void put16(std::FILE* f, u16 v) {
    std::fputc(v & 0xFF, f);
    std::fputc(v >> 8, f);
}
void put32(std::FILE* f, u32 v) {
    put16(f, (u16)v);
    put16(f, (u16)(v >> 16));
}

}  // namespace

bool AudioDump::open(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_)
        return false;
    std::fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, file_);
    put32(file_, 16);
    put16(file_, 1);  // PCM
    put16(file_, 2);  // stereo
    put32(file_, SAMPLE_RATE);
    put32(file_, SAMPLE_RATE * 4);
    put16(file_, 4);
    put16(file_, 16);
    std::fwrite("data\0\0\0\0", 1, 8, file_);
    return true;
}

void AudioDump::close() {
    if (!file_)
        return;
    std::fseek(file_, 4, SEEK_SET);
    put32(file_, 36 + frames_ * 4);
    std::fseek(file_, 40, SEEK_SET);
    put32(file_, frames_ * 4);
    std::fclose(file_);
    file_ = nullptr;
}

void AudioDump::sink(void* self, const u8* b) {
    auto* d = static_cast<AudioDump*>(self);
    if (!d->file_)
        return;
    for (int i = 0; i < 8; i++, b += 4) {
        u16 right = (u16)(b[0] << 8 | b[1]), left = (u16)(b[2] << 8 | b[3]);
        put16(d->file_, left);
        put16(d->file_, right);
    }
    d->frames_ += 8;
}
