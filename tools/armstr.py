#!/usr/bin/env python3
"""Disassemble ARM code and resolve PC-relative string loads (ANSI and UTF-32).  armstr.py lib 0xaddr nbytes"""
import sys, struct
from elftools.elf.elffile import ELFFile
from capstone import *
from capstone.arm import *
p, start, n = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
d = open(p, 'rb').read(); elf = ELFFile(open(p, 'rb'))
def v2o(va):
    for g in elf.iter_segments():
        if g['p_type'] == 'PT_LOAD' and g['p_vaddr'] <= va < g['p_vaddr'] + g['p_filesz']: return va - g['p_vaddr'] + g['p_offset']
def u32(va): o = v2o(va); return None if o is None else struct.unpack('<I', d[o:o+4])[0]
def s8(va):
    o = v2o(va)
    if o is None: return None
    e = d.find(b'\0', o); b = d[o:e]
    return b.decode() if 2 <= len(b) < 120 and all(32 <= c < 127 for c in b) else None
def s32(va):
    o = v2o(va); out = []
    if o is None: return None
    for i in range(0, 200, 4):
        c = struct.unpack('<I', d[o+i:o+i+4])[0]
        if c == 0: break
        if not (32 <= c < 0x3000): return None
        out.append(chr(c))
    return ''.join(out) if len(out) >= 2 else None
md = Cs(CS_ARCH_ARM, CS_MODE_ARM); md.detail = True
regs = {}
for i in md.disasm(d[v2o(start):v2o(start) + n], start):
    note = ''
    if i.id == ARM_INS_LDR and i.operands[1].type == ARM_OP_MEM and i.operands[1].mem.base == ARM_REG_PC:
        lit = i.address + 8 + i.operands[1].mem.disp
        v = u32(lit)
        if v is not None: regs[i.operands[0].reg] = v; note = f'; lit={v:#x}'
    elif i.id == ARM_INS_ADD and i.operands[-1].type == ARM_OP_REG and i.operands[-1].reg == ARM_REG_PC and len(i.operands) == 3 and i.operands[1].reg == ARM_REG_PC:
        r = i.operands[2].reg if i.operands[2].type == ARM_OP_REG else None
    if i.id == ARM_INS_ADD and len(i.operands) == 3 and i.operands[1].type == ARM_OP_REG and i.operands[1].reg == ARM_REG_PC and i.operands[2].type == ARM_OP_REG and i.operands[2].reg in regs:
        t = (i.address + 8 + regs[i.operands[2].reg]) & 0xffffffff
        note = f'; -> {t:#x}'
        for f in (s32, s8):
            sv = f(t)
            if sv: note += f' "{sv}"'; break
    print(f"{i.address:#09x}  {i.mnemonic:7s} {i.op_str:34s}{note}")
