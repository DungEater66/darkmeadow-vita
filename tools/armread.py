#!/usr/bin/env python3
"""Readable ARM disassembly of libUnrealEngine3.so: resolves PC-relative strings (ANSI/UTF-32) and calls made
   through GOT slots (ldr pc,[r4,r3]) to symbol names.     armread.py lib <symbol|0xaddr> [nbytes]"""
import sys, struct, subprocess
from elftools.elf.elffile import ELFFile
from capstone import *
from capstone.arm import *
p = sys.argv[1]; what = sys.argv[2]; nb = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
d = open(p, 'rb').read(); elf = ELFFile(open(p, 'rb'))
dynsym = elf.get_section_by_name('.dynsym')
def v2o(va):
    for g in elf.iter_segments():
        if g['p_type'] == 'PT_LOAD' and g['p_vaddr'] <= va < g['p_vaddr'] + g['p_filesz']: return va - g['p_vaddr'] + g['p_offset']
slot = {}
for sec in ('.rel.dyn', '.rel.plt'):
    s = elf.get_section_by_name(sec)
    if s:
        for r in s.iter_relocations():
            if r['r_info_sym']: slot[r['r_offset']] = dynsym.get_symbol(r['r_info_sym']).name
import bisect
_funcs = sorted((x['st_value'] & ~1, x['st_size'], x.name) for x in dynsym.iter_symbols()
                if x['st_shndx'] != 'SHN_UNDEF' and x['st_info']['type'] == 'STT_FUNC' and x['st_value'])
_keys = [f[0] for f in _funcs]
def sym_at(a):
    k = bisect.bisect_right(_keys, a & ~1) - 1
    if k >= 0 and _keys[k] <= (a & ~1) < _keys[k] + max(_funcs[k][1], 4): return _funcs[k][2]
def dem(n): return subprocess.run(['c++filt', n], capture_output=True, text=True).stdout.strip() or n
def u32(va): o = v2o(va); return None if o is None else struct.unpack('<I', d[o:o+4])[0]
def s8(va):
    o = v2o(va)
    if o is None: return None
    e = d.find(b'\0', o); b = d[o:e]
    return b.decode() if 2 <= len(b) < 100 and all(32 <= c < 127 for c in b) else None
def s32(va):
    o = v2o(va); out = []
    if o is None: return None
    for i in range(0, 160, 4):
        c = struct.unpack('<I', d[o+i:o+i+4])[0]
        if c == 0: break
        if not (32 <= c < 0x3000): return None
        out.append(chr(c))
    return ''.join(out) if len(out) >= 2 else None
if what.startswith('0x'): start = int(what, 16)
else:
    start = [s['st_value'] for s in dynsym.iter_symbols() if s.name == what][0]
    size = [s['st_size'] for s in dynsym.iter_symbols() if s.name == what][0]
    nb = min(nb, size)
md = Cs(CS_ARCH_ARM, CS_MODE_ARM); md.detail = True
regs = {}
for i in md.disasm(d[v2o(start):v2o(start) + nb], start):
    note = ''
    ops = i.operands
    if i.id == ARM_INS_LDR and len(ops) == 2 and ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC:
        v = u32(i.address + 8 + ops[1].mem.disp)
        if v is not None: regs[ops[0].reg] = ('lit', v); note = f'; lit={v:#x}'
    elif i.id == ARM_INS_ADD and len(ops) == 3 and ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_PC and ops[2].type == ARM_OP_REG and regs.get(ops[2].reg, (0,))[0] == 'lit':
        t = (i.address + 8 + regs[ops[2].reg][1]) & 0xffffffff
        regs[ops[0].reg] = ('addr', t)
        note = f'; -> {t:#x}'
        for f in (s32, s8):
            sv = f(t)
            if sv: note += f' "{sv}"'; break
    elif i.id == ARM_INS_LDR and len(ops) == 2 and ops[0].reg == ARM_REG_PC and ops[1].type == ARM_OP_MEM and ops[1].mem.base in regs and ops[1].mem.index in regs:
        a = regs[ops[1].mem.base]; b = regs[ops[1].mem.index]
        sl = a[1] + b[1]
        if sl in slot: note = f'; CALL {dem(slot[sl])[:110]}'
        else:
            tgt = u32(sl); nm = sym_at(tgt) if tgt else None
            note = f'; CALL {dem(nm)[:110]}' if nm else f'; CALL via GOT slot {sl:#x} -> {tgt:#x}' if tgt else f'; CALL via GOT slot {sl:#x}'
    print(f"{i.address:#09x}  {i.mnemonic:7s} {i.op_str:30s}{note}")
