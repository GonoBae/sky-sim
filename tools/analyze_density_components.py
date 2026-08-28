#!/usr/bin/env python3
"""Measure connected cloud regions in a raw little-endian UNorm16 density grid."""

from __future__ import annotations

import argparse
import array
from collections import deque
from pathlib import Path
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--shape", type=int, nargs=3, default=(64, 64, 64))
    return parser.parse_args()


def describe(values: array.array, shape: tuple[int, int, int], fraction: float) -> None:
    nx, ny, nz = shape
    threshold = max(values) * fraction
    active = bytearray(value > threshold for value in values)
    visited = bytearray(len(values))
    components: list[tuple[int, tuple[int, int, int], tuple[int, int, int]]] = []

    def index(x: int, y: int, z: int) -> int:
        return x + nx * (y + ny * z)

    for start, is_active in enumerate(active):
        if not is_active or visited[start]:
            continue
        visited[start] = 1
        queue = deque([start])
        size = 0
        minimum = [nx, ny, nz]
        maximum = [0, 0, 0]
        while queue:
            current = queue.popleft()
            z, remainder = divmod(current, nx * ny)
            y, x = divmod(remainder, nx)
            size += 1
            minimum[0] = min(minimum[0], x)
            minimum[1] = min(minimum[1], y)
            minimum[2] = min(minimum[2], z)
            maximum[0] = max(maximum[0], x)
            maximum[1] = max(maximum[1], y)
            maximum[2] = max(maximum[2], z)
            for dx, dy, dz in ((-1, 0, 0), (1, 0, 0), (0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1)):
                xx, yy, zz = x + dx, y + dy, z + dz
                if not (0 <= xx < nx and 0 <= yy < ny and 0 <= zz < nz):
                    continue
                neighbor = index(xx, yy, zz)
                if active[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    queue.append(neighbor)
        components.append((size, tuple(minimum), tuple(maximum)))

    components.sort(reverse=True)
    active_count = sum(active)
    covered_columns = sum(
        any(active[index(x, y, z)] for z in range(nz))
        for y in range(ny)
        for x in range(nx)
    )
    print(
        f"threshold={fraction:.3f}*max ({threshold:.1f}/65535) "
        f"active={active_count} ({100.0 * active_count / len(values):.3f}%) "
        f"projected_xy={covered_columns}/{nx * ny} "
        f"({100.0 * covered_columns / (nx * ny):.1f}%) "
        f"components={len(components)}"
    )
    for rank, (size, minimum, maximum) in enumerate(components[:8], 1):
        print(f"  #{rank}: voxels={size} bbox={minimum}..{maximum}")


def main() -> int:
    args = parse_args()
    shape = tuple(args.shape)
    expected = shape[0] * shape[1] * shape[2]
    values = array.array("H")
    values.frombytes(args.path.read_bytes())
    if sys.byteorder != "little":
        values.byteswap()
    if len(values) != expected:
        raise ValueError(f"expected {expected} voxels, got {len(values)}")
    print(f"shape={shape} min={min(values)} max={max(values)}")
    for fraction in (0.0, 0.01, 0.05, 0.10, 0.25):
        describe(values, shape, fraction)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
