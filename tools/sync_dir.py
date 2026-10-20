"""Make <dst> match <src>, rewriting only files whose contents differ (keeps unchanged mtimes).

With --strip-pc, generated chunk sources are passed through strip_pc.strip on the way.

Usage: sync_dir.py [--strip-pc] <src> <dst>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strip_pc import strip  # noqa: E402

args = sys.argv[1:]
strip_pc = '--strip-pc' in args
src, dst = [a for a in args if a != '--strip-pc']
changed = removed = stripped = seen = 0
wanted = set()
for root, _, files in os.walk(src):
    rel = os.path.relpath(root, src)
    os.makedirs(os.path.join(dst, rel), exist_ok=True)
    for name in files:
        s = os.path.join(root, name)
        d = os.path.normpath(os.path.join(dst, rel, name))
        wanted.add(d)
        with open(s, 'rb') as f:
            data = f.read()
        if strip_pc and name.startswith('chunk_') and name.endswith('.c'):
            text, r, t = strip(data.decode('utf-8'))
            data = text.encode('utf-8')
            stripped += r
            seen += t
        old = None
        if os.path.exists(d):
            with open(d, 'rb') as f:
                old = f.read()
        if old != data:
            with open(d, 'wb') as f:
                f.write(data)
            changed += 1
for root, _, files in os.walk(dst):
    for name in files:
        d = os.path.normpath(os.path.join(root, name))
        if d not in wanted:
            os.remove(d)
            removed += 1
if strip_pc:
    print(f'strip_pc: removed {stripped} of {seen} pc stores')
print(f'{dst}: {changed} files updated, {removed} removed')
