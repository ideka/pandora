// Minimal PNG writer (RGBA8, uncompressed deflate) for debug screenshots.
#pragma once

#include <cstdint>
#include <string>

bool write_png_rgba(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height);
