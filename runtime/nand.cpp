#include "nand.h"

#include <fstream>
#include <vector>

#include "guest.h"
#include "log.h"

bool Nand::open(const std::filesystem::path& root) {
    root_ = root;
    std::error_code ec;
    std::filesystem::create_directories(root_ / "shared2/sys", ec);
    std::filesystem::create_directories(root_ / "tmp", ec);
    if (ec) {
        LOG_ERROR("nand", "cannot create %s: %s", root_.string().c_str(), ec.message().c_str());
        return false;
    }
    auto sysconf = root_ / "shared2/sys/SYSCONF";
    if (!std::filesystem::exists(sysconf)) {
        write_default_sysconf(sysconf);
        LOG_INFO("nand", "created default %s", sysconf.string().c_str());
    }
    return true;
}

std::filesystem::path Nand::host_path(const std::string& nand_path) const {
    if (nand_path.empty() || nand_path[0] != '/' || nand_path.find("..") != std::string::npos)
        return {};
    return root_ / nand_path.substr(1);
}

// SYSCONF layout and defaults follow Dolphin's SysConf.cpp: "SCv0", entry count, u16 offsets
// (plus one past the end), entries of {type << 5 | (name length - 1), name, data}, padded to
// 0x4000 bytes with an "SCed" footer.
void Nand::write_default_sysconf(const std::filesystem::path& path) const {
    enum : u8 { BIG_ARRAY = 1, SMALL_ARRAY = 2, BYTE = 3, LONG = 5, BOOL = 7 };
    struct Entry {
        u8 type;
        std::string name;
        std::vector<u8> data;
    };
    std::vector<u8> nick = {0, 'P', 0, 'a', 0, 'n', 0, 'd', 0, 'o', 0, 'r', 0, 'a'};
    nick.resize(22);
    nick[21] = 7;
    std::vector<u8> sadr(0x1008);
    sadr[0] = 0x6E;  // country: United Kingdom (PAL)
    std::vector<u8> pc(0x4A);
    pc[1] = 0x04;
    pc[2] = 0x14;
    const std::vector<Entry> entries = {
        {BIG_ARRAY, "BT.DINF", std::vector<u8>(0x461)},
        {BIG_ARRAY, "BT.CDIF", std::vector<u8>(0x205)},
        {LONG, "BT.SENS", {0, 0, 0, 3}},
        {BYTE, "BT.BAR", {1}},
        {BYTE, "BT.SPKV", {0x58}},
        {BYTE, "BT.MOT", {1}},
        {SMALL_ARRAY, "IPL.NIK", nick},
        {BYTE, "IPL.LNG", {1}},  // English
        {BIG_ARRAY, "IPL.SADR", sadr},
        {SMALL_ARRAY, "IPL.PC", pc},
        {LONG, "IPL.CB", {0, 0, 0, 0}},
        {BYTE, "IPL.AR", {1}},   // 16:9
        {BYTE, "IPL.SSV", {1}},
        {BOOL, "IPL.CD", {0}},
        {BOOL, "IPL.CD2", {0}},
        {BOOL, "IPL.EULA", {1}},
        {BYTE, "IPL.UPT", {2}},
        {BYTE, "IPL.PGS", {0}},  // interlaced
        {BYTE, "IPL.E60", {1}},  // EuRGB60 allowed
        {BYTE, "IPL.DH", {0}},
        {LONG, "IPL.INC", {0, 0, 0, 8}},
        {LONG, "IPL.FRC", {0, 0, 0, 0x28}},
        {SMALL_ARRAY, "IPL.IDL", {0, 1}},
        {LONG, "NET.WCFG", {0, 0, 0, 1}},
        {LONG, "NET.CTPC", {0, 0, 0, 0}},
        {BYTE, "WWW.RST", {0}},
        {BOOL, "MPLS.MOVIE", {1}},
    };

    std::vector<u8> out = {'S', 'C', 'v', '0'};
    auto put16 = [](std::vector<u8>& v, size_t v16) {
        v.push_back((u8)(v16 >> 8));
        v.push_back((u8)v16);
    };
    put16(out, entries.size());
    size_t base = out.size() + 2 * (entries.size() + 1);
    std::vector<u8> body;
    for (const Entry& e : entries) {
        put16(out, base + body.size());
        body.push_back((u8)(e.type << 5 | (e.name.size() - 1)));
        body.insert(body.end(), e.name.begin(), e.name.end());
        if (e.type == BIG_ARRAY)
            put16(body, e.data.size() - 1);
        else if (e.type == SMALL_ARRAY)
            body.push_back((u8)(e.data.size() - 1));
        body.insert(body.end(), e.data.begin(), e.data.end());
    }
    put16(out, base + body.size());
    out.insert(out.end(), body.begin(), body.end());
    out.resize(0x4000);
    std::copy_n("SCed", 4, out.end() - 4);
    std::ofstream(path, std::ios::binary).write((const char*)out.data(), (std::streamsize)out.size());
}
