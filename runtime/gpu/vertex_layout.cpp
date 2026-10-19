#include "vertex_layout.h"

namespace {

// Bytes per component for position/normal/texcoord formats (u8, s8, u16, s16, f32).
u32 comp_size(u32 fmt) {
    static const u32 sizes[8] = {1, 1, 2, 2, 4, 0, 0, 0};
    return sizes[fmt & 7];
}
// Bytes per colour for RGB565, RGB888, RGB888x, RGBA4444, RGBA6, RGBA8888.
u32 color_size(u32 fmt) {
    static const u32 sizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return sizes[fmt & 7];
}

}  // namespace

VertexLayout vertex_layout(const u32* cpreg, u32 vat) {
    VertexLayout l;
    const u32 vcd_lo = cpreg[0x50], vcd_hi = cpreg[0x60];
    const u32 a = cpreg[0x70 + vat], b = cpreg[0x80 + vat], c = cpreg[0x90 + vat];

    // Matrix indices (PNMTXIDX, TEX0-7MTXIDX): one byte each when present.
    for (u32 i = 0; i < 9; i++)
        l.size += (vcd_lo >> i) & 1;

    auto attr = [&](u32 id, u32 mode, u32 direct_size, u32 index_count) {
        switch (mode) {
        case 1:
            l.size += direct_size;
            break;
        case 2:
        case 3: {
            u8 bytes = mode == 2 ? 1 : 2;
            l.indices[l.num_indices++] = {(u8)id, (u8)l.size, bytes, (u8)index_count};
            l.size += bytes * index_count;
            break;
        }
        }
    };
    attr(9, (vcd_lo >> 9) & 3, ((a & 1) ? 3 : 2) * comp_size(a >> 1), 1);  // position
    const bool nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
    attr(10, (vcd_lo >> 11) & 3, (nbt ? 9 : 3) * comp_size(a >> 10), nbt && index3 ? 3 : 1);  // normal
    attr(11, (vcd_lo >> 13) & 3, color_size(a >> 14), 1);  // colour 0
    attr(12, (vcd_lo >> 15) & 3, color_size(a >> 18), 1);  // colour 1
    const u32 tex[8][2] = {
        {(a >> 21) & 1, (a >> 22) & 7},  {(b >> 0) & 1, (b >> 1) & 7},   {(b >> 9) & 1, (b >> 10) & 7},
        {(b >> 18) & 1, (b >> 19) & 7},  {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
        {(c >> 14) & 1, (c >> 15) & 7},  {(c >> 23) & 1, (c >> 24) & 7},
    };
    for (u32 t = 0; t < 8; t++)
        attr(13 + t, (vcd_hi >> (2 * t)) & 3, (tex[t][0] ? 2 : 1) * comp_size(tex[t][1]), 1);
    return l;
}
