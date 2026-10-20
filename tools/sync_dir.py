"""Make <dst> match <src>, rewriting only files whose contents differ (keeps unchanged mtimes).

With --strip-pc, generated chunk sources are passed through strip_pc.strip on the way; with
--prune-entries <symbols.txt> <main.dol> <hle.cpp>, their entry switches through
prune_entries.prune.

Usage: sync_dir.py [--strip-pc] [--prune-entries <symbols> <dol> <hle>] <src> <dst>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prune_entries  # noqa: E402
from strip_pc import strip  # noqa: E402

args = sys.argv[1:]
strip_pc = '--strip-pc' in args
if strip_pc:
    args.remove('--strip-pc')
prune = None
if '--prune-entries' in args:
    i = args.index('--prune-entries')
    prune = args[i + 1:i + 4]
    del args[i:i + 4]
src, dst = args

is_chunk = lambda name: name.startswith('chunk_') and name.endswith('.c')
files = {}  # relative path -> bytes
for root, _, names in os.walk(src):
    for name in names:
        path = os.path.join(root, name)
        with open(path, 'rb') as f:
            files[os.path.relpath(path, src)] = f.read()

stripped = seen = 0
if strip_pc:
    for rel, data in files.items():
        if is_chunk(os.path.basename(rel)):
            text, r, t = strip(data.decode('utf-8'))
            files[rel] = text.encode('utf-8')
            stripped += r
            seen += t
    print(f'strip_pc: removed {stripped} of {seen} pc stores')

if prune:
    chunks = [rel for rel in files if is_chunk(os.path.basename(rel))]
    entries = prune_entries.collect([files[rel].decode('utf-8') for rel in chunks], *prune)
    kept = total = 0
    for rel in chunks:
        text, k, t = prune_entries.prune(files[rel].decode('utf-8'), entries)
        files[rel] = text.encode('utf-8')
        kept += k
        total += t
    print(f'prune_entries: kept {kept} of {total} entry cases ({len(entries)} enterable addresses)')

changed = removed = 0
wanted = set()
for rel, data in files.items():
    d = os.path.normpath(os.path.join(dst, rel))
    wanted.add(d)
    os.makedirs(os.path.dirname(d), exist_ok=True)
    old = None
    if os.path.exists(d):
        with open(d, 'rb') as f:
            old = f.read()
    if old != data:
        with open(d, 'wb') as f:
            f.write(data)
        changed += 1
for root, _, names in os.walk(dst):
    for name in names:
        d = os.path.normpath(os.path.join(root, name))
        if d not in wanted:
            os.remove(d)
            removed += 1
print(f'{dst}: {changed} files updated, {removed} removed')
