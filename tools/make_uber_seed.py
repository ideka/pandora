#!/usr/bin/env python3
"""Builds data/initial_pipeline_cache.db: the ubershader pipeline configs (fixed-function state
only: blend, depth, cull, write masks, plus the module's line mode and features) that the given
aurora pipeline caches use or would use, so a first run can build them at startup instead of
waiting for each on first use. Aurora merges this "bundled initial pipeline cache" into its cache.

Each GX config is mapped to its ubershader config as make_uber_pipeline_config
(extern/aurora/lib/gx/ubershader.cpp) does; ubershader configs already in the caches are checked
against that mapping.

usage: make_uber_seed.py <out.db> <pipeline_cache.db>...
"""
import sqlite3
import struct
import sys

import xxhash

GX_TYPE = 1
CONFIG_VERSION = 13  # gx::GXPipelineConfigVersion
CONFIG_SIZE = 2772   # sizeof(gx::PipelineConfig)
PIPELINE_CACHE_SCHEMA = 1  # PipelineCacheSchema (lib/gfx/pipeline_cache.cpp)
UBER_MARKER = 0xA5
FEATURE_ALPHA_TEST, FEATURE_ADVANCED = 1, 2

# gx::PipelineConfig layout: version, msaaSamples, ShaderConfig (2720 bytes) at 8, then the
# fixed-function fields. ShaderConfig: fogType, vtxStride, a bitfield byte (lineMode:2,
# fogRangeEnabled:1, pad1:5), pad2, attrs (26 x 8), tevSwapTable (4 x 16), tevStages (16 x 132),
# tevStageCount, colorChannels (4 x 20), tcgs (8 x 20), alphaCompare (20), indStages, numIndStages.
SC = 8
BITS = SC + 2
PAD2 = SC + 3
TEV = SC + 4 + 26 * 8 + 4 * 16
TEV_SIZE = 132
TEV_COUNT = TEV + 16 * TEV_SIZE
ALPHA_CMP = TEV_COUNT + 4 + 4 * 20 + 8 * 20
GX_ALWAYS = 7


def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def uber_config(cfg, template):
    """The ubershader config for a GX config (bytes), given another ubershader config whose
    blanked shader part to reuse."""
    line_mode = cfg[BITS] & 3
    alpha_test = u32(cfg, ALPHA_CMP) != GX_ALWAYS or u32(cfg, ALPHA_CMP + 12) != GX_ALWAYS
    advanced = False
    for s in range(min(u32(cfg, TEV_COUNT), 16)):
        st = TEV + s * TEV_SIZE
        color_op, alpha_op = u32(cfg, st + 32), u32(cfg, st + 52)
        channel = u32(cfg, st + 88)
        ind_mtx, wrap_s, wrap_t = u32(cfg, st + 116), u32(cfg, st + 120), u32(cfg, st + 124)
        add_prev = cfg[st + 129]
        advanced = advanced or color_op >= 8 or alpha_op >= 8 or ind_mtx != 0 or wrap_s != 0 or \
            wrap_t != 0 or add_prev != 0 or channel in (7, 8)
    out = bytearray(cfg)
    out[SC:SC + 2720] = template[SC:SC + 2720]
    features = (FEATURE_ALPHA_TEST if alpha_test else 0) | (FEATURE_ADVANCED if advanced else 0)
    out[BITS] = line_mode | features << 3
    out[PAD2] = UBER_MARKER
    return bytes(out)


def main():
    out_path, sources = sys.argv[1], sys.argv[2:]
    configs, existing = [], []
    for path in sources:
        db = sqlite3.connect(f'file:{path}?mode=ro&immutable=1', uri=True)
        for (blob,) in db.execute('SELECT config FROM pipeline_cache WHERE type = ? AND config_version = ?',
                                  (GX_TYPE, CONFIG_VERSION)):
            blob = bytes(blob)
            if len(blob) != CONFIG_SIZE:
                continue
            (existing if blob[PAD2] == UBER_MARKER else configs).append(blob)
    if not existing:
        sys.exit('need at least one ubershader config in the sources (as a template and a check)')
    template = existing[0]
    derived = {uber_config(c, template) for c in configs}
    missing = [e for e in existing if e not in derived]
    print(f'{len(configs)} GX configs -> {len(derived)} ubershader configs; '
          f'{len(existing)} ubershader configs in the caches, {len(missing)} not derived '
          f'(from an older ubershader version, e.g. without module features)')
    seeds = derived

    out = sqlite3.connect(out_path)
    out.execute('DROP TABLE IF EXISTS pipeline_cache')
    out.execute('DROP TABLE IF EXISTS aurora_schema')
    out.execute('CREATE TABLE aurora_schema (value INTEGER)')
    out.execute('INSERT INTO aurora_schema VALUES (?)', (PIPELINE_CACHE_SCHEMA,))
    out.execute('CREATE TABLE pipeline_cache (type INTEGER NOT NULL, hash INTEGER NOT NULL, '
                'config_version INTEGER NOT NULL, config_size INTEGER NOT NULL, config BLOB NOT NULL, '
                'first_frame_used INTEGER NOT NULL, PRIMARY KEY (type, hash))')
    for blob in sorted(seeds):
        # xxh3_hash(config, ShaderType::GX): XXH3-64 of the bytes, seeded with the type
        h = xxhash.xxh3_64_intdigest(blob, seed=GX_TYPE)
        h = h - (1 << 64) if h >= 1 << 63 else h
        out.execute('INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, 0)', (GX_TYPE, h, CONFIG_VERSION, len(blob), blob))
    out.commit()
    out.execute('VACUUM')
    print(f'wrote {len(seeds)} configs to {out_path}')


if __name__ == '__main__':
    main()
