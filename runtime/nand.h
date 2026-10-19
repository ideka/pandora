// Emulated NAND backed by a host directory (saves, SYSCONF, WiiConnect24 data).
#pragma once

#include <filesystem>
#include <string>

class Nand {
public:
    // Creates the root and a default /shared2/sys/SYSCONF if missing.
    bool open(const std::filesystem::path& root);

    // Host path for a NAND path such as "/title/00010000/53583350/data/save.bin", or empty if the
    // path is invalid (not absolute, or escapes the root).
    std::filesystem::path host_path(const std::string& nand_path) const;

private:
    void write_default_sysconf(const std::filesystem::path& path) const;

    std::filesystem::path root_;
};
