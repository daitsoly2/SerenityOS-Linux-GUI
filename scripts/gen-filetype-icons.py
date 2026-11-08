#!/usr/bin/env python3
"""Minimal PNG read/write + pixel-font badge painter for Serenity filetype icons.

No third-party dependencies: PNG is decoded/encoded with zlib + struct so this
runs on a bare Python 3 (the VM has no Pillow, and the host build环境 should not
need one either).
"""

import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# PNG I/O
# ---------------------------------------------------------------------------


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos = 8
    idat = b""
    width = height = depth = color_type = None
    palette = None
    trns = None
    while pos < len(data):
        length, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert interlace == 0, "interlaced PNG not supported"
        elif ctype == b"PLTE":
            palette = body
        elif ctype == b"tRNS":
            trns = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    assert depth == 8, f"bit depth {depth} unsupported"

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        f = raw[p]
        p += 1
        line = bytearray(raw[p : p + stride])
        p += stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upleft = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        out[y * stride : (y + 1) * stride] = line
        prev = line

    pixels = []
    for y in range(height):
        row = []
        for x in range(width):
            i = y * stride + x * channels
            if color_type == 6:
                row.append(tuple(out[i : i + 4]))
            elif color_type == 2:
                row.append((out[i], out[i + 1], out[i + 2], 255))
            elif color_type == 0:
                row.append((out[i], out[i], out[i], 255))
            elif color_type == 4:
                row.append((out[i], out[i], out[i], out[i + 1]))
            elif color_type == 3:
                idx = out[i]
                r, g, b = palette[idx * 3 : idx * 3 + 3]
                a = trns[idx] if trns and idx < len(trns) else 255
                row.append((r, g, b, a))
        pixels.append(row)
    return pixels


def write_png(path, pixels):
    height = len(pixels)
    width = len(pixels[0])
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for px in row:
            raw.extend(px)

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)


# ---------------------------------------------------------------------------
# 5x7 pixel font (uppercase + digits), one string per glyph row.
# ---------------------------------------------------------------------------

FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01110"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("111", "010", "010", "010", "010", "010", "111"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("001", "011", "101", "001", "001", "001", "111"),
    "2": ("01110", "10001", "00001", "00110", "01000", "10000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("10010", "10010", "10010", "11111", "00010", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    "*": ("000", "101", "010", "101", "000", "000", "000"),
    ".": ("00", "00", "00", "00", "00", "00", "11"),
}


def draw_text(pixels, x0, y0, text, color):
    cursor = x0
    for ch in text:
        glyph = FONT.get(ch)
        if glyph is None:
            cursor += 4
            continue
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit == "1":
                    y, x = y0 + gy, cursor + gx
                    if 0 <= y < len(pixels) and 0 <= x < len(pixels[0]):
                        pixels[y][x] = color
        cursor += len(glyph[0]) + 1


def text_width(text):
    total = 0
    for ch in text:
        glyph = FONT.get(ch)
        total += (len(glyph[0]) if glyph else 3) + 1
    return max(0, total - 1)


def fill(pixels, x0, y0, x1, y1, color):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= y < len(pixels) and 0 <= x < len(pixels[0]):
                pixels[y][x] = color


# ---------------------------------------------------------------------------
# Badge composition
# ---------------------------------------------------------------------------

PAGE = "/mnt/user/serenity-linuxgui/Base/res/icons/32x32/filetype-unknown.png"
PAGE16 = "/mnt/user/serenity-linuxgui/Base/res/icons/16x16/filetype-unknown.png"
DARK = (0, 0, 0, 255)


def content_bbox(pixels):
    h, w = len(pixels), len(pixels[0])
    xs = [x for y in range(h) for x in range(w) if pixels[y][x][3] > 0]
    ys = [y for y in range(h) for x in range(w) if pixels[y][x][3] > 0]
    return min(xs), min(ys), max(xs), max(ys)


def make_badge_icon(label, fill_color, out_32, out_16, text_color=(255, 255, 255, 255)):
    """White page + coloured label plate, matching Serenity's filetype-asm look.

    The plate is centred on the page and allowed to spill exactly one pixel past
    either edge - the same trick filetype-asm/filetype-pdf use, so a 3-letter
    label still reads at 32x32 without shrinking the glyphs.
    """
    page = read_png(PAGE)
    px0, py0, px1, py1 = content_bbox(page)
    page_w = px1 - px0 + 1

    plate_h = 11
    plate_w = max(page_w, text_width(label) + 6)
    x0 = px0 - (plate_w - page_w) // 2
    y1 = py1
    y0 = y1 - plate_h + 1

    # 1px dark outline, coloured plate, 1px light bevel on the top edge.
    fill(page, x0, y0, x0 + plate_w - 1, y1, DARK)
    light = tuple(min(255, c + 60) for c in fill_color[:3]) + (255,)
    fill(page, x0 + 1, y0 + 1, x0 + plate_w - 2, y1 - 1, fill_color)
    fill(page, x0 + 1, y0 + 1, x0 + plate_w - 2, y0 + 1, light)

    tx = x0 + 1 + max(0, (plate_w - 2 - text_width(label)) // 2)
    draw_text(page, tx, y0 + 2, label, text_color)
    write_png(out_32, page)

    # 16x16: no room for text - a coloured bar across the foot of the page.
    small = read_png(PAGE16)
    sx0, sy0, sx1, sy1 = content_bbox(small)
    fill(small, sx0, sy1 - 3, sx1, sy1, DARK)
    fill(small, sx0 + 1, sy1 - 2, sx1 - 1, sy1 - 1, fill_color)
    fill(small, sx0 + 1, sy1 - 2, sx1 - 1, sy1 - 2, light)
    write_png(out_16, small)


ICONS = [
    # (label, plate colour, icon name)
    ("ZIG", (247, 164, 29, 255), "zig"),
    ("RS", (183, 65, 14, 255), "rust"),
    ("GO", (0, 173, 216, 255), "go"),
    ("CFG", (99, 110, 130, 255), "config"),
    ("ISO", (60, 90, 160, 255), "diskimage"),
    ("PKG", (190, 40, 60, 255), "package"),
    ("DOC", (40, 90, 175, 255), "document"),
    ("LOG", (95, 95, 95, 255), "log"),
    ("LUA", (30, 40, 120, 255), "lua"),
    ("KEY", (150, 115, 20, 255), "certificate"),
    ("NIX", (82, 124, 180, 255), "nix"),
    ("3D", (120, 80, 180, 255), "model3d"),
]

if __name__ == "__main__":
    root = "/mnt/user/serenity-linuxgui/Base/res/icons"
    for label, color, name in ICONS:
        make_badge_icon(
            label,
            color,
            f"{root}/32x32/filetype-{name}.png",
            f"{root}/16x16/filetype-{name}.png",
        )
        print("wrote", name)
