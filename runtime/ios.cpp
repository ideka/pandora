#include "ios.h"

#include "hardware.h"
#include "log.h"
#include "memory.h"
#include "runtime.h"

namespace {

constexpr u32 IPC_OPEN = 1, IPC_CLOSE = 2, IPC_READ = 3, IPC_WRITE = 4, IPC_SEEK = 5, IPC_IOCTL = 6,
              IPC_IOCTLV = 7, IPC_REPLY = 8;

const char* command_name(u32 c) {
    static const char* names[] = {"?", "open", "close", "read", "write", "seek", "ioctl", "ioctlv"};
    return c < 8 ? names[c] : "?";
}

}  // namespace

s32 Device::read(u32, u32) {
    LOG_INFO("ios", "%s: read not implemented", name_.c_str());
    return IOS_EINVAL;
}

s32 Device::write(u32, u32) {
    LOG_INFO("ios", "%s: write not implemented", name_.c_str());
    return IOS_EINVAL;
}

s32 Device::seek(s32, u32) {
    LOG_INFO("ios", "%s: seek not implemented", name_.c_str());
    return IOS_EINVAL;
}

s32 Device::ioctl(const Request&, u32 cmd, u32, u32 in_len, u32, u32 out_len) {
    LOG_INFO("ios", "%s: ioctl 0x%X not implemented (in %u, out %u)", name_.c_str(), cmd, in_len, out_len);
    return IOS_EINVAL;
}

s32 Device::ioctlv(const Request&, u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) {
    LOG_INFO("ios", "%s: ioctlv 0x%X not implemented (%zu in, %zu io)", name_.c_str(), cmd, in.size(), io.size());
    return IOS_EINVAL;
}

Ios::Ios(Runtime& rt) : rt_(rt) {}

// ES creates a title's data directory when it is installed or launched; the game saves there.
bool Ios::init(const std::filesystem::path& disc_root, const std::filesystem::path& nand_root) {
    if (!disc_.open(disc_root) || !nand_.open(nand_root))
        return false;
    const auto& t = disc_.ticket();
    char dir[48];
    std::snprintf(dir, sizeof dir, "/title/%02x%02x%02x%02x/%02x%02x%02x%02x/data", t[0x1DC], t[0x1DD], t[0x1DE],
                  t[0x1DF], t[0x1E0], t[0x1E1], t[0x1E2], t[0x1E3]);
    std::error_code ec;
    std::filesystem::create_directories(nand_.host_path(dir), ec);
    if (ec)
        LOG_ERROR("ios", "cannot create %s: %s", dir, ec.message().c_str());
    return !ec;
}
Ios::~Ios() = default;

void Ios::submit(u32 addr) {
    CPUState* c = &rt_.cpu;
    Request r{};
    r.addr = addr;
    r.command = mem::read32(c, addr);
    r.fd = (s32)mem::read32(c, addr + 8);
    for (int i = 0; i < 5; i++)
        r.args[i] = mem::read32(c, addr + 0x0C + 4 * i);

    s32 result = IOS_EINVAL;
    if (r.command == IPC_OPEN) {
        std::string path = mem::cstr(c, r.args[0], 64);
        auto dev = make_device(path);
        if (!dev) {
            LOG_INFO("ios", "open %s: no such device", path.c_str());
            result = IOS_ENOENT;
        } else if ((result = dev->open(r.args[1])) >= 0) {
            result = next_fd_++;
            LOG_DEBUG("ios", "open %s -> fd %d", path.c_str(), result);
            fds_[result] = std::move(dev);
        } else {
            LOG_DEBUG("ios", "open %s failed: %d", path.c_str(), result);
        }
    } else {
        auto it = fds_.find(r.fd);
        if (it == fds_.end()) {
            LOG_ERROR("ios", "%s on unknown fd %d", command_name(r.command), r.fd);
            result = IOS_EINVAL;
        } else {
            Device& d = *it->second;
            switch (r.command) {
            case IPC_CLOSE:
                LOG_DEBUG("ios", "close fd %d (%s)", r.fd, d.name().c_str());
                result = d.close();
                fds_.erase(it);
                break;
            case IPC_READ: result = d.read(r.args[0], r.args[1]); break;
            case IPC_WRITE: result = d.write(r.args[0], r.args[1]); break;
            case IPC_SEEK: result = d.seek((s32)r.args[0], r.args[1]); break;
            case IPC_IOCTL: result = d.ioctl(r, r.args[0], r.args[1], r.args[2], r.args[3], r.args[4]); break;
            case IPC_IOCTLV: {
                std::vector<IoVec> in(r.args[1]), io(r.args[2]);
                u32 v = r.args[3];
                for (auto& e : in) { e = {mem::read32(c, v), mem::read32(c, v + 4)}; v += 8; }
                for (auto& e : io) { e = {mem::read32(c, v), mem::read32(c, v + 4)}; v += 8; }
                result = d.ioctlv(r, r.args[0], in, io);
                break;
            }
            default: LOG_ERROR("ios", "unknown IPC command %u", r.command);
            }
        }
    }
    if (result != IOS_NO_REPLY)
        reply(r, result);
}

void Ios::reply(const Request& r, s32 result) {
    CPUState* c = &rt_.cpu;
    mem::write32(c, r.addr + 4, (u32)result);
    mem::write32(c, r.addr + 8, r.command);
    mem::write32(c, r.addr, IPC_REPLY);
    rt_.hw().ipc_reply(r.addr);
}
