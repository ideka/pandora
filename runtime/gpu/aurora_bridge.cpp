#include "aurora_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dolphin/gx/GXAurora.h>

#include "gx/fifo.hpp"  // aurora internal: the FIFO its command processor consumes

#include "../log.h"
#include "../memory.h"
#include "../png_write.h"
#include "../runtime.h"
#include "vertex_layout.h"

namespace {

// Aurora extension opcode and subcommands (include/dolphin/gx/GXAurora.h).
constexpr u8 GX_AURORA = 0x50;
constexpr u16 LOAD_ARRAYBASE = 0x0010, LOAD_TEXOBJ = 0x0030, LOAD_TLUT = 0x0031, LOAD_COPY_SRC = 0x0035,
              LOAD_COPY_DST = 0x0036, LOAD_COPY_DEST = 0x0037;

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }

// Texture formats that index a TLUT (C4, C8, C14X2).
bool is_ci(u32 fmt) { return fmt == 8 || fmt == 9 || fmt == 10; }

// Mipmapped when the minification filter uses mips and the max LOD is above zero.
bool has_mips(u32 mode0, u32 mode1) {
    u32 min_filter = (mode0 >> 5) & 7;
    u32 max_lod = (mode1 >> 8) & 0xFF;
    return (min_filter & 3) != 0 && max_lod > 0;
}

// EFB copy target format (BP 0x52) to the GXTexFmt aurora expects.
u32 copy_format(u32 control) {
    u32 target = (control >> 3) & 0xF;
    u32 real = target / 2 + (target & 1) * 8;
    bool intensity = (control >> 15) & 1;
    if (intensity) {
        static const u32 to_intensity[16] = {0, 1, 2, 3, 4, 5, 6, 0x27, 1, 0x29, 0x2A, 0x2B, 0x2C, 0, 0, 0};
        return to_intensity[real];
    }
    if (real >= 4 && real <= 6)
        return real;  // RGB565, RGB5A3, RGBA8
    return 0x20 | real;  // GX_CTF_*
}

// Frame whose commands are logged in detail (PANDORA_DEBUG_FRAME, default 100).
u64 debug_frame() {
    static const u64 frame = [] {
        const char* e = std::getenv("PANDORA_DEBUG_FRAME");
        return e ? std::strtoull(e, nullptr, 10) : 100ull;
    }();
    return frame;
}

// BP 0x41 (PE colour mode 0): blend enable (0), logic op enable (1), dst factor (5-7), src factor
// (8-10), subtract (11), logic op (12-15). WebGPU has no logic ops and aurora implements only
// CLEAR, COPY and NOOP, so the others are approximated with blending using Dolphin's table
// (VideoCommon/RenderState.cpp, ApproximateLogicOpWithBlending). The shader-side adjustments
// Dolphin makes for CLEAR/INVERT/COPY_INVERTED/SET are not applied.
u32 approximate_logic_op(u32 value) {
    if (!(value & 2))
        return value;
    const u32 op = (value >> 12) & 0xF;
    if (op == 0 || op == 3 || op == 5)
        return value;  // aurora handles these
    struct Approx {
        bool blend, subtract;
        u8 src, dst;
    };
    static constexpr Approx table[16] = {
        {false, false, 1, 0}, {true, false, 2, 0}, {true, true, 1, 3},  {false, false, 1, 0},
        {true, true, 2, 1},   {true, false, 0, 1}, {true, false, 3, 3}, {true, false, 3, 1},
        {true, false, 3, 3},  {true, false, 3, 0}, {true, false, 3, 0}, {true, false, 1, 7},
        {false, false, 1, 0}, {true, false, 3, 1}, {true, false, 3, 3}, {false, false, 1, 0},
    };
    const Approx& a = table[op];
    value &= ~(0x3u | (7u << 5) | (7u << 8) | (1u << 11));
    if (a.blend)
        value |= 1u | ((u32)a.dst << 5) | ((u32)a.src << 8) | (a.subtract ? 1u << 11 : 0);
    else
        value |= (1u << 5) * 0 | (1u << 8);  // plain write: src ONE, dst ZERO
    return value;
}

// Debugging: decode the position of vertex 0 of a draw (direct or indexed).
std::string first_position(CPUState* cpu, const u8* p, u32 len, const u32* cpreg, const u32* base, const u32* stride) {
    const u32 vat = p[0] & 7, vcd = cpreg[0x50], a = cpreg[0x70 + vat];
    const u32 mode = (vcd >> 9) & 3, fmt = (a >> 1) & 7, frac = (a >> 4) & 0x1F, cnt = (a & 1) ? 3 : 2;
    u32 off = 3;
    for (u32 i = 0; i < 9; i++)
        off += (vcd >> i) & 1;
    u32 addr = 0;
    const u8* src = nullptr;
    if (mode == 1) {
        src = p + off;
    } else if (mode >= 2) {
        if (off + 2 > len)
            return "?";
        const u32 idx = mode == 2 ? p[off] : (u32)(p[off] << 8 | p[off + 1]);
        addr = base[0] + idx * stride[0];
    } else {
        return "none";
    }
    static const u32 sizes[8] = {1, 1, 2, 2, 4, 0, 0, 0};
    float v[3] = {};
    for (u32 k = 0; k < cnt; k++) {
        const u32 sz = sizes[fmt];
        u32 raw = 0;
        for (u32 b = 0; b < sz; b++)
            raw = raw << 8 | (src ? src[k * sz + b] : mem::read8(cpu, 0x80000000 | (addr + k * sz + b)));
        float f;
        switch (fmt) {
        case 0: f = (float)(u8)raw; break;
        case 1: f = (float)(s8)raw; break;
        case 2: f = (float)(u16)raw; break;
        case 3: f = (float)(s16)raw; break;
        default: std::memcpy(&f, &raw, 4); break;
        }
        v[k] = fmt == 4 ? f : f / (float)(1u << frac);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.3g, %.3g, %.3g)", v[0], v[1], v[2]);
    return buf;
}

u32 hash3(u32 a, u32 b, u32 c) {
    u32 h = a * 0x9E3779B1u;
    h ^= b + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h ^= c + 0x165667B1u + (h << 6) + (h >> 2);
    return h ? h : 1;
}

}  // namespace

// --- Output ----------------------------------------------------------------------------------

void AuroraBridge::write(const void* data, u32 len) { aurora::gx::fifo::write_data(data, len); }
void AuroraBridge::put8(u8 v) { write(&v, 1); }
void AuroraBridge::put16(u16 v) {
    u8 b[2] = {(u8)(v >> 8), (u8)v};
    write(b, 2);
}
void AuroraBridge::put32(u32 v) {
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    write(b, 4);
}
void AuroraBridge::put64(u64 v) {
    put32((u32)(v >> 32));
    put32((u32)v);
}
void AuroraBridge::aurora_cmd(u16 sub) {
    put8(GX_AURORA);
    put16(sub);
}

// Host pointer for a guest physical address; aurora reads the data (big-endian) from there.
u64 AuroraBridge::host(u32 phys, u32 len) const {
    u8* p = mem::ptr(&rt_.cpu, phys, std::max<u32>(len, 1));
    if (!p)
        p = mem::ptr(&rt_.cpu, phys, 1);
    return reinterpret_cast<u64>(p);
}

// --- Commands --------------------------------------------------------------------------------

void AuroraBridge::command(const u8* p, u32 len, const u32* cpreg) {
    const u8 op = p[0];
    // Debugging: log every command of frame 100 up to its second draw.
    if (frames_ == debug_frame() && frame_draws_ < 16 && g_log_level >= 3) {
        char hex[3 * 128 + 1] = {};
        for (u32 i = 0; i < len && i < 128; i++)
            std::snprintf(hex + 3 * i, 4, "%02X ", p[i]);
        LOG_TRACE("bridge", "cmd %s%s", hex, len > 128 ? "..." : "");
    }
    if (op == 0x08) {
        cp_reg(p[1], be32(p + 2));
        if (p[1] >= 0xA0 && p[1] < 0xB0)
            return;  // array bases are sent as aurora commands when a draw needs them
    } else if (op == 0x61) {
        bp_reg(be32(p + 1));
        return;  // bp_reg forwards (possibly rewritten) itself
    } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        // Indexed XF load from arrays 12-15: idx * stride + (n words).
        u32 array = 12 + (op - 0x20) / 8;
        u32 idx = be16(p + 1), words = (be16(p + 3) >> 12) + 1;
        ensure_array(array, idx * array_stride_[array] + words * 4);
    } else if (op >= 0x80 && op < 0xC0) {
        for (auto it = draw_shots_.begin(); it != draw_shots_.end();)
            if (it->frame == frames_ && it->index == frame_draws_) {
                capture_efb(0, 0, 640, 480, std::move(it->path));
                it = draw_shots_.erase(it);
            } else
                ++it;
        stats_.draws++;
        frame_draws_++;
        if (len >= 3 + 16 && vertex_layout(cpreg, p[0] & 7).size == 16) {
            last_color_ = be32(p + 3 + 12);
            if (frames_ == debug_frame() && (p[0] & 0xF8) == 0x90)
                LOG_DEBUG("bridge", "  16-byte-vertex draw issued from %s\n%s", rt_.describe_pc().c_str(),
                          rt_.backtrace(12).c_str());
        }
        stats_.verts += be16(p + 1);
        if (frames_ == debug_frame())
            LOG_DEBUG("bridge", "  draw %u prim %02X vat %u count %u vtxsize %u vcd %05X/%04X pos@%08X/%u v0 %s  gen %06X z %06X "
                      "cmode0 %06X cmode1 %06X alpha %06X",
                      frame_draws_ - 1, p[0] & 0xF8, p[0] & 7, be16(p + 1), vertex_layout(cpreg, p[0] & 7).size,
                      cpreg[0x50], cpreg[0x60], array_base_[0], array_stride_[0],
                      first_position(&rt_.cpu, p, len, cpreg, array_base_.data(), array_stride_.data()).c_str(),
                      bp_[0x00], bp_[0x40], bp_[0x41], bp_[0x42], bp_[0xF3]);
        prepare_draw(p, len, cpreg);
        write(p, len);
        aurora::gx::fifo::publish();
        return;
    } else if (op == 0x40) {
        return;  // display lists are inlined by the command processor
    } else if (op == 0x10) {
        stats_.xf++;
    }
    write(p, len);
}

void AuroraBridge::cp_reg(u8 reg, u32 value) {
    if (reg >= 0xA0 && reg < 0xB0)
        array_base_[reg - 0xA0] = value & 0x3FFFFFFF;
    else if (reg >= 0xB0 && reg < 0xC0)
        array_stride_[reg - 0xB0] = value & 0xFF;
}

// Make sure aurora has array idx mapped with at least bytes_needed bytes.
void AuroraBridge::ensure_array(u32 idx, u32 bytes_needed) {
    Emitted& s = array_sent_[idx];
    const u32 base = array_base_[idx];
    if (s.base == base && s.size >= bytes_needed && s.size)
        return;
    u32 size = std::max(bytes_needed, 32u);
    if (s.base == base)
        size = std::max(size, s.size * 2);  // grow geometrically to limit re-uploads
    // Clamp to the end of the memory region the array lives in.
    const u32 region_end = (base & 0x10000000) ? 0x14000000 : 0x01800000;
    const u32 off = base & 0x1FFFFFFF;
    size = std::min(size, off < region_end ? region_end - off : 32u);
    s = {base, size};
    stats_.arrays++;
    aurora_cmd((u16)(LOAD_ARRAYBASE | idx));
    put64(host(base, size));
    put32(size);
    put8(0);  // big-endian data
}

// Before a draw: map every indexed array far enough for the indices it uses, and send any
// texture whose registers changed.
void AuroraBridge::prepare_draw(const u8* p, u32 len, const u32* cpreg) {
    const VertexLayout layout = vertex_layout(cpreg, p[0] & 7);
    const u32 count = be16(p + 1);
    if (layout.size && 3 + count * layout.size <= len) {
        for (u32 i = 0; i < layout.num_indices; i++) {
            const auto& ix = layout.indices[i];
            const u32 none = ix.bytes == 1 ? 0xFF : 0xFFFF;  // "no data" index
            u32 max = 0;
            bool any = false;
            for (u32 v = 0; v < count; v++) {
                const u8* q = p + 3 + v * layout.size + ix.offset;
                for (u32 k = 0; k < ix.count; k++, q += ix.bytes) {
                    u32 index = ix.bytes == 1 ? *q : be16(q);
                    if (index != none) {
                        max = std::max(max, index);
                        any = true;
                    }
                }
            }
            if (any) {
                u32 array = ix.attr - 9;
                ensure_array(array, (max + 1) * std::max<u32>(array_stride_[array], 4));
            }
        }
    }
    flush_textures();
}

void AuroraBridge::flush_textures() {
    for (u32 id = 0; id < 8; id++) {
        TexMap& t = tex_[id];
        if (!t.dirty)
            continue;
        t.dirty = false;
        const u32 width = (t.image0 & 0x3FF) + 1, height = ((t.image0 >> 10) & 0x3FF) + 1;
        const u32 format = (t.image0 >> 20) & 0xF;
        const u32 addr = (t.image3 & 0x00FFFFFF) << 5;
        u32 slot = 0;
        if (is_ci(format)) {
            // Find the TLUT loaded at this map's TMEM offset and send it with this map's format.
            const u32 tmem = t.tlut & 0x3FF, tlut_format = (t.tlut >> 10) & 3;
            for (u32 s = 0; s < tluts_.size(); s++) {
                if (tluts_[s].tmem == tmem) {
                    slot = s;
                    aurora_cmd(LOAD_TLUT);
                    put8((u8)s);
                    put64(host(tluts_[s].addr, tluts_[s].entries * 2));
                    put32(tlut_format);
                    put16((u16)tluts_[s].entries);
                    put32(hash3(tluts_[s].addr, tluts_[s].entries, 0x7105));
                    put32(tluts_[s].version);
                    break;
                }
            }
        }
        stats_.textures++;
        stats_.formats |= 1u << format;
        if (frames_ == debug_frame())
            LOG_DEBUG("bridge", "  tex map %u: %08X %ux%u fmt %u mode0 %06X mode1 %06X", id, addr, width, height,
                      format, t.mode0 & 0xFFFFFF, t.mode1 & 0xFFFFFF);
        aurora_cmd(LOAD_TEXOBJ);
        put8((u8)id);
        put64(host(addr, width * height * 4));
        put32(width);
        put32(height);
        put32(format);
        put32(slot);
        put8(has_mips(t.mode0, t.mode1) ? 1 : 0);
        put32(hash3(addr, t.image0 & 0xFFFFFF, format));
        put32(tex_version_);
    }
}

void AuroraBridge::bp_reg(u32 value) {
    const u32 reg = value >> 24;
    bp_[reg] = value & 0xFFFFFF;
    if ((reg >= 0x80 && reg < 0x9C) || (reg >= 0xA0 && reg < 0xBC)) {
        const u32 kind = (reg & 0x1F) >> 2, id = (reg & 3) + (reg >= 0xA0 ? 4 : 0);
        TexMap& t = tex_[id];
        switch (kind) {
        case 0: t.mode0 = value; break;
        case 1: t.mode1 = value; break;
        case 2: t.image0 = value; break;
        case 5: t.image3 = value; break;
        case 6: t.tlut = value; break;
        }
        t.dirty = true;
    } else if (reg == 0x64) {
        bp64_ = value;
    } else if (reg == 0x65) {
        // TLUT load: remember the source; aurora identifies TLUTs by slot (its GXTlut id),
        // which takes the place of the TMEM offset in the register.
        const u32 tmem = value & 0x3FF, entries = ((value >> 10) & 0x3FF) + 1;
        u32 slot = 0;
        while (slot < tluts_.size() && tluts_[slot].tmem != tmem && tluts_[slot].tmem != 0xFFFFFFFF)
            slot++;
        if (slot == tluts_.size())
            slot = tmem % tluts_.size();
        Tlut& tl = tluts_[slot];
        tl = {tmem, (bp64_ & 0x00FFFFFF) << 5, entries, tl.version + 1};
        for (auto& t : tex_)
            if ((t.tlut & 0x3FF) == tmem)
                t.dirty = true;
        value = (value & ~0x3FFu) | slot;
    } else if (reg == 0x66) {
        tex_version_++;  // texture cache invalidate: guest texture memory may have changed
        for (auto& t : tex_)
            t.dirty = true;
    } else if (reg == 0x59) {
        // Scissor box offset. The hardware (and Nintendo's SDK) store (offset + 342) / 2 per axis
        // and subtract twice the field from the scissor coordinates; aurora stores
        // (offset - 342) / 2 and adds it. Convert: aurora field = -hardware field (10-bit).
        const u32 hx = value & 0x3FF, hy = (value >> 10) & 0x3FF;
        value = (value & 0xFF000000) | ((0x400 - hy) & 0x3FF) << 10 | ((0x400 - hx) & 0x3FF);
    } else if (reg == 0x41) {
        value = approximate_logic_op(value);
    } else if (reg == 0x4F) {
        clear_ar_ = value & 0xFFFF;
    } else if (reg == 0x50) {
        clear_gb_ = value & 0xFFFF;
    } else if (reg == 0x49) {
        copy_src_ = value;
    } else if (reg == 0x4A) {
        copy_size_ = value;
    } else if (reg == 0x4B) {
        copy_dest_ = value;
    } else if (reg == 0x52) {
        efb_copy(value);
        if ((value >> 14) & 1)
            return;  // display copies end the frame; aurora presents its EFB instead
    }
    u8 b[5] = {0x61, (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value};
    write(b, 5);
}

void AuroraBridge::efb_copy(u32 control) {
    if ((control >> 14) & 1) {
        if (frame_shot_requested_) {
            frame_shot_requested_ = false;
            capture_efb(copy_src_ & 0x3FF, (copy_src_ >> 10) & 0x3FF, (copy_size_ & 0x3FF) + 1,
                        ((copy_size_ >> 10) & 0x3FF) + 1, std::move(frame_shot_path_));
        }
        frame_end_ = true;
        frames_++;
        frame_copies_ = 0;
        frame_draws_ = 0;
        if (frames_ % 30 == 0)
            log_stats();
        return;
    }
    stats_.copies++;
    if (frames_ == debug_frame())
        LOG_DEBUG("bridge", "  efb copy src %d,%d %dx%d -> %08X fmt %u%s%s", (int)(copy_src_ & 0x3FF),
                  (int)((copy_src_ >> 10) & 0x3FF), (int)(copy_size_ & 0x3FF) + 1, (int)((copy_size_ >> 10) & 0x3FF) + 1,
                  (copy_dest_ & 0xFFFFFF) << 5, copy_format(control), (control >> 9) & 1 ? " half" : "",
                  (control >> 11) & 1 ? " clear" : "");
    const s32 x = copy_src_ & 0x3FF, y = (copy_src_ >> 10) & 0x3FF;
    const s32 w = (copy_size_ & 0x3FF) + 1, h = ((copy_size_ >> 10) & 0x3FF) + 1;
    const bool half = (control >> 9) & 1;
    aurora_cmd(LOAD_COPY_SRC);
    put32((u32)x);
    put32((u32)y);
    put32((u32)w);
    put32((u32)h);
    aurora_cmd(LOAD_COPY_DST);
    put32(half ? (u32)(w + 1) / 2 : (u32)w);
    put32(half ? (u32)(h + 1) / 2 : (u32)h);
    put32(copy_format(control));
    put8(half ? 1 : 0);
    aurora_cmd(LOAD_COPY_DEST);
    const u64 dest = host((copy_dest_ & 0x00FFFFFF) << 5, (u32)(w * h * 4));
    put64(dest);
    for (auto it = copy_shots_.begin(); it != copy_shots_.end();) {
        if (it->frame != frames_ || it->index != frame_copies_) {
            ++it;
            continue;
        }
        // Forward the copy now so the readback sees it, then request its contents.
        u8 b[5] = {0x61, (u8)(control >> 24), (u8)(control >> 16), (u8)(control >> 8), (u8)control};
        write(b, 5);
        aurora::gx::fifo::publish();
        capture_copy(reinterpret_cast<const void*>(dest), half ? (w + 1) / 2 : w, half ? (h + 1) / 2 : h,
                     copy_format(control), std::move(it->path));
        it = copy_shots_.erase(it);
    }
    frame_copies_++;
}

void AuroraBridge::request_copy_shot(u64 frame, u32 index, std::string path) {
    copy_shots_.push_back({frame, index, std::move(path)});
}

void AuroraBridge::request_draw_shot(u64 frame, u32 draw, std::string path) {
    draw_shots_.push_back({frame, draw, std::move(path)});
}

void AuroraBridge::draw_done() {
    aurora::gx::fifo::publish();
    aurora::gx::fifo::drain();
}

// --- Screenshots -----------------------------------------------------------------------------

void AuroraBridge::request_screenshot(std::string path) {
    frame_shot_path_ = std::move(path);
    frame_shot_requested_ = true;
}

// Ask aurora to read back a texture copy (identified by its destination) once it is rendered.
bool AuroraBridge::capture_copy(const void* src, u32 w, u32 h, u32 format, std::string path) {
    if (format != 6 && format != 4) {
        LOG_ERROR("shot", "copy format %u not supported; %s skipped", format, path.c_str());
        return false;
    }
    Shot shot;
    shot.stream = AuroraReadbackCreateStream();
    shot.w = w;
    shot.h = h;
    shot.format = format;
    shot.path = std::move(path);
    const AuroraReadbackCopySrc s{1, src};
    if (!AuroraReadbackCaptureCopies(shot.stream, &s, 1)) {
        LOG_ERROR("shot", "readback unavailable; %s skipped", shot.path.c_str());
        AuroraReadbackDestroyStream(shot.stream);
        return false;
    }
    shots_.push_back(std::move(shot));
    return true;
}

// Copy an EFB region to a private RGBA8 copy texture and read it back. The copy does not clear,
// so rendering continues unaffected.
void AuroraBridge::capture_efb(u32 x, u32 y, u32 w, u32 h, std::string path) {
    auto key = std::make_unique<u8>(0);
    aurora_cmd(LOAD_COPY_SRC);
    put32(x);
    put32(y);
    put32(w);
    put32(h);
    aurora_cmd(LOAD_COPY_DST);
    put32(w);
    put32(h);
    put32(6);  // GX_TF_RGBA8
    put8(0);
    aurora_cmd(LOAD_COPY_DEST);
    put64(reinterpret_cast<u64>(key.get()));
    put8(0x61);
    put32(0x52000000 | (12u << 3));  // texture copy, RGBA8 (target format 12), no clear
    aurora::gx::fifo::publish();
    if (capture_copy(key.get(), w, h, 6, std::move(path)))
        shots_.back().key = std::move(key);
}

void AuroraBridge::poll_screenshot() {
    for (auto it = shots_.begin(); it != shots_.end();) {
        Shot& s = *it;
        const u32 bw = (s.w + 3) / 4, bh = (s.h + 3) / 4;
        const u32 block_bytes = s.format == 6 ? 64 : 32;
        std::vector<u8> tiled((size_t)bw * bh * block_bytes);
        AuroraReadbackDest dest{1, tiled.data(), (u32)tiled.size(), false};
        if (!AuroraReadbackGet(s.stream, &dest, 1) || !dest.found) {
            if (++s.polls > 120) {
                LOG_ERROR("shot", "readback of %s never completed", s.path.c_str());
                AuroraReadbackDestroyStream(s.stream);
                it = shots_.erase(it);
            } else
                ++it;
            continue;
        }
        // GX RGBA8 tiles: 4x4 pixels, 32 bytes of (A, R) pairs then 32 bytes of (G, B) pairs.
        // RGB565 tiles: 4x4 big-endian 16-bit pixels.
        std::vector<u8> rgba((size_t)s.w * s.h * 4);
        for (u32 by = 0; by < bh; by++)
            for (u32 bx = 0; bx < bw; bx++) {
                const u8* blk = tiled.data() + ((size_t)by * bw + bx) * block_bytes;
                for (u32 i = 0; i < 16; i++) {
                    const u32 px = bx * 4 + i % 4, py = by * 4 + i / 4;
                    if (px >= s.w || py >= s.h)
                        continue;
                    u8* o = &rgba[((size_t)py * s.w + px) * 4];
                    if (s.format == 6) {
                        o[0] = blk[2 * i + 1];
                        o[1] = blk[32 + 2 * i];
                        o[2] = blk[32 + 2 * i + 1];
                    } else {
                        const u16 v = be16(blk + 2 * i);
                        o[0] = (u8)((v >> 11) * 255 / 31);
                        o[1] = (u8)(((v >> 5) & 63) * 255 / 63);
                        o[2] = (u8)((v & 31) * 255 / 31);
                    }
                    o[3] = 255;  // EFB alpha is often 0 or unused; keep the image visible
                }
            }
        if (write_png_rgba(s.path, rgba.data(), s.w, s.h))
            LOG_INFO("shot", "wrote %s (%ux%u)", s.path.c_str(), s.w, s.h);
        else
            LOG_ERROR("shot", "cannot write %s", s.path.c_str());
        AuroraReadbackDestroyStream(s.stream);
        it = shots_.erase(it);
    }
}

void AuroraBridge::log_stats() {
    LOG_DEBUG("bridge",
              "frame %llu: last 30 frames %u draws, %u verts, %u xf loads, %u texture loads (formats %04X), "
              "%u array uploads, %u efb copies; clear colour A%02X R%02X G%02X B%02X; overlay %08X",
              (unsigned long long)frames_, stats_.draws, stats_.verts, stats_.xf, stats_.textures, stats_.formats,
              stats_.arrays, stats_.copies, clear_ar_ >> 8, clear_ar_ & 0xFF, clear_gb_ >> 8, clear_gb_ & 0xFF,
              last_color_);
    stats_ = {};
}
