# Full instruction stream of the FMinimalViewInfo builder around its GPV call,
# to see how RAX (the controller whose vtable holds GPV at slot 0x828) is
# loaded. If RAX traces back to a static/known vtable we can read slot 0x828
# and get GPV's RVA without RTTI or runtime capture.

OUT  = r"C:\tmp\rvty_builder_setup.txt"
BASE = 0x140000000
LO   = 0x03e72a70
HI   = 0x03e72cc0

listing = currentProgram.getListing()
fact    = currentProgram.getAddressFactory()
def addr(v): return fact.getDefaultAddressSpace().getAddress(v)

lines = []
inst = listing.getInstructionAt(addr(BASE + LO))
while inst is not None:
    rva = inst.getAddress().getOffset() - BASE
    if rva > HI: break
    lines.append("  0x%08x  %s" % (rva, inst.toString()))
    inst = inst.getNext()

with open(OUT, "w") as f:
    f.write("\n".join(lines))
print("Wrote %s (%d lines)" % (OUT, len(lines)))
