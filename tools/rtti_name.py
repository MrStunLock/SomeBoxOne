#!/usr/bin/env python3
"""Print the MSVC RTTI class name for a vtable address (the qword before the vtable points at the complete object locator).
Usage: rtti_name.py <exe> <vtable-VA-hex> ...   Useful to learn which WinRT delegate/interface a vtable belongs to."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]; sp = pe + 24 + osz
secs = [struct.unpack_from('<8sIIII', d, sp + i * 40) for i in range(nsec)]
def off(rva):
    for _, vs, va, rs, rp in secs:
        if va <= rva < va + rs: return rp + rva - va
for a in sys.argv[2:]:
    va = int(a, 16)
    try:
        col = struct.unpack_from('<Q', d, off(va - base - 8))[0]
        td = struct.unpack_from('<I', d, off(col - base) + 12)[0]; o = off(td) + 16
        print(a, d[o:d.index(b'\0', o)].decode('latin1'))
    except Exception as e:
        print(a, 'no RTTI:', e)
