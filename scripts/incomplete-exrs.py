#!/usr/bin/env python3
"""Make incomplete EXRs from a complete one, for testing how rega copes.

Two kinds of incomplete, as they turn up in practice:

  cut-NN.exr        an interrupted copy: header and offset table intact,
                    file ends NN% of the way through the pixel data, so the
                    table points past the end of the file
  rendering-NN.exr  a render still being written: OpenEXR writes a zeroed
                    offset table first and fills it in on close, so the
                    table is all zeros and only the first NN% of chunks exist

plus the edges: cut-header.exr (ends mid-header), no-chunks.exr (table
complete, no pixel data), rendering-0.exr (header and zeroed table only).

It also writes a sequence the way a farm leaves one mid-render: the first
frames complete, the next ones still rendering, the last only just opened.

    python3 scripts/incomplete-exrs.py [source.exr] [out_dir] [--sequence src_dir] [--frames N]

Standard library only; handles single-part EXRs (scanline or tiled).
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

MAGIC = 20000630
MULTIPART = 0x1000


def layout(data):
    """(offset table start, chunk offsets in table order) of a single-part EXR."""
    magic, version = struct.unpack_from("<ii", data, 0)
    if magic != MAGIC:
        sys.exit("not an EXR")
    if version & MULTIPART:
        sys.exit("multi-part EXRs aren't handled")
    pos = 8
    while data[pos] != 0:  # attributes: name\0 type\0 int32 size, value
        pos = data.index(b"\0", pos) + 1
        pos = data.index(b"\0", pos) + 1
        (size,) = struct.unpack_from("<i", data, pos)
        pos += 4 + size
    table = pos + 1
    # The table ends where the first chunk begins.
    offsets, pos, first = [], table, len(data)
    while pos < first:
        (off,) = struct.unpack_from("<Q", data, pos)
        offsets.append(off)
        first = min(first, off)
        pos += 8
    return table, offsets


def write(path, data):
    path.write_bytes(data)
    print(f"{path}  {len(data)} bytes")


def rendering(data, table, offsets, fraction):
    """The file as a writer leaves it with `fraction` of its chunks done."""
    ordered = sorted(offsets) + [len(data)]
    done = round(fraction * len(offsets))
    zeroed = bytes(8 * len(offsets))
    return data[:table] + zeroed + data[ordered[0] : ordered[done]]


def main():
    parser = argparse.ArgumentParser(description="Make incomplete EXRs from a complete one.")
    parser.add_argument("source", nargs="?", default="demos/rega-sequence/shot.1001.exr")
    parser.add_argument("out_dir", nargs="?", default="demos/incomplete")
    parser.add_argument("--sequence", default="demos/rega-sequence", help="sequence to copy mid-render")
    parser.add_argument("--frames", type=int, default=10, help="frames of it to take")
    args = parser.parse_args()

    data = Path(args.source).read_bytes()
    table, offsets = layout(data)
    start = min(offsets)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    print(f"{args.source}: header {table} bytes, {len(offsets)} chunks from byte {start}")

    write(out / "cut-header.exr", data[: table // 2])
    write(out / "no-chunks.exr", data[:start])
    for pct in (25, 50, 75, 99):
        write(out / f"cut-{pct}.exr", data[: start + (len(data) - start) * pct // 100])
    for pct in (0, 10, 50, 90):
        write(out / f"rendering-{pct}.exr", rendering(data, table, offsets, pct / 100))

    frames = sorted(Path(args.sequence).glob("*.exr"))[: args.frames]
    if not frames:
        return
    # Complete, then rendering ever less far along, then only just opened.
    seq = out / "sequence"
    seq.mkdir(exist_ok=True)
    tail = [0.9, 0.6, 0.3, 0.0]
    complete = max(len(frames) - len(tail), 1)
    for i, frame in enumerate(frames):
        if i < complete:
            shutil.copyfile(frame, seq / frame.name)
            print(f"{seq / frame.name}  complete")
            continue
        fdata = frame.read_bytes()
        ftable, foffsets = layout(fdata)
        fraction = tail[min(i - complete, len(tail) - 1)]
        write(seq / frame.name, rendering(fdata, ftable, foffsets, fraction))


if __name__ == "__main__":
    main()
