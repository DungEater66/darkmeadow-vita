#!/usr/bin/env python3
"""Turn offsets from a crash report in log.txt into C++ function names.

  python3 tools/resolve_addr.py libUnrealEngine3.so 0x1234567 0x89abcd ...

Needs pyelftools; uses `c++filt` (binutils) for readable names if it is installed.
"""
import subprocess, sys
from elftools.elf.elffile import ELFFile

if len(sys.argv) < 3:
    sys.exit(__doc__)
elf = ELFFile(open(sys.argv[1], 'rb'))
syms = []
for secname in ('.dynsym', '.symtab'):
    sec = elf.get_section_by_name(secname)
    if sec:
        for s in sec.iter_symbols():
            if s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
                syms.append((s['st_value'] & ~1, s['st_size'], s.name))
syms.sort()

def demangle(n):
    try:
        return subprocess.run(['c++filt', n], capture_output=True, text=True).stdout.strip() or n
    except FileNotFoundError:
        return n

for a in sys.argv[2:]:
    addr = int(a, 16)
    best = None
    for start, size, name in syms:
        if start <= addr and (best is None or start > best[0]):
            best = (start, size, name)
    if best and (addr < best[0] + max(best[1], 1) + 0x1000):
        print(f"0x{addr:08x}  {demangle(best[2])}  +0x{addr - best[0]:x}" + ("" if addr < best[0] + best[1] else "  (past the symbol's size: approximate)"))
    else:
        print(f"0x{addr:08x}  (no symbol found)")
