#!/usr/bin/env python3
"""Where the buckets of an EXR are on disk, without decoding the image.

    python3 exr-chunks.py FILE.exr [--pixel X Y]

Stdlib only, so it also runs in Nuke (nuke -t exr-chunks.py ...) or RV's
Python. Prints no pixel values: the headers, the chunk (bucket / strip)
table, a scan of the chunks in file order, and a map of each tiled part's
buckets:

    #  on disk, whole (and, for zip, inflates to the right size)
    %  on disk, cut short (the file ends inside it)
    x  on disk, whole, but its zip data does not inflate
    !  the table points at it, but there is no such bucket there
    2  on disk more than once (the first copy is what OpenEXR reads)
    .  not on disk

--pixel X Y (EXR coordinates, y down from the top; RV's inspector counts y
up from the bottom) also details the bucket holding that pixel.
"""

import argparse
import os
import struct
import sys
import time
import zlib

TILED, LONG_NAMES, DEEP, MULTIPART = 0x200, 0x400, 0x800, 0x1000
COMPRESSIONS = ["none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a",
                "dwaa", "dwab", "htj2k256", "htj2k32"]
LINES_PER_CHUNK = {0: 1, 1: 1, 2: 1, 3: 16, 4: 32, 5: 16, 6: 32, 7: 32,
                   8: 32, 9: 256, 10: 256, 11: 32}
LINE_ORDERS = ["increasing_y", "decreasing_y", "random_y"]
LEVEL_MODES = ["one_level", "mipmap", "ripmap"]
PIXEL_BYTES = {0: 4, 1: 2, 2: 4}


def cstr(f):
    out = b""
    while True:
        c = f.read(1)
        if not c:
            raise EOFError("end of file inside a header")
        if c == b"\0":
            return out.decode("latin-1")
        out += c


def read_header(f):
    h = {}
    while True:
        name = cstr(f)
        if not name:
            return h
        typ = cstr(f)
        (size,) = struct.unpack("<i", f.read(4))
        data = f.read(size)
        if typ == "box2i":
            h[name] = struct.unpack("<4i", data)
        elif typ == "tiledesc":
            xs, ys, mode = struct.unpack("<IIB", data)
            h[name] = (xs, ys, mode & 0xF, mode >> 4)
        elif typ in ("compression", "lineOrder"):
            h[name] = data[0]
        elif typ == "int":
            h[name] = struct.unpack("<i", data)[0]
        elif typ == "string":
            h[name] = data.decode("latin-1")
        elif typ == "chlist":
            chans, i = [], 0
            while data[i] != 0:
                end = data.index(b"\0", i)
                cname = data[i:end].decode("latin-1")
                ptype, _, xs, ys = struct.unpack_from("<iB3xii", data, end + 1)
                chans.append((cname, ptype, xs, ys))
                i = end + 1 + 16
            h[name] = chans


def round_log2(x, up):
    n = 0
    if up:
        while (1 << n) < x:
            n += 1
    else:
        while (2 << n) <= x:
            n += 1
    return n


def level_size(size, l, up):
    return max(1, (size + (1 << l) - 1) >> l if up else size >> l)


class Part:
    def __init__(self, index, h, flags):
        self.index = index
        self.h = h
        self.name = h.get("name", "")
        x0, y0, x1, y1 = h["dataWindow"]
        self.dw = (x0, y0, x1, y1)
        self.w, self.h_px = x1 - x0 + 1, y1 - y0 + 1
        self.type = h.get("type") or (
            "tiledimage" if flags & TILED else "scanlineimage")
        self.tiled = "tile" in self.type
        self.deep = "deep" in self.type
        self.comp = h.get("compression", 0)
        self.order = h.get("lineOrder", 0)
        self.chans = h.get("channels", [])
        self.keys = {}  # (lx, ly, tx, ty) or y -> table index
        self.levels = {}  # (lx, ly) -> (width, height, tiles x, tiles y)
        if self.tiled:
            xs, ys, mode, rnd = h["tiles"]
            self.tile = (xs, ys)
            self.mode = mode
            up = rnd == 1
            if mode == 0:
                pairs = [(0, 0)]
            elif mode == 1:
                n = round_log2(max(self.w, self.h_px), up) + 1
                pairs = [(l, l) for l in range(n)]
            else:
                nx = round_log2(self.w, up) + 1
                ny = round_log2(self.h_px, up) + 1
                pairs = [(lx, ly) for ly in range(ny) for lx in range(nx)]
            for lx, ly in pairs:
                lw = level_size(self.w, lx, up)
                lh = level_size(self.h_px, ly, up)
                ntx, nty = -(-lw // xs), -(-lh // ys)
                self.levels[(lx, ly)] = (lw, lh, ntx, nty)
                for ty in range(nty):
                    for tx in range(ntx):
                        self.keys[(lx, ly, tx, ty)] = len(self.keys)
        else:
            self.lines = LINES_PER_CHUNK.get(self.comp, 1)
            for i, y in enumerate(range(y0, y1 + 1, self.lines)):
                self.keys[y] = i
        self.count = h.get("chunkCount", len(self.keys))

    def describe(self):
        comp = (COMPRESSIONS[self.comp] if self.comp < len(COMPRESSIONS)
                else str(self.comp))
        order = (LINE_ORDERS[self.order] if self.order < len(LINE_ORDERS)
                 else str(self.order))
        s = "part %d %r: %s, data window %s (%dx%d), %s, %s, %d chunks" % (
            self.index, self.name, self.type, self.dw, self.w, self.h_px,
            comp, order, self.count)
        if self.tiled:
            s += ", %dx%d tiles, %s" % (self.tile[0], self.tile[1],
                                        LEVEL_MODES[self.mode])
        chans = ", ".join(c[0] for c in self.chans)
        return s + "\n    channels: " + chans

    def unpacked_size(self, key):
        """Bytes a flat chunk inflates to; None when unknown (deep)."""
        if self.deep:
            return None
        if self.tiled:
            lx, ly, tx, ty = key
            lw, lh = self.levels[(lx, ly)][:2]
            w = min(self.tile[0], lw - tx * self.tile[0])
            h = min(self.tile[1], lh - ty * self.tile[1])
        else:
            w = self.w
            h = min(self.lines, self.dw[3] - key + 1)
        return sum(w * h * PIXEL_BYTES.get(c[1], 4) for c in self.chans)


class Chunk:
    def __init__(self, part, key, offset, size, end):
        self.part, self.key, self.offset = part, key, offset
        self.size, self.end = size, end  # end: where its data stops


def read_leader(f, parts, offset, file_size, multipart):
    """The chunk starting at `offset`, or (None, reason)."""
    f.seek(offset)
    try:
        p = parts[0]
        if multipart:
            (pi,) = struct.unpack("<i", f.read(4))
            if not 0 <= pi < len(parts):
                return None, "part number %d out of range" % pi
            p = parts[pi]
        if p.tiled:
            key = struct.unpack("<4i", f.read(16))
            tx, ty, lx, ly = key
            key = (lx, ly, tx, ty)
        else:
            (key,) = struct.unpack("<i", f.read(4))
        if key not in p.keys:
            return None, "no chunk %s in part %d" % (key, p.index)
        if p.deep:
            a, b, _ = struct.unpack("<3q", f.read(24))
            size = a + b
        else:
            (size,) = struct.unpack("<i", f.read(4))
        if size < 0 or size > file_size:
            return None, "data size %d out of range" % size
        return Chunk(p, key, offset, size, f.tell() + size), None
    except struct.error:
        return None, "end of file inside a chunk header"


def inflate_report(f, c):
    """How a zip chunk's data inflates: where it ends or breaks, and to how
    many bytes against the size its pixels need."""
    if c.part.comp not in (2, 3) or c.part.deep or c.end > os.fstat(
            f.fileno()).st_size:
        return None
    want = c.part.unpacked_size(c.key)
    if want is None or c.size >= want:
        return "stored raw (%d bytes, %s needed)" % (c.size, want)
    f.seek(c.end - c.size)
    data = f.read(c.size)
    if len(data) < 2 or (data[0] * 256 + data[1]) % 31 or data[0] & 0xF != 8:
        return "no zlib header: %s" % data[:2].hex()
    # Raw deflate past the 2-byte header, so the checksum is not checked and
    # the exact length comes out; a byte at a time, so an error says where.
    d = zlib.decompressobj(-15)
    out, used = bytearray(), 2
    try:
        while used < len(data) and not d.eof:
            out += d.decompress(data[used:used + 1])
            used += 1
        out += d.flush()
    except zlib.error as e:
        return ("deflate error at byte %d of %d, after inflating %d of %d: %s"
                % (used, c.size, len(out), want, e))
    if not d.eof:
        return ("deflate stream not finished at the end of its %d bytes: %d "
                "of %d inflated" % (c.size, len(out), want))
    rest = data[used:]
    check = "no checksum after it"
    if len(rest) >= 4:
        stored = struct.unpack(">I", rest[:4])[0]
        actual = zlib.adler32(bytes(out))
        check = ("checksum matches" if stored == actual else
                 "checksum %08x, data gives %08x (%d bits differ)" % (
                     stored, actual, bin(stored ^ actual).count("1")))
    return ("deflate stream ends at byte %d of %d (%d after it); inflates to "
            "%d bytes, %d needed%s; %s" % (
                used, c.size, len(rest), len(out), want,
                "" if len(out) == want else " (%+d)" % (len(out) - want),
                check))


def find_bit_flip(f, c):
    """The one bit whose flip makes a zip chunk inflate to its size with a
    matching checksum: (byte offset in the chunk's data, bit), or None.
    Tries them all, so takes a few seconds per 10 KB."""
    want = c.part.unpacked_size(c.key)
    f.seek(c.end - c.size)
    data = f.read(c.size)
    if want is None or len(data) < 7 or c.size >= want:
        return None
    stored = struct.unpack(">I", data[-4:])[0]
    body = bytearray(data[2:-4])
    for i in range(len(body)):
        orig = body[i]
        for bit in range(8):
            body[i] = orig ^ (1 << bit)
            d = zlib.decompressobj(-15)
            try:
                out = d.decompress(bytes(body), want + 1)
            except zlib.error:
                continue
            if (len(out) == want and d.eof and
                    zlib.adler32(out) == stored):
                return i + 2, bit
        body[i] = orig
    return None


def best_guess(f, c, x, y):
    """A damaged zip chunk decoded as far as it goes, checksum ignored and
    missing bytes zero (what a lenient reader shows): the channel values at
    pixel (x, y), and how many of the chunk's values are absurd (NaN, inf or
    beyond +-1e4). None if not a zip chunk."""
    p = c.part
    want = p.unpacked_size(c.key)
    if p.comp not in (2, 3) or p.deep or want is None or c.end > os.fstat(
            f.fileno()).st_size:
        return None
    f.seek(c.end - c.size)
    data = f.read(c.size)
    if c.size >= want:
        raw = bytearray(data[:want])
    else:
        d = zlib.decompressobj(-15)
        raw = bytearray()
        for i in range(2, len(data)):
            try:
                raw += d.decompress(data[i:i + 1])
            except zlib.error:
                break
            if d.eof:
                break
        raw = raw[:want] + bytearray(max(0, want - len(raw)))
        for i in range(1, want):
            raw[i] = (raw[i - 1] + raw[i] - 128) & 255
        half = (want + 1) // 2
        out = bytearray(want)
        out[0::2] = raw[:len(out[0::2])]
        out[1::2] = raw[half:half + len(out[1::2])]
        raw = out
    if p.tiled:
        lx, ly, tx, ty = c.key
        lw, lh = p.levels[(lx, ly)][:2]
        w = min(p.tile[0], lw - tx * p.tile[0])
        h = min(p.tile[1], lh - ty * p.tile[1])
        ox, oy = p.dw[0] + tx * p.tile[0], p.dw[1] + ty * p.tile[1]
    else:
        w, h = p.w, min(p.lines, p.dw[3] - c.key + 1)
        ox, oy = p.dw[0], c.key
    fmt = {0: "<I", 1: "<e", 2: "<f"}
    values, absurd, total, pos = {}, 0, 0, 0
    for row in range(h):
        for name, ptype, _, _ in p.chans:
            n = PIXEL_BYTES.get(ptype, 4)
            for col in range(w):
                (v,) = struct.unpack_from(fmt.get(ptype, "<f"), raw, pos)
                pos += n
                total += 1
                if ptype != 0 and (v != v or abs(v) > 1e4):
                    absurd += 1
                if (ox + col, oy + row) == (x, y):
                    values[name] = v
    return values, absurd, total


def inflates(f, c):
    """False if a zip chunk is whole but does not inflate to its size."""
    if c.part.comp not in (2, 3) or c.part.deep:
        return True
    want = c.part.unpacked_size(c.key)
    if want is None or c.size >= want:
        return True  # stored raw
    f.seek(c.end - c.size)
    try:
        return len(zlib.decompress(f.read(c.size))) == want
    except zlib.error:
        return False


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("exr")
    ap.add_argument("--pixel", nargs=2, type=int, metavar=("X", "Y"))
    ap.add_argument("--repair", action="store_true",
                    help="with --pixel: look for the one flipped bit that "
                    "breaks that chunk")
    a = ap.parse_args()

    file_size = os.path.getsize(a.exr)
    with open(a.exr, "rb") as f:
        magic, flags = struct.unpack("<ii", f.read(8))
        if magic != 20000630:
            sys.exit("not an EXR")
        multipart = bool(flags & MULTIPART)
        headers = []
        while True:
            h = read_header(f)
            if not h:
                break
            headers.append(h)
            if not multipart:
                break
        parts = [Part(i, h, flags) for i, h in enumerate(headers)]
        print("%s: %d bytes, version %d, flags 0x%x" % (
            a.exr, file_size, flags & 0xFF, flags & ~0xFF))
        for p in parts:
            print(p.describe())

        # The chunk tables, one per part; zeros until the writer closes.
        tables = []
        for p in parts:
            raw = f.read(8 * p.count)
            n = len(raw) // 8
            tables.append(list(struct.unpack("<%dQ" % n, raw[:8 * n])))
        first = f.tell()
        print()
        for p, t in zip(parts, tables):
            valid = sum(1 for o in t if first <= o < file_size)
            zeros = sum(1 for o in t if o == 0)
            print("part %d table: %d of %d entries point into the file, "
                  "%d zero%s" % (p.index, valid, p.count, zeros,
                                 " (never closed by its writer)"
                                 if zeros == p.count else ""))

        # Every chunk in file order, as OpenEXR does when the table is bad.
        found = {}  # (part, key) -> [Chunk]
        offset, stop = first, None
        while offset < file_size:
            c, why = read_leader(f, parts, offset, file_size, multipart)
            if c is None:
                stop = "stopped at byte %d: %s" % (offset, why)
                break
            found.setdefault((c.part.index, c.key), []).append(c)
            offset = c.end
        n = sum(len(v) for v in found.values())
        print("scan: %d chunks in file order from byte %d; %s" % (
            n, first, stop or "reached the end of the file"))
        if stop:
            print("    %d bytes after it are unread (a bucket after a bad one "
                  "is lost to OpenEXR's rebuild too)" % (file_size - offset))

        # Chunks the table knows of that the scan did not reach.
        disagree = 0
        for p, t in zip(parts, tables):
            for key, i in p.keys.items():
                if i >= len(t) or not first <= t[i] < file_size:
                    continue
                have = found.get((p.index, key), [])
                if any(c.offset == t[i] for c in have):
                    continue
                c, _ = read_leader(f, parts, t[i], file_size, multipart)
                if c is not None and c.key == key and c.part is p:
                    found.setdefault((p.index, key), []).append(c)
                else:
                    found.setdefault((p.index, key), []).append(
                        Chunk(p, key, t[i], -1, -1))
                disagree += 1
        if disagree:
            print("table: %d entries point where the scan found no such "
                  "chunk" % disagree)

        def state(p, key):
            cs = found.get((p.index, key), [])
            if not cs:
                return "."
            c = cs[0]
            if c.size < 0:
                return "!"
            if c.end > file_size:
                return "%"
            if len(cs) > 1:
                return "2"
            return "#" if inflates(f, c) else "x"

        for p in parts:
            print()
            if p.tiled:
                ntx, nty = p.levels[(0, 0)][2:]
                cells = [[state(p, (0, 0, tx, ty)) for tx in range(ntx)]
                         for ty in range(nty)]
                counts = {}
                for row in cells:
                    for s in row:
                        counts[s] = counts.get(s, 0) + 1
                print("part %d, full-res buckets (%dx%d, top row first): %s"
                      % (p.index, ntx, nty, "  ".join(
                          "%s %d" % kv for kv in sorted(counts.items()))))
                for ty, row in enumerate(cells):
                    print("  %4d %s" % (p.dw[1] + ty * p.tile[1], "".join(row)))
            else:
                cells = [state(p, y) for y in sorted(p.keys)]
                bad = [i for i, s in enumerate(cells) if s != "#"]
                print("part %d: %d of %d strips whole%s" % (
                    p.index, len(cells) - len(bad), len(cells),
                    ", first missing at y=%d" % (p.dw[1] + bad[0] * p.lines)
                    if bad else ""))

        if a.pixel:
            x, y = a.pixel
            for p in parts:
                x0, y0, x1, y1 = p.dw
                if not (x0 <= x <= x1 and y0 <= y <= y1):
                    continue
                if p.tiled:
                    key = (0, 0, (x - x0) // p.tile[0], (y - y0) // p.tile[1])
                else:
                    key = y0 + (y - y0) // p.lines * p.lines
                i = p.keys[key]
                t = tables[p.index]
                print()
                print("pixel %d,%d -> part %d chunk %s (table index %d): %s"
                      % (x, y, p.index, key, i, state(p, key)))
                print("    table entry: %d" % (t[i] if i < len(t) else -1))
                print("    unpacked size: %s" % p.unpacked_size(key))
                for c in found.get((p.index, key), []):
                    if c.size < 0:
                        print("    at %d: not a chunk header for it" % c.offset)
                    else:
                        print("    at %d: %d bytes of data, %s" % (
                            c.offset, c.size, "cut short"
                            if c.end > file_size else "whole"))
                        why = inflate_report(f, c)
                        if why:
                            print("        " + why)
                        guess = (best_guess(f, c, x, y)
                                 if state(p, key) not in "#." else None)
                        if guess:
                            vals, absurd, total = guess
                            print("        best guess at %d,%d (checksum "
                                  "ignored, missing bytes zero): %s" % (
                                      x, y, ", ".join(
                                          "%s %.6g" % kv
                                          for kv in vals.items())))
                            print("        %d of %d values in it are NaN, "
                                  "inf or beyond +-1e4" % (absurd, total))
                        if a.repair and c.end <= file_size:
                            t = time.time()
                            hit = find_bit_flip(f, c)
                            print("        repair: %s (%.1f s)" % (
                                "flipping bit %d of byte %d (of %d) gives the "
                                "whole chunk, checksum matching" % (
                                    hit[1], hit[0], c.size)
                                if hit else "no single bit flip fixes it",
                                time.time() - t))
                if p.tiled:
                    lx, ly, tx, ty = key
                    near = []
                    for dx, dy, side in ((-1, 0, "left"), (1, 0, "right"),
                                         (0, -1, "above"), (0, 1, "below")):
                        cs = found.get((p.index, (lx, ly, tx + dx, ty + dy)))
                        if cs and cs[0].size >= 0:
                            near.append("%s %d" % (side, cs[0].size))
                    print("    neighbours' data sizes: " + ", ".join(near))


if __name__ == "__main__":
    main()
