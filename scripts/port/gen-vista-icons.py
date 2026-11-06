#!/usr/bin/env python3
"""Generate a Vista/Aero-style icon set (32x32 -> 16x16) with no dependencies.

Why not Pillow/SVG: the host has neither Pillow nor a usable SVG rasteriser
(ImageMagick's MSVG drops gradients and emits grayscale). Raw pixel maths keeps
the result deterministic and lets us regenerate the set anywhere.

Output: Base/res/icons/vista/16x16/<name>.png
The sibling C++ data blob is produced by scripts/port/embed-desktop-icons.py.
"""

import math
import os
import struct
import subprocess
import sys

SIZE = 32

# Aero palette
BLUE_TOP = (168, 208, 247)
BLUE_MID = (74, 139, 220)
BLUE_BOT = (27, 79, 156)
BLUE_EDGE = (18, 63, 140)
WHITE = (255, 255, 255)
GREY_TOP = (245, 246, 248)
GREY_BOT = (188, 192, 198)
GREY_EDGE = (108, 116, 128)
YELLOW_TOP = (255, 226, 130)
YELLOW_BOT = (226, 168, 40)
YELLOW_EDGE = (150, 104, 20)
GREEN_TOP = (150, 226, 130)
GREEN_BOT = (56, 150, 60)
GREEN_EDGE = (28, 96, 36)
RED = (206, 60, 52)


def new_canvas():
    return [[(0, 0, 0, 0) for _ in range(SIZE)] for _ in range(SIZE)]


def mix(dst, src):
    sr, sg, sb, sa = src
    if sa == 0:
        return dst
    da = dst[3]
    if sa == 255 or da == 0:
        return (sr, sg, sb, sa) if sa == 255 else (sr, sg, sb, sa)
    oa = sa + da * (255 - sa) // 255
    if oa == 0:
        return (0, 0, 0, 0)
    inv = da * (255 - sa) // 255
    return (
        (sr * sa + dst[0] * inv) // oa,
        (sg * sa + dst[1] * inv) // oa,
        (sb * sa + dst[2] * inv) // oa,
        oa,
    )


def as_rgba(color):
    return color if len(color) == 4 else color + (255,)


def put(buf, x, y, color):
    if 0 <= x < SIZE and 0 <= y < SIZE:
        buf[y][x] = mix(buf[y][x], as_rgba(color))


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def rounded_rect(buf, x0, y0, x1, y1, radius, top, bottom, edge=None, edge_w=1.0):
    """Vertical gradient rounded rect with optional 1px edge."""
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, (y1 - y0))
        if t < 0.5:
            col = lerp(top, lerp(top, bottom, 0.5), t * 2)
        else:
            col = lerp(lerp(top, bottom, 0.5), bottom, (t - 0.5) * 2)
        for x in range(x0, x1 + 1):
            # corner test
            cx = min(max(x, x0 + radius), x1 - radius)
            cy = min(max(y, y0 + radius), y1 - radius)
            d = math.hypot(x - cx, y - cy)
            if d > radius + 0.5:
                continue
            if edge is not None and d > radius - edge_w:
                put(buf, x, y, edge)
            elif edge is not None and (x <= x0 + edge_w or x >= x1 - edge_w or y <= y0 + edge_w or y >= y1 - edge_w):
                put(buf, x, y, edge)
            else:
                put(buf, x, y, col)


def circle(buf, cx, cy, r, color):
    for y in range(int(cy - r) - 1, int(cy + r) + 2):
        for x in range(int(cx - r) - 1, int(cx + r) + 2):
            d = math.hypot(x - cx, y - cy)
            if d <= r:
                # cheap 1px antialias
                a = 255 if d <= r - 0.7 else int(255 * (r - d + 0.7) / 1.4)
                if a > 0:
                    sr, sg, sb, sa = as_rgba(color)
                    put(buf, x, y, (sr, sg, sb, max(0, min(255, sa * a // 255))))


def ring(buf, cx, cy, r, w, color):
    for y in range(int(cy - r - w) - 1, int(cy + r + w) + 2):
        for x in range(int(cx - r - w) - 1, int(cx + r + w) + 2):
            d = abs(math.hypot(x - cx, y - cy) - r)
            if d <= w / 2:
                a = 255 if d <= w / 2 - 0.7 else int(255 * (w / 2 - d + 0.7) / 1.4)
                if a > 0:
                    sr, sg, sb, sa = as_rgba(color)
                    put(buf, x, y, (sr, sg, sb, max(0, min(255, sa * a // 255))))


def poly(buf, pts, color):
    if len(pts) < 3:
        return
    ys = [p[1] for p in pts]
    for y in range(int(min(ys)), int(max(ys)) + 1):
        xs = []
        for i in range(len(pts)):
            x1, y1 = pts[i]
            x2, y2 = pts[(i + 1) % len(pts)]
            if y1 == y2:
                continue
            if min(y1, y2) <= y < max(y1, y2):
                xs.append(x1 + (y - y1) * (x2 - x1) / (y2 - y1))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(int(math.floor(xs[i] + 0.5)), int(math.ceil(xs[i + 1] - 0.5)) + 1):
                put(buf, x, y, color)


def line(buf, x0, y0, x1, y1, w, color):
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(steps + 1):
        t = i / steps
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        circle(buf, x, y, w / 2, color)


def gloss(buf, x0, y0, x1, y1, radius):
    """Top highlight band, clipped to a rounded rect."""
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, (y1 - y0))
        a = int(90 * (1 - t))
        if a <= 0:
            continue
        for x in range(x0, x1 + 1):
            cx = min(max(x, x0 + radius), x1 - radius)
            cy = min(max(y, y0 + radius), y1 - radius)
            if math.hypot(x - cx, y - cy) <= radius:
                put(buf, x, y, (255, 255, 255, a))


# --------------------------------------------------------------------------
# icon builders
# --------------------------------------------------------------------------

def icon_disc_arrow(buf, direction):
    circle(buf, 16, 16, 13.5, BLUE_MID)
    ring(buf, 16, 16, 13.5, 1.6, BLUE_EDGE)
    circle(buf, 16, 11.5, 9, (255, 255, 255, 40))
    w = (255, 255, 255)
    if direction == 'back':
        poly(buf, [(18.5, 8), (10, 16), (18.5, 24), (18.5, 19.5), (23, 19.5), (23, 12.5), (18.5, 12.5)], w)
    elif direction == 'forward':
        poly(buf, [(13.5, 8), (22, 16), (13.5, 24), (13.5, 19.5), (9, 19.5), (9, 12.5), (13.5, 12.5)], w)
    else:
        poly(buf, [(8, 17.5), (16, 9.5), (24, 17.5), (19.5, 17.5), (19.5, 23), (12.5, 23), (12.5, 17.5)], w)


def icon_reload(buf):
    circle(buf, 16, 16, 13.5, BLUE_MID)
    ring(buf, 16, 16, 13.5, 1.6, BLUE_EDGE)
    circle(buf, 16, 11.5, 9, (255, 255, 255, 40))
    # 3/4 ring
    for y in range(0, SIZE):
        for x in range(0, SIZE):
            d = math.hypot(x - 16, y - 16)
            if 7.2 <= d <= 10.2:
                ang = math.degrees(math.atan2(y - 16, x - 16)) % 360
                if not (20 <= ang <= 80):
                    put(buf, x, y, (255, 255, 255, 255))
    poly(buf, [(21, 5), (27, 12), (19, 13)], (255, 255, 255))


def icon_find(buf):
    circle(buf, 16, 14, 13.5, BLUE_MID)
    ring(buf, 16, 14, 13.5, 1.6, BLUE_EDGE)
    circle(buf, 16, 10, 9, (255, 255, 255, 40))
    ring(buf, 14, 13, 6.5, 3.0, (255, 255, 255))
    ring(buf, 14, 13, 6.5, 1.2, (60, 90, 140))
    line(buf, 19, 18, 25, 25, 3.4, (235, 240, 250))
    line(buf, 19, 18, 25, 25, 1.6, (40, 70, 120))


def icon_help(buf):
    circle(buf, 16, 16, 13.5, BLUE_MID)
    ring(buf, 16, 16, 13.5, 1.6, BLUE_EDGE)
    circle(buf, 16, 11.5, 9, (255, 255, 255, 40))
    ring(buf, 16, 12, 5.6, 3.0, (255, 255, 255))
    poly(buf, [(16, 15), (18.6, 15), (18.6, 19), (16, 19)], (255, 255, 255))
    circle(buf, 17.3, 23, 2.0, (255, 255, 255))


def icon_properties(buf):
    rounded_rect(buf, 5, 3, 22, 29, 2.5, WHITE, (222, 228, 236), GREY_EDGE)
    rounded_rect(buf, 5, 3, 22, 8, 2.5, (250, 251, 253), (235, 239, 246), GREY_EDGE)
    for i, y in enumerate((12, 16, 20)):
        line(buf, 8, y, 19, y, 1.2, (150, 162, 178))
    # gear
    circle(buf, 23, 23, 7.2, (120, 132, 148))
    for k in range(8):
        a = k * math.pi / 4
        poly(buf, [
            (23 + math.cos(a - 0.18) * 6.2, 23 + math.sin(a - 0.18) * 6.2),
            (23 + math.cos(a - 0.12) * 9.4, 23 + math.sin(a - 0.12) * 9.4),
            (23 + math.cos(a + 0.12) * 9.4, 23 + math.sin(a + 0.12) * 9.4),
            (23 + math.cos(a + 0.18) * 6.2, 23 + math.sin(a + 0.18) * 6.2),
        ], (140, 150, 165))
    circle(buf, 23, 23, 7.0, (150, 160, 175))
    circle(buf, 23, 23, 3.4, (250, 251, 253))


def icon_archive(buf):
    rounded_rect(buf, 4, 7, 28, 28, 3, BLUE_TOP, BLUE_BOT, BLUE_EDGE)
    gloss(buf, 4, 7, 28, 16, 3)
    # zipper
    line(buf, 16, 8, 16, 27, 3.2, (250, 250, 252))
    for y in range(9, 27, 3):
        line(buf, 13.6, y, 18.4, y, 1.1, (120, 132, 150))
    rounded_rect(buf, 13, 3, 19, 8, 1.5, (245, 246, 249), (200, 206, 214), GREY_EDGE)


def icon_folder(buf, extra=None):
    poly(buf, [(3, 10), (12, 10), (14, 7), (27, 7), (27, 12), (3, 12)], YELLOW_BOT)
    rounded_rect(buf, 3, 10, 29, 27, 2.5, YELLOW_TOP, YELLOW_BOT, YELLOW_EDGE)
    gloss(buf, 3, 10, 29, 18, 2.5)
    if extra == 'plus':
        line(buf, 22, 17, 22, 24, 3.4, (255, 255, 255))
        line(buf, 18.5, 20.5, 25.5, 20.5, 3.4, (255, 255, 255))
    elif extra == 'up':
        circle(buf, 21.5, 20.5, 6.0, (46, 108, 190))
        poly(buf, [(17.2, 21.4), (21.5, 16.8), (25.8, 21.4), (23.4, 21.4), (23.4, 24.4), (19.6, 24.4), (19.6, 21.4)], WHITE)


def icon_new_file(buf):
    poly(buf, [(7, 4), (21, 4), (26, 9), (26, 28), (7, 28)], (252, 253, 255))
    poly(buf, [(21, 4), (26, 9), (21, 9)], (206, 214, 226))
    rounded_rect(buf, 7, 4, 26, 28, 1.5, (255, 255, 255), (232, 237, 244), GREY_EDGE)
    for y in (14, 18, 22):
        line(buf, 10, y, 23, y, 1.2, (158, 170, 186))
    circle(buf, 23, 23, 6.4, GREEN_BOT)
    ring(buf, 23, 23, 6.4, 1.2, GREEN_EDGE)
    line(buf, 23, 19.5, 23, 26.5, 3.0, WHITE)
    line(buf, 19.5, 23, 26.5, 23, 3.0, WHITE)


def icon_new_window(buf):
    rounded_rect(buf, 3, 9, 22, 27, 2, (250, 251, 253), (206, 213, 224), GREY_EDGE)
    rounded_rect(buf, 3, 9, 22, 14, 2, (86, 140, 214), (44, 92, 168), (28, 64, 122))
    rounded_rect(buf, 11, 5, 29, 22, 2, (255, 255, 255), (226, 232, 242), GREY_EDGE)
    rounded_rect(buf, 11, 5, 29, 10, 2, (126, 176, 236), (58, 112, 190), (28, 64, 122))
    circle(buf, 14.2, 7.5, 1.1, (255, 255, 255, 210))


def icon_image_viewer(buf):
    rounded_rect(buf, 3, 6, 29, 26, 2.5, (250, 251, 253), (208, 215, 226), GREY_EDGE)
    rounded_rect(buf, 5, 8, 27, 22, 1.5, (222, 240, 255), (168, 206, 240), (150, 176, 206))
    circle(buf, 10, 12, 2.6, (255, 226, 120))
    poly(buf, [(5, 22), (13, 13), (19, 19), (23, 15), (27, 22)], (96, 172, 96))


def icon_terminal(buf):
    rounded_rect(buf, 3, 5, 29, 27, 3, (46, 52, 60), (12, 14, 18), (0, 0, 0))
    rounded_rect(buf, 3, 5, 29, 10, 3, (96, 104, 116), (54, 60, 70), (0, 0, 0))
    for i, x in enumerate((7, 11, 15)):
        circle(buf, x, 7.5, 1.2, (255, 255, 255, 200))
    poly(buf, [(8, 14), (13, 18), (8, 22), (8, 19.4), (10.6, 18), (8, 16.6)], (120, 226, 130))
    line(buf, 15, 22, 23, 22, 2.0, (240, 244, 250))


def icon_desktop(buf):
    rounded_rect(buf, 3, 6, 29, 23, 2.5, (232, 238, 246), (186, 196, 210), GREY_EDGE)
    rounded_rect(buf, 5, 8, 27, 20, 1.5, (150, 200, 245), (60, 120, 190), (40, 86, 150))
    poly(buf, [(13, 23), (19, 23), (20, 27), (12, 27)], (150, 158, 170))
    rounded_rect(buf, 9, 26, 23, 28, 1, (170, 178, 190), (120, 128, 140), GREY_EDGE)


def icon_display_settings(buf):
    rounded_rect(buf, 2, 5, 24, 21, 2.5, (232, 238, 246), (186, 196, 210), GREY_EDGE)
    rounded_rect(buf, 4, 7, 22, 18, 1.5, (150, 200, 245), (60, 120, 190), (40, 86, 150))
    poly(buf, [(10, 21), (16, 21), (17, 25), (9, 25)], (150, 158, 170))
    circle(buf, 24, 23, 7.0, (140, 150, 165))
    for k in range(8):
        a = k * math.pi / 4
        poly(buf, [
            (24 + math.cos(a - 0.18) * 6.0, 23 + math.sin(a - 0.18) * 6.0),
            (24 + math.cos(a - 0.12) * 9.6, 23 + math.sin(a - 0.12) * 9.6),
            (24 + math.cos(a + 0.12) * 9.6, 23 + math.sin(a + 0.12) * 9.6),
            (24 + math.cos(a + 0.18) * 6.0, 23 + math.sin(a + 0.18) * 6.0),
        ], (150, 160, 175))
    circle(buf, 24, 23, 2.8, (250, 251, 253))


def icon_file_manager(buf):
    icon_folder(buf)
    rounded_rect(buf, 14, 14, 29, 27, 2, (255, 255, 255), (220, 227, 238), GREY_EDGE)
    rounded_rect(buf, 14, 14, 29, 18, 2, (126, 176, 236), (58, 112, 190), (28, 64, 122))
    line(buf, 17, 22, 26, 22, 1.2, (150, 162, 178))


def icon_go_to(buf):
    rounded_rect(buf, 3, 6, 29, 26, 2.5, (250, 251, 253), (214, 221, 232), GREY_EDGE)
    rounded_rect(buf, 5, 8, 27, 13, 1.5, (255, 255, 255), (238, 242, 249), (170, 180, 196))
    poly(buf, [(7, 22), (15, 16), (23, 22), (8.2, 22)], (70, 130, 205))
    poly(buf, [(18, 21.5), (25.5, 21.5), (25.5, 18.5), (29.5, 23.5), (25.5, 28.5), (25.5, 25.5), (18, 25.5)], (46, 108, 190))


def icon_open_parent(buf):
    icon_folder(buf, 'up')


ICONS = {
    'go-back': lambda b: icon_disc_arrow(b, 'back'),
    'go-forward': lambda b: icon_disc_arrow(b, 'forward'),
    'go-up': lambda b: icon_disc_arrow(b, 'up'),
    'reload': icon_reload,
    'find': icon_find,
    'app-help': icon_help,
    'properties': icon_properties,
    'filetype-archive': icon_archive,
    'mkdir': lambda b: icon_folder(b, 'plus'),
    'new': icon_new_file,
    'new-window': icon_new_window,
    'app-image-viewer': icon_image_viewer,
    'app-terminal': icon_terminal,
    'desktop': icon_desktop,
    'app-display-settings': icon_display_settings,
    'app-file-manager': icon_file_manager,
    'go-to': icon_go_to,
    'open-parent-directory': icon_open_parent,
}


def write_pam(path, buf):
    with open(path, 'wb') as f:
        f.write(b'P7\n')
        f.write(b'WIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n' % (SIZE, SIZE))
        for row in buf:
            for px in row:
                f.write(bytes(px))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else 'Base/res/icons/vista/16x16'
    out_dir32 = os.path.join(os.path.dirname(out_dir), '32x32')
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(out_dir32, exist_ok=True)
    tmp_dir = '/tmp/vista-icons'
    os.makedirs(tmp_dir, exist_ok=True)

    for name, builder in ICONS.items():
        buf = new_canvas()
        builder(buf)
        pam = os.path.join(tmp_dir, name + '.pam')
        write_pam(pam, buf)
        png = os.path.join(out_dir, name + '.png')
        subprocess.run(['convert', pam, '-filter', 'Lanczos', '-resize', '16x16', '-depth', '8', png], check=True)
        png32 = os.path.join(out_dir32, name + '.png')
        subprocess.run(['convert', pam, '-depth', '8', png32], check=True)
        print('  ', name, '->', png, '+ 32x32')

    # contact sheet for eyeballing the whole set
    files = sorted(os.path.join(out_dir, n + '.png') for n in ICONS)
    subprocess.run(['montage', *files, '-tile', '6x', '-geometry', '32x32+6+6',
                    '-background', '#e8eef7', os.path.join(tmp_dir, 'sheet.png')], check=True)
    print('sheet:', os.path.join(tmp_dir, 'sheet.png'))


if __name__ == '__main__':
    main()
