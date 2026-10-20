"""Make <dst> match <src>, rewriting only files whose contents differ (keeps unchanged mtimes).

Usage: sync_dir.py <src> <dst>
"""
import filecmp
import os
import shutil
import sys

src, dst = sys.argv[1], sys.argv[2]
changed = removed = 0
wanted = set()
for root, _, files in os.walk(src):
    rel = os.path.relpath(root, src)
    os.makedirs(os.path.join(dst, rel), exist_ok=True)
    for name in files:
        s = os.path.join(root, name)
        d = os.path.normpath(os.path.join(dst, rel, name))
        wanted.add(d)
        if not os.path.exists(d) or not filecmp.cmp(s, d, shallow=False):
            shutil.copyfile(s, d)
            changed += 1
for root, _, files in os.walk(dst):
    for name in files:
        d = os.path.normpath(os.path.join(root, name))
        if d not in wanted:
            os.remove(d)
            removed += 1
print(f'{dst}: {changed} files updated, {removed} removed')
