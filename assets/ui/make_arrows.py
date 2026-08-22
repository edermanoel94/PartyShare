"""Generates the chevron PNGs the stylesheet points at.

Kept out of the build: they are a handful of bytes each and regenerating them
from a script nobody runs is worse than a file in the repository. This is here
so the shapes can be changed without drawing them by hand.
"""

import pathlib
import struct
import zlib

# One grey for both colour schemes. It sits between the muted text of the light
# palette (#6C7089) and of the dark one (#999DB4), which is close enough to
# both that a second file per scheme would buy nothing a stylesheet could not
# already do without one.
INK = (0x84, 0x89, 0xA0)


def png(path, width, height, alpha):
    """Writes an 8-bit greyscale-plus-alpha PNG of one colour."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        for x in range(width):
            raw += bytes(INK) + bytes([alpha(x, y)])

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 6 = RGBA
    blob = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    pathlib.Path(path).write_bytes(blob)
    print("wrote", path, f"{width}x{height}")


def chevron(scale, up):
    """A two-stroke chevron, anti-aliased by supersampling.

    Drawn as the distance to the two line segments rather than as a polygon:
    at ten pixels across, a filled triangle reads as a blob and a stroke reads
    as an arrow.
    """
    width, height = 10 * scale, 6 * scale
    thickness = 1.6 * scale
    over = 4  # supersampling per axis

    def segment_distance(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        length = dx * dx + dy * dy
        t = 0.0 if length == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
        cx, cy = ax + t * dx, ay + t * dy
        return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5

    inset = thickness / 2 + 0.2
    left = (inset, inset if not up else height - inset)
    mid = (width / 2, height - inset if not up else inset)
    right = (width - inset, inset if not up else height - inset)

    def alpha(x, y):
        hits = 0
        for sy in range(over):
            for sx in range(over):
                px = x + (sx + 0.5) / over
                py = y + (sy + 0.5) / over
                near = min(
                    segment_distance(px, py, left[0], left[1], mid[0], mid[1]),
                    segment_distance(px, py, mid[0], mid[1], right[0], right[1]),
                )
                if near <= thickness / 2:
                    hits += 1
        return round(255 * hits / (over * over))

    return width, height, alpha


here = pathlib.Path("assets/ui")
here.mkdir(parents=True, exist_ok=True)

for name, up in (("chevron-down", False), ("chevron-up", True)):
    for scale, suffix in ((1, ""), (2, "@2x"), (3, "@3x")):
        width, height, alpha = chevron(scale, up)
        png(here / f"{name}{suffix}.png", width, height, alpha)
