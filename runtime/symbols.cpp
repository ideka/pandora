#include "symbols.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

bool Symbols::load(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string addr, size, name;
        if (!(ss >> addr >> size >> name))
            continue;
        Entry e{(u32)std::stoul(addr, nullptr, 16), (u32)std::stoul(size, nullptr, 16), name};
        by_name_.emplace(e.name, e.addr);
        entries_.push_back(std::move(e));
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.addr < b.addr; });
    return !entries_.empty();
}

std::string Symbols::describe(u32 addr) const {
    char buf[32];
    auto it = std::upper_bound(entries_.begin(), entries_.end(), addr,
                               [](u32 a, const Entry& e) { return a < e.addr; });
    if (it != entries_.begin()) {
        const Entry& e = *std::prev(it);
        if (addr < e.addr + std::max<u32>(e.size, 4)) {
            if (addr == e.addr)
                return e.name;
            std::snprintf(buf, sizeof buf, "+0x%X", addr - e.addr);
            return e.name + buf;
        }
    }
    std::snprintf(buf, sizeof buf, "0x%08X", addr);
    return buf;
}

std::optional<u32> Symbols::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it == by_name_.end())
        return std::nullopt;
    return it->second;
}
