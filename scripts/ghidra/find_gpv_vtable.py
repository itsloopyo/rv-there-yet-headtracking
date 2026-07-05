# Resolve GetPlayerViewPoint (and the FOV-writer vfn) RVAs from the UE class
# vtables, via MSVC RTTI. The render caller invokes them as
#   call [reg+0x828]   -> GetPlayerViewPoint  (slot index 0x828/8 = 261)
#   call [reg+0x7f8]   -> FOV-writer vfn       (slot index 0x7f8/8 = 255)
# where reg is the controller/PCM. We find each target class's TypeDescriptor
# (.?AV<name>@@), walk TD -> CompleteObjectLocator -> vtable, and read the two
# slots. The slot targets are the function RVAs the mod hooks / captures.
#
# RTTI walk mirrors find_camera_rtti.py.

OUT = r"C:\tmp\rvty_gpv_vtable.txt"
TARGET_CLASSES = ["APlayerController", "APlayerCameraManager"]
SLOTS = [0x7f8, 0x828]

mem  = currentProgram.getMemory()
fact = currentProgram.getAddressFactory()
image_base = currentProgram.getImageBase().getOffset()

def addr(v):
    return fact.getDefaultAddressSpace().getAddress(v)

def read_u32(a):
    try: return mem.getInt(a) & 0xffffffff
    except: return None

def read_u64(a):
    try: return mem.getLong(a) & 0xffffffffffffffff
    except: return None

def read_cstring_at(a, max_len=256):
    out = []
    cur = a
    for _ in range(max_len):
        try: b = mem.getByte(cur) & 0xff
        except: return None
        if b == 0:
            return "".join(chr(c) for c in out)
        if not (0x20 <= b <= 0x7e):
            return None
        out.append(b); cur = cur.add(1)
    return None

def blk(a):
    b = mem.getBlock(a)
    return b.getName() if b else "?"

# 1) Find TypeDescriptor for each target: scan for the mangled name
#    ".?AV<name>@@". The TD starts 0x10 before the name string.
def find_td(name):
    mangled = ".?AV" + name + "@@"
    target = [ord(c) for c in mangled]
    for block in mem.getBlocks():
        if not block.isInitialized(): continue
        if block.getName() not in (".data", ".rdata", "_RDATA"): continue
        start = block.getStart(); end = block.getEnd()
        cur = start
        first = target[0]
        while cur.compareTo(end) < 0:
            try: b = mem.getByte(cur) & 0xff
            except: cur = cur.add(1); continue
            if b == first:
                s = read_cstring_at(cur, len(mangled) + 2)
                if s == mangled:
                    return cur.subtract(0x10)  # TD start
            cur = cur.add(1)
    return None

# 2) COL points at the TD via +0xC (RVA). Sweep .rdata for a u32 == td_rva,
#    validate it's a COL (signature at -0xC == 1), then vtable = COL_ref + 8
#    where COL address sits at vtable[-1].
def find_vtable_for_td(td_addr):
    td_rva = td_addr.getOffset() - image_base
    results = []
    for block in mem.getBlocks():
        if not block.isInitialized(): continue
        if block.getName() not in (".rdata", "_RDATA"): continue
        cur = block.getStart(); end = block.getEnd()
        while cur.compareTo(end) < 0:
            v = read_u32(cur)
            if v == td_rva:
                col = cur.subtract(0xC)  # +0xC holds pTypeDescriptor
                sig = read_u32(col)
                if sig == 1:
                    # find the vtable whose [-1] slot == col address
                    # scan .rdata for a pointer to col
                    results.append(col)
            cur = cur.add(4)
    return results

def find_vtables_pointing_to(col):
    col_off = col.getOffset()
    vts = []
    for block in mem.getBlocks():
        if not block.isInitialized(): continue
        if block.getName() not in (".rdata", "_RDATA"): continue
        cur = block.getStart(); end = block.getEnd()
        while cur.compareTo(end) < 0:
            v = read_u64(cur)
            if v == col_off:
                vts.append(cur.add(8))  # vtable starts right after COL ptr
            cur = cur.add(8)
    return vts

lines = []
def out(s):
    lines.append(s); print(s)

out("GPV vtable resolver")
out("=" * 60)
for name in TARGET_CLASSES:
    out("\n## %s" % name)
    td = find_td(name)
    if td is None:
        out("  TypeDescriptor NOT found")
        continue
    out("  TD @ %s (rva 0x%x)" % (td, td.getOffset() - image_base))
    cols = find_vtable_for_td(td)
    out("  COLs: %d" % len(cols))
    for col in cols:
        vts = find_vtables_pointing_to(col)
        for vt in vts:
            out("  vtable @ %s (rva 0x%x) in %s" % (vt, vt.getOffset() - image_base, blk(vt)))
            for slot in SLOTS:
                idx = slot // 8
                fnptr = read_u64(vt.add(slot))
                if fnptr:
                    out("    slot 0x%x (idx %d) -> 0x%x (rva 0x%x)" % (
                        slot, idx, fnptr, fnptr - image_base))
                else:
                    out("    slot 0x%x (idx %d) -> <unreadable>" % (slot, idx))

with open(OUT, "w") as f:
    f.write("\n".join(lines))
print("\nWrote %s" % OUT)
