"""Minimal DOL reader shared by the analysis scripts."""
import struct

SECTION_NAMES = ['.init', '.text'] + [None] * 5 + ['extab', 'extabindex', '.ctors', '.dtors', '.rodata', '.data', '.sdata', '.sdata2'] + [None] * 4


class Dol:
    def __init__(self, path):
        d = self.data = open(path, 'rb').read()
        self.off = struct.unpack('>18I', d[0:72])
        self.addr = struct.unpack('>18I', d[72:144])
        self.size = struct.unpack('>18I', d[144:216])
        self.bss_addr, self.bss_size, self.entry = struct.unpack('>III', d[216:228])

    def sections(self, text_only=False):
        for i in range(7 if text_only else 18):
            if self.size[i]:
                yield i, self.addr[i], self.size[i], self.off[i]

    def section_of(self, a):
        for i, va, sz, _ in self.sections():
            if va <= a < va + sz:
                return i
        return None

    def is_text(self, a):
        i = self.section_of(a)
        return i is not None and i < 7

    def word(self, a):
        for _, va, sz, fo in self.sections():
            if va <= a < va + sz:
                p = fo + a - va
                return struct.unpack('>I', self.data[p:p + 4])[0]
        return None

    def code(self):
        for _, va, sz, fo in self.sections(True):
            for p in range(0, sz, 4):
                yield va + p, struct.unpack('>I', self.data[fo + p:fo + p + 4])[0]


def simm(w):
    v = w & 0xffff
    return v - 0x10000 if v & 0x8000 else v
