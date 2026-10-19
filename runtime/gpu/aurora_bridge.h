// Feeds the guest's GX command stream to aurora's FIFO processor (which renders it).
//
// The stream is forwarded byte-for-byte. Commands that carry guest physical addresses are
// replaced or accompanied by aurora's extension commands (opcode 0x50) carrying host pointers:
// vertex array bases (sized from the indices each draw uses), textures (from the BP image
// registers, versioned by texture invalidates), TLUTs, and EFB copy parameters. Display list
// calls are inlined by the command processor before they reach the bridge.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../guest.h"

class Runtime;

class AuroraBridge {
public:
    explicit AuroraBridge(Runtime& rt) : rt_(rt) {}

    // One complete GX command; cpreg is the command processor's CP register file.
    void command(const u8* p, u32 len, const u32* cpreg);

    // Draw done: wait for aurora to finish the commands sent so far (the guest may reuse their
    // memory once it sees the PE finish interrupt).
    void draw_done();

    // True once per display copy (EFB -> XFB): the end of a guest frame.
    bool take_frame_end() {
        bool f = frame_end_;
        frame_end_ = false;
        return f;
    }
    u64 frames() const { return frames_; }

    // Host time spent in the bridge (forwarding commands) and waiting for aurora at draw done,
    // in seconds since the last call (for --profile).
    struct Timing {
        double forward = 0, drain = 0;
        u32 draws = 0, drains = 0;
    };
    Timing take_timing() {
        Timing t = timing_;
        timing_ = {};
        return t;
    }

    // Debug screenshots: copy the EFB at the next display copy and write it as a PNG once aurora
    // has read it back (a few frames later; call poll_screenshot() every frame).
    void request_screenshot(std::string path);
    // Read back the index-th EFB texture copy of the given frame (RGBA8 and RGB565 copies).
    void request_copy_shot(u64 frame, u32 index, std::string path);
    // Capture the EFB just before the given draw of a frame (draws counted from 0).
    void request_draw_shot(u64 frame, u32 draw, std::string path);
    void poll_screenshot();

private:
    void write(const void* data, u32 len);
    void aurora_cmd(u16 sub);
    void put8(u8 v);
    void put16(u16 v);
    void put32(u32 v);
    void put64(u64 v);
    u64 host(u32 phys, u32 len) const;

    void forward(const u8* p, u32 len, const u32* cpreg);
    void cp_reg(u8 reg, u32 value);
    void bp_reg(u32 value);
    void prepare_draw(const u8* p, u32 len, const u32* cpreg);
    void ensure_array(u32 idx, u32 bytes_needed);
    void flush_textures();
    void efb_copy(u32 value);

    Runtime& rt_;
    Timing timing_;

    // Vertex arrays 0-15: guest base and stride, and what aurora was last given.
    std::array<u32, 16> array_base_{}, array_stride_{};
    struct Emitted {
        u32 base = 0, size = 0;
    };
    std::array<Emitted, 16> array_sent_{};

    // Texture maps 0-7: BP mode0/mode1/image0/image3/tlut registers.
    struct TexMap {
        u32 mode0 = 0, mode1 = 0, image0 = 0, image3 = 0, tlut = 0;
        bool dirty = false;
    };
    std::array<TexMap, 8> tex_{};
    u32 tex_version_ = 1;  // bumped by texture cache invalidation (BP 0x66)

    // TLUTs loaded into TMEM: offset (BP 0x65 bits 0-9) -> source memory.
    struct Tlut {
        u32 tmem = 0xFFFFFFFF, addr = 0, entries = 0, version = 0;
    };
    std::array<Tlut, 20> tluts_{};
    u32 bp64_ = 0;

    // EFB copy source/destination registers.
    u32 copy_src_ = 0, copy_size_ = 0, copy_dest_ = 0;

    bool frame_end_ = false;
    u64 frames_ = 0;

    // Per-interval statistics (logged at debug level every 30 frames).
    struct Stats {
        u32 draws = 0, verts = 0, textures = 0, arrays = 0, copies = 0, xf = 0;
        u32 formats = 0;  // bitmask of texture formats loaded
    } stats_;
    u32 clear_ar_ = 0, clear_gb_ = 0;
    u32 last_color_ = 0;
    std::array<u32, 256> bp_{};  // last value written to each BP register (debug)
    void log_stats();

    // Readbacks in flight, each on its own stream (a stream keeps only its latest capture).
    struct Shot {
        u64 stream = 0;
        std::unique_ptr<u8> key;  // its address identifies our own copies to aurora
        u32 w = 0, h = 0, format = 6, polls = 0;
        std::string path;
    };
    std::vector<Shot> shots_;
    void capture_efb(u32 x, u32 y, u32 w, u32 h, std::string path);
    bool capture_copy(const void* src, u32 w, u32 h, u32 format, std::string path);
    std::string frame_shot_path_;
    bool frame_shot_requested_ = false;
    struct Pending {
        u64 frame;
        u32 index;
        std::string path;
    };
    std::vector<Pending> copy_shots_, draw_shots_;
    u32 frame_copies_ = 0, frame_draws_ = 0;
};
