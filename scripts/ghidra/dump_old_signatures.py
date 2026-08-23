# Dump prologue bytes at known-good RVAs from a fully analyzed Ghidra project
# and write them as the signature file scripts/sigfile.py reads, so a patched
# EXE can be relocated without re-analyzing 225MB of UE5 binary.
#
# The bytes are the game's own machine code. Keep the output under scratch/
# (gitignored) - it must never be committed.
#
# Run against the OLD, fully analyzed project, passing an output path and one
# <name>:<rva>:<length> target per signature:
#
#   analyzeHeadless C:\temp <Project> -process <exe> -noanalysis \
#     -postScript dump_old_signatures.py \
#       ..\scratch\signatures.json gpv_prologue:<rva>:32
#
# Bytes come out literal. Hand-edit each relocated displacement to "??" before
# using the file across builds, or the scan only matches the build it came from.
import json

BASE = 0x140000000

args = getScriptArgs()
if len(args) < 2:
    raise Exception("usage: dump_old_signatures.py <out.json> <name>:<rva>:<len> ...")

out_path = args[0]
mem = currentProgram.getMemory()
fact = currentProgram.getAddressFactory()


def dump(rva, n):
    a = fact.getDefaultAddressSpace().getAddress(BASE + rva)
    out = []
    for i in range(n):
        out.append(mem.getByte(a.add(i)) & 0xFF)
    return out


sigs = {}
for spec in args[1:]:
    name, rva, length = spec.split(":")
    sigs[name] = " ".join("%02x" % b for b in dump(int(rva, 0), int(length, 0)))

with open(out_path, "w") as f:
    json.dump(sigs, f, indent=2, sort_keys=True)

print("Wrote %s (%d signature(s))" % (out_path, len(sigs)))
