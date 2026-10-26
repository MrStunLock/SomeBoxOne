#!/usr/bin/env python3
"""Decode a 16-byte GUID/IID stored at a virtual address (e.g. the IID passed to QueryInterface). Usage: guid_at.py <exe> <VA-hex> ..."""
import struct, sys, uuid
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]; sp = pe + 24 + osz
secs = [struct.unpack_from('<8sIIII', d, sp + i * 40) for i in range(nsec)]
for a in sys.argv[2:]:
    rva = int(a, 16) - base
    for _, vs, va, rs, rp in secs:
        if va <= rva < va + rs: print(a, uuid.UUID(bytes_le=d[rp + rva - va: rp + rva - va + 16])); break
    else: print(a, 'not in a section')
