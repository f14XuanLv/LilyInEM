"""Offline check of data/binary/*.json against the game executables.

For each manifest: SHA-256 of the EXE must be listed, and the bytes at every function RVA
(mapped through the PE section table) must equal the declared prefix.
Usage: python tools/verify_binary.py [EL|EM ...]   (paths come from devconfig.json)
"""
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_config():
    with open(os.path.join(ROOT, "devconfig.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    local = os.path.join(ROOT, "devconfig.local.json")
    if os.path.exists(local):
        with open(local, encoding="utf-8") as f:
            cfg.update(json.load(f))
    tokens = {"{magnoliaGame}": cfg["magnoliaGame"], "{liliesGame}": cfg["liliesGame"],
              "{localAppData}": os.environ.get("LOCALAPPDATA", "")}
    for inst in cfg["instances"].values():
        for k, v in inst.items():
            if isinstance(v, str):
                for t, r in tokens.items():
                    v = v.replace(t, r)
                inst[k] = v
    return cfg


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + opt_size
    out = []
    for i in range(count):
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, table + i * 40 + 8)
        out.append((va, max(vsize, raw_size), raw_ptr))
    return out


def read_rva(data, secs, rva, n):
    for va, size, raw in secs:
        if va <= rva < va + size:
            off = raw + (rva - va)
            return data[off:off + n]
    return None


def verify(name, exe_path):
    with open(os.path.join(ROOT, "data", "binary", f"{name}.json"), encoding="utf-8") as f:
        man = json.load(f)
    with open(exe_path, "rb") as f:
        data = f.read()
    ok = True
    digest = hashlib.sha256(data).hexdigest().upper()
    if digest not in [h.upper() for h in man["sha256"]]:
        print(f"[{name}] FAIL sha256 {digest} not listed")
        ok = False
    secs = sections(data)
    for fn, info in man["functions"].items():
        prefix = bytes.fromhex(info["prefix"])
        got = read_rva(data, secs, int(info["rva"], 16), len(prefix))
        if got != prefix:
            print(f"[{name}] FAIL {fn} @ {info['rva']}: expected {prefix.hex().upper()} got {got.hex().upper() if got else None}")
            ok = False
        else:
            print(f"[{name}] ok   {fn} @ {info['rva']}")
    return ok


def main():
    cfg = load_config()
    exes = {
        "EL": os.path.join(cfg["instances"]["guest"]["root"], cfg["instances"]["guest"]["exe"]),
        "EM": os.path.join(cfg["instances"]["host"]["root"], cfg["instances"]["host"]["exe"]),
    }
    names = sys.argv[1:] or list(exes)
    results = [verify(n, exes[n]) for n in names]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
