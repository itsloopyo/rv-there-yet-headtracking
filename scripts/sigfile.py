#!/usr/bin/env python3
"""Load masked byte signatures from a local, untracked file.

A signature is machine code lifted out of the shipped game executable, so it is
the game's copyrighted code and never gets committed here. Dump it from your own
legally owned install with scripts/ghidra/dump_old_signatures.py, which writes
the JSON file this module reads.

Format is {"<name>": "48 89 5c 24 ?? 55 ..."}, one masked-hex string per
signature, "??" marking a byte to wildcard (relocated displacements, mostly).
"""
import json
import os

DEFAULT_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "scratch", "signatures.json")


def _read():
    path = os.environ.get("RVTY_SIGNATURES", DEFAULT_PATH)
    if not os.path.exists(path):
        raise SystemExit(
            "signature file not found: %s\n"
            "Generate it from your own game install with\n"
            "  scripts/ghidra/dump_old_signatures.py\n"
            "run against a fully analyzed Ghidra project, or point\n"
            "RVTY_SIGNATURES at an existing file." % path)
    with open(path) as f:
        return path, json.load(f)


def load(name):
    """Return the signature as a list of ints, None for each wildcard byte."""
    path, sigs = _read()
    if name not in sigs:
        raise SystemExit("signature '%s' missing from %s" % (name, path))
    return [None if tok == "??" else int(tok, 16) for tok in sigs[name].split()]


def load_exact(name):
    """Return the signature as literal bytes; wildcards are rejected."""
    sig = load(name)
    if any(b is None for b in sig):
        raise SystemExit("signature '%s' has wildcards, but this scan needs "
                         "literal bytes" % name)
    return bytes(sig)
