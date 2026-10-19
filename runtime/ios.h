// Emulated IOS: services IPC requests posted through the Hollywood IPC registers.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "disc.h"
#include "guest.h"
#include "nand.h"

class Runtime;
class Ios;

// IOS error codes returned to the PPC.
enum : s32 {
    IOS_OK = 0,
    IOS_EACCES = -1,
    IOS_EINVAL = -4,
    IOS_ENOENT = -6,
    FS_ENOENT = -106,
    // Not an IOS code: the request is completed later (or never) via Ios::reply.
    IOS_NO_REPLY = 0x7FFFFFFF,
};

struct IoVec {
    u32 addr;
    u32 size;
};

struct Request {
    u32 addr;     // physical address of the request block
    u32 command;  // 1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv
    s32 fd;
    u32 args[5];
};

class Device {
public:
    explicit Device(std::string name) : name_(std::move(name)) {}
    virtual ~Device() = default;
    const std::string& name() const { return name_; }

    virtual s32 open(u32 /*mode*/) { return IOS_OK; }
    virtual s32 close() { return IOS_OK; }
    virtual s32 read(u32 buf, u32 len);
    virtual s32 write(u32 buf, u32 len);
    virtual s32 seek(s32 offset, u32 whence);
    virtual s32 ioctl(const Request& r, u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len);
    virtual s32 ioctlv(const Request& r, u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io);

private:
    std::string name_;
};

class Ios {
public:
    explicit Ios(Runtime& rt);
    ~Ios();

    bool init(const std::filesystem::path& disc_root, const std::filesystem::path& nand_root);

    void submit(u32 request);
    void reply(const Request& r, s32 result);
    Runtime& rt() { return rt_; }
    Disc& disc() { return disc_; }
    Nand& nand() { return nand_; }

private:
    std::unique_ptr<Device> make_device(const std::string& path);  // ios_devices.cpp

    Runtime& rt_;
    Disc disc_;
    Nand nand_;
    std::unordered_map<s32, std::unique_ptr<Device>> fds_;
    s32 next_fd_ = 3;
};
