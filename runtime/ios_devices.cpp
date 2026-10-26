// IOS device implementations. Protocols follow Dolphin's IOS HLE (Source/Core/Core/IOS).
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "ios.h"
#include "log.h"
#include "memory.h"
#include "runtime.h"

namespace {

CPUState* cpu_of(Ios& ios) { return &ios.rt().cpu; }

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

void copy_to_guest(CPUState* c, u32 addr, const void* src, size_t len) {
    if (u8* dst = mem::ptr(c, addr, (u32)len))
        std::memcpy(dst, src, len);
}

// --- /dev/di ---------------------------------------------------------------------------------

class DiDevice : public Device {
public:
    explicit DiDevice(Ios& ios) : Device("/dev/di"), ios_(ios) {}

    // DI results: 1 success, 2 drive error, 128 bad argument.
    s32 ioctl(const Request&, u32 cmd, u32 in, u32, u32 out, u32 out_len) override {
        CPUState* c = cpu_of(ios_);
        Disc& disc = ios_.disc();
        switch (cmd) {
        case 0x12:  // Inquiry
            if (u8* p = mem::ptr(c, out, 0x20)) std::memset(p, 0, 0x20);
            return 1;
        case 0x70:  // ReadDiskID
            copy_to_guest(c, out, disc.header().data(), 0x20);
            return 1;
        case 0x71: {  // Read (decrypted, partition-relative; offset in words)
            u32 len = mem::read32(c, in + 4);
            u64 pos = (u64)mem::read32(c, in + 8) << 2;
            if (!partition_open_) {
                LOG_ERROR("di", "read with no partition open");
                return 32;
            }
            len = std::min(len, out_len);
            u8* dst = mem::ptr(c, out, len);
            if (!dst || !disc.read_partition(pos, len, dst))
                return 2;
            LOG_DEBUG("di", "read %08llX +%X -> %08X", (unsigned long long)pos, len, out);
            return 1;
        }
        case 0x8D: {  // UnencryptedRead (raw disc system area)
            u32 len = std::min(mem::read32(c, in + 4), out_len);
            u64 pos = (u64)mem::read32(c, in + 8) << 2;
            // The SDK's "error 001" check reads past the end of the disc and requires the
            // drive to reject it.
            if (pos + len > SINGLE_LAYER_BYTES) {
                last_error_ = ERROR_BLOCK_OUT_OF_RANGE;
                return 2;
            }
            u8* dst = mem::ptr(c, out, len);
            return dst && disc.read_raw(pos, len, dst) ? 1 : 2;
        }
        case 0xA4:  // ReportKey: retail drives reject it, and games check that they do
            last_error_ = ERROR_INVALID_COMMAND;
            return 2;
        case 0xE0:  // RequestError
            mem::write32(c, out, last_error_);
            last_error_ = 0;
            return 1;
        case 0x7A:  // GetCoverRegister: cover closed
        case 0x84:  // GetImmBuf
        case 0x95:  // GetStatusRegister
        case 0x96:  // GetControlRegister
            mem::write32(c, out, 0);
            return 1;
        case 0x88:  // GetCoverStatus: disc inserted
            mem::write32(c, out, 2);
            return 1;
        case 0x8C:  // ClosePartition
            partition_open_ = false;
            return 1;
        case 0x7E: case 0x85: case 0x86: case 0x87: case 0x89: case 0x8A: case 0xAB: case 0xE3: case 0xE4:
            return 1;  // NotifyReset, interrupt masks, Reset, Seek, StopMotor, AudioBufferConfig
        }
        LOG_INFO("di", "unhandled ioctl 0x%X", cmd);
        return 128;
    }

    s32 ioctlv(const Request&, u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        CPUState* c = cpu_of(ios_);
        if (cmd == 0x8B && in.size() == 3 && io.size() == 2) {  // OpenPartition
            u64 off = (u64)mem::read32(c, in[0].addr + 4) << 2;
            if (off != Disc::GAME_PARTITION_OFFSET)
                LOG_ERROR("di", "open of unknown partition %llX", (unsigned long long)off);
            const auto& tmd = ios_.disc().tmd();
            copy_to_guest(c, io[0].addr, tmd.data(), std::min<size_t>(tmd.size(), io[0].size));
            mem::write32(c, io[1].addr, 0);  // ES result
            partition_open_ = true;
            return 1;
        }
        LOG_INFO("di", "unhandled ioctlv 0x%X", cmd);
        return 128;
    }

private:
    static constexpr u64 SINGLE_LAYER_BYTES = 0x118240000;  // 4.7 GB, 0x230480 sectors
    static constexpr u32 ERROR_BLOCK_OUT_OF_RANGE = 0x00052100;
    static constexpr u32 ERROR_INVALID_COMMAND = 0x00052000;

    Ios& ios_;
    bool partition_open_ = true;  // the system menu/apploader leave the game partition open
    u32 last_error_ = 0;
};

// --- /dev/stm --------------------------------------------------------------------------------

class StmImmediate : public Device {
public:
    StmImmediate() : Device("/dev/stm/immediate") {}
    s32 ioctl(const Request&, u32 cmd, u32, u32, u32, u32) override {
        LOG_DEBUG("stm", "immediate ioctl 0x%X", cmd);
        return IOS_OK;
    }
};

// Event hook (power/reset button): the request completes only when an event occurs.
class StmEventHook : public Device {
public:
    StmEventHook() : Device("/dev/stm/eventhook") {}
    s32 ioctl(const Request&, u32 cmd, u32, u32, u32, u32) override {
        return cmd == 0x1000 ? IOS_NO_REPLY : IOS_EINVAL;
    }
};

// --- /dev/es ---------------------------------------------------------------------------------

class EsDevice : public Device {
public:
    explicit EsDevice(Ios& ios) : Device("/dev/es"), ios_(ios) {}

    s32 ioctlv(const Request&, u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        CPUState* c = cpu_of(ios_);
        const auto& ticket = ios_.disc().ticket();
        const auto& tmd = ios_.disc().tmd();
        u32 tid_hi = be32(&ticket[0x1DC]), tid_lo = be32(&ticket[0x1E0]);
        switch (cmd) {
        case 0x20:  // GetTitleID
            mem::write32(c, io[0].addr, tid_hi);
            mem::write32(c, io[0].addr + 4, tid_lo);
            return IOS_OK;
        case 0x1D: {  // GetTitleDir (data directory)
            char path[32];
            std::snprintf(path, sizeof path, "/title/%08x/%08x/data", mem::read32(c, in[0].addr),
                          mem::read32(c, in[0].addr + 4));
            copy_to_guest(c, io[0].addr, path, std::strlen(path) + 1);
            return IOS_OK;
        }
        case 0x1B: {  // DIGetTicketView: version byte + ticket from ticket_id (0x1D0)
            u8 view[0xD8] = {};
            view[0] = ticket[0x1BC];
            std::memcpy(view + 4, &ticket[0x1D0], sizeof view - 4);
            copy_to_guest(c, io[0].addr, view, sizeof view);
            return IOS_OK;
        }
        case 0x39:  // DIGetTMDSize
            mem::write32(c, io[0].addr, (u32)tmd.size());
            return IOS_OK;
        case 0x3A:  // DIGetTMD
            copy_to_guest(c, io[0].addr, tmd.data(), std::min<size_t>(tmd.size(), io[0].size));
            return IOS_OK;
        case 0x07:  // GetDeviceID
            mem::write32(c, io[0].addr, 0x0403AC68);
            return IOS_OK;
        }
        LOG_INFO("es", "unhandled ioctlv 0x%X (%zu in, %zu io)", cmd, in.size(), io.size());
        return -1017;  // ES_EINVAL
    }

private:
    Ios& ios_;
};

// --- /dev/fs and NAND files ------------------------------------------------------------------

constexpr s32 FS_EINVAL = -101, FS_EEXIST = -105;

class NandFile : public Device {
public:
    NandFile(Ios& ios, std::string path, std::filesystem::path host)
        : Device(std::move(path)), ios_(ios), host_(std::move(host)) {}

    s32 open(u32 mode) override {
        if (!std::filesystem::is_regular_file(host_))
            return FS_ENOENT;
        mode_ = mode;
        file_.open(host_, std::ios::binary | std::ios::in | ((mode & 2) ? std::ios::out : std::ios::openmode{}));
        return file_ ? IOS_OK : IOS_EACCES;
    }
    s32 read(u32 buf, u32 len) override {
        u8* dst = mem::ptr(cpu_of(ios_), buf, len);
        if (!dst || !(mode_ & 1))
            return IOS_EACCES;
        file_.clear();
        file_.seekg(pos_);
        file_.read((char*)dst, len);
        s32 n = (s32)file_.gcount();
        pos_ += n;
        return n;
    }
    s32 write(u32 buf, u32 len) override {
        const u8* src = mem::ptr(cpu_of(ios_), buf, len);
        if (!src || !(mode_ & 2))
            return IOS_EACCES;
        file_.clear();
        file_.seekp(pos_);
        file_.write((const char*)src, len);
        file_.flush();
        pos_ += len;
        return (s32)len;
    }
    s32 seek(s32 offset, u32 whence) override {
        s64 base = whence == 0 ? 0 : whence == 1 ? pos_ : (s64)size();
        s64 next = base + offset;
        if (whence > 2 || next < 0 || next > (s64)size())
            return FS_EINVAL;
        pos_ = next;
        return (s32)pos_;
    }
    s32 ioctl(const Request&, u32 cmd, u32, u32, u32 out, u32) override {
        if (cmd == 11) {  // GetFileStats {size, position}
            mem::write32(cpu_of(ios_), out, (u32)size());
            mem::write32(cpu_of(ios_), out + 4, (u32)pos_);
            return IOS_OK;
        }
        return FS_EINVAL;
    }

private:
    u64 size() const {
        std::error_code ec;
        return std::filesystem::file_size(host_, ec);
    }

    Ios& ios_;
    std::filesystem::path host_;
    std::fstream file_;
    u32 mode_ = 0;
    s64 pos_ = 0;
};

class FsDevice : public Device {
public:
    explicit FsDevice(Ios& ios) : Device("/dev/fs"), ios_(ios) {}

    s32 ioctl(const Request&, u32 cmd, u32 in, u32, u32 out, u32) override {
        CPUState* c = cpu_of(ios_);
        auto host = [&](u32 addr) { return ios_.nand().host_path(mem::cstr(c, addr, 64)); };
        std::error_code ec;
        switch (cmd) {
        case 2: {  // GetStats (fixed, plausible numbers)
            const u32 stats[7] = {0x4000, 0x5DEC, 0x1DD4, 0x10, 0x2F0, 0x146B, 0x394};
            for (int i = 0; i < 7; i++)
                mem::write32(c, out + 4 * i, stats[i]);
            return IOS_OK;
        }
        case 3: case 9: {  // CreateDir / CreateFile (ISFSParams: path at +6)
            auto p = host(in + 6);
            if (p.empty()) return FS_EINVAL;
            if (std::filesystem::exists(p)) return FS_EEXIST;
            if (!std::filesystem::is_directory(p.parent_path())) return FS_ENOENT;
            if (cmd == 3)
                std::filesystem::create_directory(p, ec);
            else if (!std::ofstream(p, std::ios::binary))
                return IOS_EACCES;
            LOG_DEBUG("fs", "create %s", mem::cstr(c, in + 6, 64).c_str());
            return ec ? FS_ENOENT : IOS_OK;
        }
        case 5:  // SetAttr
            return IOS_OK;
        case 6: {  // GetAttr -> ISFSParams
            auto p = host(in);
            if (p.empty() || !std::filesystem::exists(p)) return FS_ENOENT;
            u8 params[0x4A] = {};
            params[0] = 0; params[1] = 0; params[2] = 0x10; params[3] = 0x00;  // uid 0x1000
            params[4] = 0x30; params[5] = 0x31;                                  // gid "01"
            std::string path = mem::cstr(c, in, 64);
            std::memcpy(params + 6, path.data(), std::min<size_t>(path.size(), 63));
            params[0x46] = params[0x47] = params[0x48] = 3;  // rw for owner/group/other
            copy_to_guest(c, out, params, sizeof params);
            return IOS_OK;
        }
        case 7: {  // Delete
            auto p = host(in);
            if (p.empty() || !std::filesystem::exists(p)) return FS_ENOENT;
            std::filesystem::remove_all(p, ec);
            return ec ? FS_EINVAL : IOS_OK;
        }
        case 8: {  // Rename
            auto from = host(in), to = host(in + 64);
            if (from.empty() || to.empty() || !std::filesystem::exists(from)) return FS_ENOENT;
            // On Windows a file just written can be held for a moment (antivirus, the search
            // indexer) and the rename fails with a sharing violation: retry for a while. A failed
            // rename here is what the game turns into an empty banner.bin.
            for (int attempt = 0; attempt < 50; attempt++) {
                std::filesystem::rename(from, to, ec);
                if (!ec)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (ec)
                LOG_ERROR("fs", "rename %s -> %s failed: %s", mem::cstr(c, in, 64).c_str(),
                          mem::cstr(c, in + 64, 64).c_str(), ec.message().c_str());
            return ec ? FS_EINVAL : IOS_OK;
        }
        case 13:  // Shutdown
            return IOS_OK;
        }
        LOG_INFO("fs", "unhandled ioctl %u", cmd);
        return FS_EINVAL;
    }

    s32 ioctlv(const Request&, u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        CPUState* c = cpu_of(ios_);
        if (in.empty())
            return FS_EINVAL;
        auto p = ios_.nand().host_path(mem::cstr(c, in[0].addr, 64));
        if (p.empty() || !std::filesystem::exists(p))
            return FS_ENOENT;
        if (cmd == 4) {  // ReadDir: names are NUL-separated, at most 12 characters each
            std::vector<std::string> names;
            for (const auto& e : std::filesystem::directory_iterator(p))
                names.push_back(e.path().filename().string());
            if (in.size() == 1) {  // count only
                mem::write32(c, io[0].addr, (u32)names.size());
                return IOS_OK;
            }
            u32 max = mem::read32(c, in[1].addr), list = io[0].addr, n = 0;
            for (; n < names.size() && n < max; n++) {
                copy_to_guest(c, list, names[n].c_str(), names[n].size() + 1);
                list += (u32)names[n].size() + 1;
            }
            mem::write32(c, io[1].addr, n);
            return IOS_OK;
        }
        if (cmd == 12) {  // GetUsage: clusters (16 KiB) and inodes
            u64 bytes = 0, inodes = 1;
            for (const auto& e : std::filesystem::recursive_directory_iterator(p)) {
                inodes++;
                if (e.is_regular_file())
                    bytes += (e.file_size() + 0x3FFF) & ~0x3FFFull;
            }
            mem::write32(c, io[0].addr, (u32)(bytes / 0x4000));
            mem::write32(c, io[1].addr, (u32)inodes);
            return IOS_OK;
        }
        LOG_INFO("fs", "unhandled ioctlv %u", cmd);
        return FS_EINVAL;
    }

private:
    Ios& ios_;
};

}  // namespace

std::unique_ptr<Device> Ios::make_device(const std::string& path) {
    if (path == "/dev/di") return std::make_unique<DiDevice>(*this);
    if (path == "/dev/fs") return std::make_unique<FsDevice>(*this);
    if (path == "/dev/es") return std::make_unique<EsDevice>(*this);
    if (path == "/dev/stm/immediate") return std::make_unique<StmImmediate>();
    if (path == "/dev/stm/eventhook") return std::make_unique<StmEventHook>();
    if (path.rfind("/dev/", 0) == 0)
        return nullptr;
    auto host = nand_.host_path(path);
    if (host.empty())
        return nullptr;
    return std::make_unique<NandFile>(*this, path, host);
}
