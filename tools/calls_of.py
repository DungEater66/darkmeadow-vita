#!/usr/bin/env python3
"""List what a function in libUnrealEngine3.so calls, in address order, with demangled names.
   usage: calls_of.py libUnrealEngine3.so <symbol-or-0xaddr> [max_bytes]   (needs /tmp/plt_<lib>.json from plt.py)"""
import sys, json, subprocess, bisect
from elftools.elf.elffile import ELFFile
from capstone import *
path, what = sys.argv[1], sys.argv[2]
maxb = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
d = open(path, 'rb').read(); elf = ELFFile(open(path, 'rb'))
plt = {int(a, 16): n for a, n in json.load(open('/tmp/plt_%s.json' % path.split('/')[-1])).items()}
funcs = []
for s in elf.get_section_by_name('.dynsym').iter_symbols():
    if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
        funcs.append((s['st_value'] & ~1, s['st_size'], s.name, s['st_value'] & 1))
funcs.sort(); keys = [f[0] for f in funcs]
def v2o(va):
    for g in elf.iter_segments():
        if g['p_type'] == 'PT_LOAD' and g['p_vaddr'] <= va < g['p_vaddr'] + g['p_filesz']: return va - g['p_vaddr'] + g['p_offset']
def dem(n):
    return subprocess.run(['c++filt', n], capture_output=True, text=True).stdout.strip()
def name_at(a):
    if a in plt: return 'IMPORT ' + plt[a]
    k = bisect.bisect_right(keys, a) - 1
    if k >= 0 and keys[k] <= a < keys[k] + max(funcs[k][1], 4): return dem(funcs[k][2])
    return hex(a)
if what.startswith('0x'): start = int(what, 16) & ~1; size = maxb; th = 0
else:
    f = [x for x in funcs if x[2] == what][0]; start, size, th = f[0], min(f[1], maxb), f[3]
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if th else CS_MODE_ARM)
o = v2o(start)
for i in md.disasm(d[o:o+size], start):
    if i.mnemonic in ('bl', 'blx', 'b') and i.op_str.startswith('#'):
        t = int(i.op_str[1:], 16)
        print(f"  {i.address:#09x} {i.mnemonic:4s} {name_at(t & ~1)[:150]}")
