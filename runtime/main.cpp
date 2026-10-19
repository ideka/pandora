#include <cstdlib>
#include <cstring>

#include "log.h"
#include "runtime.h"

int g_log_level = 1;

static void usage() {
    std::fprintf(stderr,
                 "usage: pandora --disc <extracted partition> [--nand <dir>] [--map <file>] [--max-blocks N] [--audio-dump <wav>] [--watch <symbol>]... [--dump <hexaddr>[:len]]... [-v|-vv|-vvv] "
                 "[--trace]\n");
}

int main(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (!std::strcmp(a, "--disc") && i + 1 < argc)
            opts.disc = next();
        else if (!std::strcmp(a, "--nand") && i + 1 < argc)
            opts.nand = next();
        else if (!std::strcmp(a, "--map") && i + 1 < argc)
            opts.map = next();
        else if (!std::strcmp(a, "--max-blocks") && i + 1 < argc)
            opts.max_blocks = std::strtoull(next(), nullptr, 0);
        else if (!std::strcmp(a, "--audio-dump") && i + 1 < argc)
            opts.audio_dump = next();
        else if (!std::strcmp(a, "--watch") && i + 1 < argc)
            opts.watch.push_back(next());
        else if (!std::strcmp(a, "--dump") && i + 1 < argc) {
            char* end;
            u32 addr = (u32)std::strtoul(next(), &end, 16);
            opts.dumps.emplace_back(addr, *end == ':' ? (u32)std::strtoul(end + 1, nullptr, 0) : 64u);
        } else if (!std::strcmp(a, "--watch-write") && i + 1 < argc)
            opts.watch_writes.push_back((u32)std::strtoul(next(), nullptr, 16));
        else if (!std::strcmp(a, "--profile"))
            opts.profile = true;
        else if (!std::strcmp(a, "--trace"))
            opts.trace_calls = true;
        else if (!std::strncmp(a, "-v", 2) && std::strspn(a + 1, "v") == std::strlen(a + 1))
            g_log_level = 1 + (int)std::strlen(a + 1);
        else {
            usage();
            return 2;
        }
    }
    if (opts.disc.empty()) {
        usage();
        return 2;
    }

    Runtime rt(opts);
    if (!rt.boot())
        return 1;
    return rt.run();
}
