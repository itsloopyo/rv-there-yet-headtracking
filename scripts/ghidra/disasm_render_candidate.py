# Disassemble the render-caller candidate function and any vtable it derefs,
# to (a) confirm the GPV render path (controller+0x368 PCM deref, FOV write,
# call [reg+0x828]) and (b) see whether Ghidra resolved the indirect GPV call
# target - if the controller vtable is a known static address we can read
# slot 0x828/0x7f8 directly and get GPV's RVA without RTTI.

OUT  = r"C:\tmp\rvty_render_candidate.txt"
BASE = 0x140000000
FUNCS = [0x03c323a0, 0x03e72a70, 0x03ee70b0]

listing = currentProgram.getListing()
fact    = currentProgram.getAddressFactory()
fm      = currentProgram.getFunctionManager()
mem     = currentProgram.getMemory()

def addr(v): return fact.getDefaultAddressSpace().getAddress(v)

lines = []
def out(s):
    lines.append(s)

for frva in FUNCS:
    a = addr(BASE + frva)
    fn = fm.getFunctionContaining(a)
    out("\n==================================================================")
    out("FUNC rva 0x%x (abs %s)  ghidra=%s" % (frva, a, fn.getName() if fn else "?"))
    out("==================================================================")
    if fn is None:
        out("  no function")
        continue
    body = fn.getBody()
    inst = listing.getInstructionAt(fn.getEntryPoint())
    count = 0
    while inst is not None and body.contains(inst.getAddress()) and count < 260:
        mnem = inst.getMnemonicString()
        rva = inst.getAddress().getOffset() - BASE
        txt = inst.toString()
        extra = ""
        # For calls, show resolved reference targets (Ghidra may resolve
        # indirect calls through a recovered vtable).
        if mnem.lower().startswith("call") or "0x828" in txt or "0x7f8" in txt or "0x368" in txt or mnem.lower() == "movss":
            refs = inst.getReferencesFrom()
            tgts = []
            for r in refs:
                t = r.getToAddress()
                if t is not None:
                    tgts.append("%s(rva 0x%x)" % (t, t.getOffset() - BASE) if t.getOffset() >= BASE else str(t))
            if tgts:
                extra = "   -> " + ", ".join(tgts)
            out("  0x%08x  %s%s" % (rva, txt, extra))
        inst = inst.getNext()
        count += 1

with open(OUT, "w") as f:
    f.write("\n".join(lines))
print("Wrote %s" % OUT)
