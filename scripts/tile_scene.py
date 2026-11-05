#!/usr/bin/env python3
"""Synthetic stress scene: N copies of a binary PLY splat scene side by side on a grid.

For capacity and correctness tests of scene sizes no public scene reaches (ADR-0016): the copies
are identical splats shifted along the scene's two horizontal axes (x and z; RDF and RUB both have
y vertical), spaced by the robust extent (1st-99th percentile of the centres) times --spacing, so
the tiles do not overlap. Every other property is copied unchanged. The output is not a real
capture - never use it for image-quality claims or publish renders of it as a scene.

Usage: python scripts/tile_scene.py in.ply out.ply --copies 3 [--spacing 1.1]
       then: splatconv pack out.ply out.pspk --sh 0
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np

PLY_TYPES = {"float": "f4", "float32": "f4", "double": "f8", "uchar": "u1", "uint8": "u1", "int": "i4",
             "int32": "i4", "uint": "u4", "uint32": "u4", "short": "i2", "ushort": "u2", "char": "i1"}


def read_header(f) -> tuple[list[str], int, np.dtype]:
    if f.readline().strip() != b"ply":
        raise ValueError("not a PLY file")
    names, fields, count, in_vertex = [], [], 0, False
    while True:
        line = f.readline()
        if not line:
            raise ValueError("PLY header ends early")
        words = line.decode("ascii").split()
        if not words:
            continue
        if words[0] == "format" and words[1] != "binary_little_endian":
            raise ValueError("only binary_little_endian PLY is supported")
        elif words[0] == "element":
            in_vertex = words[1] == "vertex"
            if in_vertex:
                count = int(words[2])
            elif count:
                raise ValueError("elements after 'vertex' are not supported")
        elif words[0] == "property" and in_vertex:
            if words[1] == "list":
                raise ValueError("list properties are not supported")
            fields.append((words[2], "<" + PLY_TYPES[words[1]]))
            names.append(words[2])
        elif words[0] == "end_header":
            return names, count, np.dtype(fields)


def write_header(f, count: int, dtype: np.dtype) -> None:
    inverse = {"f4": "float", "f8": "double", "u1": "uchar", "i4": "int", "u4": "uint", "i2": "short",
               "u2": "ushort", "i1": "char"}
    lines = ["ply", "format binary_little_endian 1.0", f"element vertex {count}"]
    lines += [f"property {inverse[dtype[name].str[1:]]} {name}" for name in dtype.names]
    lines.append("end_header")
    f.write(("\n".join(lines) + "\n").encode("ascii"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("src", type=Path)
    parser.add_argument("dst", type=Path)
    parser.add_argument("--copies", type=int, required=True, help="number of tiles (>= 2)")
    parser.add_argument("--spacing", type=float, default=1.1, help="tile pitch / robust extent")
    args = parser.parse_args()
    if args.copies < 2:
        parser.error("--copies must be at least 2")

    with args.src.open("rb") as f:
        _, count, dtype = read_header(f)
        data = np.fromfile(f, dtype=dtype, count=count)
    if len(data) != count:
        print(f"ERROR: {args.src} holds {len(data)} of {count} vertices", file=sys.stderr)
        return 1
    for axis in ("x", "y", "z"):
        if axis not in dtype.names:
            print(f"ERROR: no '{axis}' property", file=sys.stderr)
            return 1

    lo = np.array([np.percentile(data[a], 1) for a in ("x", "z")])
    hi = np.array([np.percentile(data[a], 99) for a in ("x", "z")])
    pitch = (hi - lo) * args.spacing
    columns = math.ceil(math.sqrt(args.copies))
    total = count * args.copies
    if total >= 2**31:
        print("ERROR: more than 2^31 splats", file=sys.stderr)
        return 1
    with args.dst.open("wb") as f:
        write_header(f, total, dtype)
        for t in range(args.copies):
            tile = data.copy()
            tile["x"] += pitch[0] * (t % columns)
            tile["z"] += pitch[1] * (t // columns)
            tile.tofile(f)
    print(f"{args.dst}: {args.copies} x {count:,} = {total:,} splats, grid {columns} wide, "
          f"pitch x {pitch[0]:.3f} z {pitch[1]:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
