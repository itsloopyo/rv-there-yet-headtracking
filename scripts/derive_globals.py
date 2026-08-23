#!/usr/bin/env python3
"""Relocate GUObjectArray.ObjObjects and FNamePool for a patched EXE.

Locates the FUObjectArray allocator and the FName decoder by masked byte
signatures, then capstone-decodes each to read the rip-relative global it
references:

  ObjObjects : the unique `mov qword [rip+x], rax` in the allocator.
  FNamePool  : the first `lea r8, [rip+x]` in the decoder, cross-checked
               against `cmp byte [rip+y], 0` where pool == y + 0x267.

The signatures are prologue bytes of the game's own code, so they are not
committed: sigfile reads them from scratch/signatures.json, which you generate
locally from your own install (see scripts/sigfile.py).

Usage: py -3 derive_globals.py <path-to-exe>
"""
import sys
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

import sigfile

EXE = sys.argv[1]
pe = pefile.PE(EXE, fast_load=True)
text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
TVA = text.VirtualAddress
D = text.get_data()

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def masked_scan(sig):
    """sig: list of ints or None (wildcard). Returns list of RVAs."""
    hits = []
    n = len(sig)
    fixed = [(i, b) for i, b in enumerate(sig) if b is not None]
    first = sig[0]
    start = 0
    while True:
        i = D.find(bytes([first]), start)
        if i == -1 or i + n > len(D):
            break
        if all(D[i + k] == b for k, b in fixed):
            hits.append(TVA + i)
        start = i + 1
    return hits

ALLOC_SIG = sigfile.load("fuobjectarray_allocator")
DECODER_SIG = sigfile.load("fname_decoder")

def disasm(fn_rva, length=0x300):
    code = D[fn_rva - TVA: fn_rva - TVA + length]
    return list(md.disasm(code, fn_rva))

def rip_target(ins):
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            return ins.address + ins.size + op.mem.disp
    return None

print("EXE:", EXE)
fh = pe.FILE_HEADER; oh = pe.OPTIONAL_HEADER
print("PE: ts=0x%08x size=0x%08x csum=0x%08x" % (fh.TimeDateStamp, oh.SizeOfImage, oh.CheckSum))
print()

# --- ObjObjects ---
alloc = masked_scan(ALLOC_SIG)
print("allocator sig: %d hit(s): %s" % (len(alloc), ", ".join("0x%08x" % x for x in alloc)))
objobjects = None
if len(alloc) == 1:
    for ins in disasm(alloc[0]):
        if ins.mnemonic == "mov" and len(ins.operands) == 2:
            d, s = ins.operands
            if (d.type == X86_OP_MEM and d.mem.base == X86_REG_RIP
                    and s.type != X86_OP_MEM and ins.reg_name(s.reg) == "rax"):
                objobjects = rip_target(ins)
                print("  ObjObjects write @ fn+0x%x: %s %s -> 0x%08x"
                      % (ins.address - alloc[0], ins.mnemonic, ins.op_str, objobjects))
                break

# --- FNamePool ---
dec = masked_scan(DECODER_SIG)
print("decoder sig:   %d hit(s): %s" % (len(dec), ", ".join("0x%08x" % x for x in dec)))
fnamepool = None
flag = None
if len(dec) == 1:
    for ins in disasm(dec[0], 0x80):
        if ins.mnemonic == "lea" and ins.reg_name(ins.operands[0].reg) == "r8":
            t = rip_target(ins)
            if t:
                fnamepool = t
                print("  FNamePool lea @ fn+0x%x -> 0x%08x" % (ins.address - dec[0], t))
                break
    for ins in disasm(dec[0], 0x40):
        if ins.mnemonic == "cmp" and ins.operands[0].type == X86_OP_MEM \
           and ins.operands[0].mem.base == X86_REG_RIP:
            flag = rip_target(ins)
            print("  FName init flag @ fn+0x%x -> 0x%08x  (pool-flag=0x%x)"
                  % (ins.address - dec[0], flag, (fnamepool - flag) if fnamepool else 0))
            break

print()
print("=== RESULT ===")
print("kObjObjects = 0x%08x" % objobjects if objobjects else "kObjObjects = NOT FOUND")
print("kFNamePool  = 0x%08x" % fnamepool if fnamepool else "kFNamePool  = NOT FOUND")
if fnamepool and flag:
    print("flag check: pool - flag = 0x%x (expect 0x267)" % (fnamepool - flag))
pe.close()
