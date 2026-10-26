#!/usr/bin/env python3
"""List every direct `call rel32` to a function in a PE's .text section.
Usage: find_callers.py <exe> <function-VA-hex>      e.g. find_callers.py Minecraft.Client.exe 0x140673400"""
import struct, sys
d = open(sys.argv[1], 'rb').read(); target = int(sys.argv[2], 16)
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]; sp = pe + 24 + osz
for i in range(nsec):
    name, vs, va, rs, rp = struct.unpack_from('<8sIIII', d, sp + i * 40)
    if not (struct.unpack_from('<I', d, sp + i * 40 + 36)[0] & 0x20000000): continue          # executable sections only
    text = d[rp:rp + rs]
    for k in range(len(text) - 5):
        if text[k] == 0xE8 and base + va + k + 5 + struct.unpack_from('<i', text, k + 1)[0] == target:
            print('call from 0x%x  (%s+0x%x)' % (base + va + k, name.rstrip(b'\0').decode(), k))
