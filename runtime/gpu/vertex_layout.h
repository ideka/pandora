// Vertex layout from the CP vertex descriptor (VCD) and attribute format (VAT) registers: total
// vertex size and, for indexed attributes, where each index sits inside a vertex.
#pragma once

#include <array>

#include "../guest.h"

struct VertexLayout {
    u32 size = 0;
    // Indexed attributes, numbered as GX attributes 0-20 (0 = PNMTXIDX ... 9 = POS, 10 = NRM,
    // 11-12 = colours, 13-20 = texcoords). Array index = attr - 9.
    struct Index {
        u8 attr;
        u8 offset;  // byte offset inside the vertex
        u8 bytes;   // 1 (INDEX8) or 2 (INDEX16)
        u8 count;   // indices per vertex (3 for NBT with NORMALINDEX3)
    };
    std::array<Index, 12> indices{};
    u32 num_indices = 0;
};

// cpreg: the CP register file (0x50 VCD_LO, 0x60 VCD_HI, 0x70/0x80/0x90 + fmt VAT A/B/C).
VertexLayout vertex_layout(const u32* cpreg, u32 vat);
