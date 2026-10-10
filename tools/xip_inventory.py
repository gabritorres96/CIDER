#!/usr/bin/env python
"""Streaming, read-only inventory of an Apple .xip (xar + pbzx + cpio). Nothing is executed or installed.

usage: python -I tools/xip_inventory.py <file.xip> <listing.txt> [<outdir> <regex> [--max-bytes N]]

Writes "<mode-octal> <size> <path>" for every cpio entry to <listing.txt>. With <outdir> and <regex>, regular files whose path matches
and whose size is <= --max-bytes (default 32 MiB) are also extracted (paths are confined to <outdir>; symlinks are never created).
"""
import lzma, os, re, struct, sys, xml.etree.ElementTree as ET, zlib


def xar_member(f, wanted_prefix="Content"):
    f.seek(0)
    magic, hsize, ver, toc_c, toc_u, alg = struct.unpack(">4sHHQQI", f.read(28))
    if magic != b"xar!":
        raise SystemExit("not a xar/xip file")
    f.seek(hsize)
    toc = ET.fromstring(zlib.decompress(f.read(toc_c)))
    heap = hsize + toc_c
    for file in toc.iter("file"):
        name = file.findtext("name") or ""
        d = file.find("data")
        if name.startswith(wanted_prefix) and d is not None:
            enc = d.find("encoding").get("style") if d.find("encoding") is not None else ""
            return heap + int(d.findtext("offset")), int(d.findtext("length")), enc
    raise SystemExit("no Content member")


def pbzx_chunks(f, off, length):
    f.seek(off)
    if f.read(4) != b"pbzx":
        raise SystemExit("Content is not pbzx")
    f.read(8)
    end = off + length
    while f.tell() < end:
        hdr = f.read(16)
        if len(hdr) < 16:
            return
        _, clen = struct.unpack(">QQ", hdr)
        data = f.read(clen)
        yield lzma.decompress(data) if data[:6] == b"\xfd7zXZ\x00" else data


class Reader:
    def __init__(self, it):
        self.it, self.buf = it, bytearray()

    def need(self, n):
        while len(self.buf) < n:
            try:
                self.buf += next(self.it)
            except StopIteration:
                return False
        return True

    def take(self, n):
        if not self.need(n):
            raise EOFError
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def skip(self, n):
        while n:
            if not self.buf and not self.need(1):
                raise EOFError
            k = min(n, len(self.buf))
            del self.buf[:k]
            n -= k


def main():
    xip, listing = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    rx = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
    cap = int(sys.argv[sys.argv.index("--max-bytes") + 1]) if "--max-bytes" in sys.argv else 32 << 20
    f = open(xip, "rb")
    off, length, _ = xar_member(f)
    r = Reader(pbzx_chunks(f, off, length))
    n = files = 0
    with open(listing, "w", encoding="utf-8") as lst:
        while True:
            if not r.need(6):
                break
            h = r.take(6)
            if h != b"070707":
                break
            rest = r.take(70)
            mode, namesize, size = int(rest[12:18], 8), int(rest[53:59], 8), int(rest[59:70], 8)
            name = r.take(namesize)[:-1].decode("utf-8", "replace")
            if name == "TRAILER!!!":
                break
            lst.write(f"{mode & 0o170000:o} {size} {name}\n")
            n += 1
            want = out and rx and (mode & 0o170000) == 0o100000 and rx.search(name) and size <= cap
            if want:
                dst = os.path.normpath(os.path.join(out, name.lstrip("./")))
                if os.path.commonpath([os.path.abspath(out), os.path.abspath(dst)]) == os.path.abspath(out):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as w:
                        w.write(r.take(size))
                    files += 1
                    continue
            r.skip(size)
    print(f"{n} entries, {files} extracted")


main()
