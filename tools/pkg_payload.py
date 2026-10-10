#!/usr/bin/env python
"""Selective, read-only extractor for macOS installer payloads (pbzx/xz/gzip + cpio odc/newc). Nothing is executed.

usage: python -I tools/pkg_payload.py list    <Payload> [regex]
       python -I tools/pkg_payload.py extract <Payload> <outdir> <regex> [--max-bytes N]

Entries whose normalized path would leave <outdir> (absolute, '..') are skipped. Symlinks are listed but never created.
"""
import gzip, io, lzma, os, re, struct, sys


def open_stream(path):
    f = open(path, "rb")
    head = f.read(4)
    f.seek(0)
    if head == b"pbzx":
        f.read(4)
        f.read(8)                      # chunk size
        out = io.BytesIO()
        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                break
            flags, length = struct.unpack(">QQ", hdr)
            data = f.read(length)
            if data[:6] == b"\xfd7zXZ\x00":
                out.write(lzma.decompress(data))
            else:
                out.write(data)
        out.seek(0)
        return out
    if head[:2] == b"\x1f\x8b":
        return io.BytesIO(gzip.decompress(f.read()))
    if head[:6] == b"\xfd7zXZ\x00":
        return io.BytesIO(lzma.decompress(f.read()))
    return f


def entries(stream):
    """Yield (name, mode, size, data_offset) for cpio odc ('070707') and newc ('070701'/'070702') archives."""
    pos = 0
    while True:
        stream.seek(pos)
        h = stream.read(6)
        if h == b"070707":
            rest = stream.read(70)
            mode = int(rest[12:18], 8)
            namesize = int(rest[53:59], 8)
            size = int(rest[59:70], 8)
            name = stream.read(namesize)[:-1].decode("utf-8", "replace")
            off = stream.tell()
            nxt = off + size
        elif h in (b"070701", b"070702"):
            rest = stream.read(104)
            f = [int(rest[i * 8:(i + 1) * 8], 16) for i in range(13)]
            mode, size, namesize = f[1], f[6], f[11]
            name = stream.read(namesize)[:-1].decode("utf-8", "replace")
            hdr_end = stream.tell()
            off = (hdr_end + 3) & ~3
            nxt = (off + size + 3) & ~3
        else:
            return
        if name == "TRAILER!!!":
            return
        yield name, mode, size, off
        pos = nxt


def safe(out, name):
    p = os.path.normpath(os.path.join(out, name.lstrip("./")))
    return p if os.path.commonpath([os.path.abspath(out), os.path.abspath(p)]) == os.path.abspath(out) else None


def main():
    cmd, payload = sys.argv[1], sys.argv[2]
    s = open_stream(payload)
    if cmd == "list":
        rx = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
        for name, mode, size, _ in entries(s):
            if not rx or rx.search(name):
                print(f"{mode & 0o170000:o} {size:>10} {name}")
    elif cmd == "extract":
        out, rx = sys.argv[3], re.compile(sys.argv[4])
        cap = int(sys.argv[sys.argv.index("--max-bytes") + 1]) if "--max-bytes" in sys.argv else 64 << 20
        n = 0
        for name, mode, size, off in list(entries(s)):
            if (mode & 0o170000) != 0o100000 or not rx.search(name) or size > cap:
                continue
            dst = safe(out, name)
            if not dst:
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            s.seek(off)
            with open(dst, "wb") as w:
                w.write(s.read(size))
            n += 1
        print(f"extracted {n} files")


main()
