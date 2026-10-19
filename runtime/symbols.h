// Function symbols loaded from the "ADDR SIZE NAME" map exported by tools/export_map.py.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "guest.h"

class Symbols {
public:
    bool load(const std::string& path);

    // "name+0xoff" for an address inside a known function, otherwise the hex address.
    std::string describe(u32 addr) const;
    std::optional<u32> find(std::string_view name) const;
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        u32 addr;
        u32 size;
        std::string name;
    };
    std::vector<Entry> entries_;  // sorted by address
    std::unordered_map<std::string, u32> by_name_;
};
