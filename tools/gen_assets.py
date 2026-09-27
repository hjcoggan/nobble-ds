#!/usr/bin/env python3
"""Generate the Nobble DS assets:
  data/*.bin            full-colour 256x192 backgrounds (RGB15 with the alpha bit)
  source/assets.c       palettes, sprite tiles and text-layer tiles
  include/assets.h
and build/preview_*.png for a quick look (both screens stacked like a DS).

Run from the repo root:  python3 tools/gen_assets.py
"""
import math
import os
import re
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 256, 192            # one DS screen


def rgb15(r, g, b):
    return (int(r) >> 3) | ((int(g) >> 3) << 5) | ((int(b) >> 3) << 10)


def clamp(v, lo=0, hi=255):
    return lo if v < lo else hi if v > hi else v


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def scale(c, k):
    return tuple(clamp(v * k) for v in c)


# ---------------------------------------------------------------- noise
def hash2(ix, iy, seed):
    h = (ix * 374761393 + iy * 668265263 + seed * 982451653) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def vnoise(x, y, seed):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = hash2(ix, iy, seed)
    b = hash2(ix + 1, iy, seed)
    c = hash2(ix, iy + 1, seed)
    d = hash2(ix + 1, iy + 1, seed)
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy


def fbm(x, y, seed, octaves=3):
    v, amp, tot = 0.0, 1.0, 0.0
    for o in range(octaves):
        v += vnoise(x, y, seed + o * 17) * amp
        tot += amp
        x, y, amp = x * 2.03, y * 2.03, amp * 0.5
    return v / tot



# ---------------------------------------------------------------- canvas helpers
class Canvas:
    def __init__(self, fill=(0, 0, 0)):
        self.px = [[fill] * W for _ in range(H)]

    def get(self, x, y):
        return self.px[y][x]

    def put(self, x, y, c, a=1.0):
        if 0 <= x < W and 0 <= y < H:
            if a >= 1.0:
                self.px[y][x] = c
            else:
                self.px[y][x] = mix(self.px[y][x], c, a)

    def shade(self, x, y, k):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = scale(self.px[y][x], k)

    def each(self, fn, box=None):
        x0, y0, x1, y1 = box or (0, 0, W, H)
        for y in range(max(0, y0), min(H, y1)):
            for x in range(max(0, x0), min(W, x1)):
                r = fn(x, y, self.px[y][x])
                if r is not None:
                    self.px[y][x] = r


def in_poly(x, y, poly):
    inside = False
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        if (y0 > y) != (y1 > y) and x < (x1 - x0) * (y - y0) / (y1 - y0) + x0:
            inside = not inside
    return inside


def fill_poly(cv, poly, colfn):
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    for y in range(int(min(ys)), int(max(ys)) + 1):
        for x in range(int(min(xs)), int(max(xs)) + 1):
            if in_poly(x + 0.5, y + 0.5, poly):
                c = colfn(x, y) if callable(colfn) else colfn
                cv.put(x, y, c)


def disc(cv, cx, cy, r, colfn):
    for y in range(int(cy - r - 1), int(cy + r + 2)):
        for x in range(int(cx - r - 1), int(cx + r + 2)):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if d <= r:
                c = colfn(x, y, d) if callable(colfn) else colfn
                if c is not None:
                    cv.put(x, y, c)


GOLD = (230, 176, 56)
GOLD_LT = (255, 232, 140)
GOLD_DK = (140, 88, 20)
JADE = (40, 160, 130)
JADE_LT = (120, 220, 180)
JADE_DK = (16, 80, 70)
TERRACOTTA = (180, 70, 40)
BONE = (236, 226, 196)



def glyph_mask(text, cell, gap, glyphs):
    cols = []
    for i, ch in enumerate(text):
        g = glyphs[ch]
        for gx in range(len(g[0])):
            cols.append([g[gy][gx] == "#" for gy in range(len(g))])
        if i < len(text) - 1:
            cols += [[False] * len(g)] * gap
    w = len(cols) * cell
    h = len(cols[0]) * cell
    return w, h, lambda x, y: 0 <= x < w and 0 <= y < h and cols[x // cell][y // cell]


def carved_text(cv, text, glyphs, cell, gap, top, face_top, face_bot, edge_lt, edge_dk, outline,
                band=True):
    w, h, m = glyph_mask(text, cell, gap, glyphs)
    x0 = (W - w) // 2
    # drop shadow
    for y in range(h):
        for x in range(w):
            if m(x, y):
                for o in (3, 4):
                    cv.shade(x0 + x + o, top + y + o, 0.45)
    # outline
    for y in range(-2, h + 2):
        for x in range(-2, w + 2):
            if not m(x, y) and any(m(x + dx, y + dy) for dx in (-2, -1, 0, 1, 2) for dy in (-2, -1, 0, 1, 2)):
                cv.put(x0 + x, top + y, outline)
    # face with bevel
    for y in range(h):
        for x in range(w):
            if not m(x, y):
                continue
            t = y / h
            c = mix(face_top, face_bot, t)
            n = 0.94 + 0.12 * fbm((x0 + x) * 0.25, (top + y) * 0.25, 41, 2)
            c = scale(c, n)
            if not m(x - 1, y) or not m(x, y - 1) or not m(x - 2, y - 2):
                c = edge_lt
            elif not m(x + 1, y) or not m(x, y + 1) or not m(x + 2, y + 2):
                c = edge_dk
            elif band and cell * 3 <= y < cell * 3 + 2:
                c = scale(c, 0.7)       # carved stripe
            elif band and y == cell * 3 + 2:
                c = scale(c, 1.15)
            cv.put(x0 + x, top + y, c)


SMALL_GLYPHS = {
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
}





# ---------------------------------------------------------------- tiles and palettes
def to_tiles(img, w, h, bpp=4):
    """Tiles in row-major order, returned as u32 words."""
    words = []
    per = 32 // bpp
    for ty in range(h // 8):
        for tx in range(w // 8):
            for r in range(8):
                for half in range(8 // per):
                    v = 0
                    for c in range(per):
                        v |= (img[ty * 8 + r][tx * 8 + half * per + c] & ((1 << bpp) - 1)) << (bpp * c)
                    words.append(v)
    return words


def pal16(cols):
    cols = list(cols) + [(0, 0, 0)] * (16 - len(cols))
    return [rgb15(*c) for c in cols]


# ---------------------------------------------------------------- font
FONT_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:!->+=.',%/"
GLYPHS = {
    "0": ".###. #...# #..## #.#.# ##..# #...# .###.",
    "1": "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.",
    "2": ".###. #...# ....# ...#. ..#.. .#... #####",
    "3": "##### ...#. ..#.. ...#. ....# #...# .###.",
    "4": "...#. ..##. .#.#. #..#. ##### ...#. ...#.",
    "5": "##### #.... ####. ....# ....# #...# .###.",
    "6": "..##. .#... #.... ####. #...# #...# .###.",
    "7": "##### ....# ...#. ..#.. .#... .#... .#...",
    "8": ".###. #...# #...# .###. #...# #...# .###.",
    "9": ".###. #...# #...# .#### ....# ...#. .##..",
    "A": ".###. #...# #...# ##### #...# #...# #...#",
    "B": "####. #...# #...# ####. #...# #...# ####.",
    "C": ".###. #...# #.... #.... #.... #...# .###.",
    "D": "####. #...# #...# #...# #...# #...# ####.",
    "E": "##### #.... #.... ####. #.... #.... #####",
    "F": "##### #.... #.... ####. #.... #.... #....",
    "G": ".###. #...# #.... #.### #...# #...# .####",
    "H": "#...# #...# #...# ##### #...# #...# #...#",
    "I": ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",
    "J": "..### ...#. ...#. ...#. ...#. #..#. .##..",
    "K": "#...# #..#. #.#.. ##... #.#.. #..#. #...#",
    "L": "#.... #.... #.... #.... #.... #.... #####",
    "M": "#...# ##.## #.#.# #.#.# #...# #...# #...#",
    "N": "#...# #...# ##..# #.#.# #..## #...# #...#",
    "O": ".###. #...# #...# #...# #...# #...# .###.",
    "P": "####. #...# #...# ####. #.... #.... #....",
    "Q": ".###. #...# #...# #...# #.#.# #..#. .##.#",
    "R": "####. #...# #...# ####. #.#.. #..#. #...#",
    "S": ".#### #.... #.... .###. ....# ....# ####.",
    "T": "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",
    "U": "#...# #...# #...# #...# #...# #...# .###.",
    "V": "#...# #...# #...# #...# #...# .#.#. ..#..",
    "W": "#...# #...# #...# #.#.# #.#.# #.#.# .#.#.",
    "X": "#...# #...# .#.#. ..#.. .#.#. #...# #...#",
    "Y": "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..",
    "Z": "##### ....# ...#. ..#.. .#... #.... #####",
    ":": "..... ..#.. ..#.. ..... ..#.. ..#.. .....",
    "!": "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#..",
    "-": "..... ..... ..... .###. ..... ..... .....",
    ">": "#.... .#... ..#.. ...#. ..#.. .#... #....",
    "+": "..... ..#.. ..#.. ##### ..#.. ..#.. .....",
    "=": "..... ..... ##### ..... ##### ..... .....",
    ".": "..... ..... ..... ..... ..... ..... ..#..",
    "'": "..#.. ..#.. .#... ..... ..... ..... .....",
    ",": "..... ..... ..... ..... ..... ..#.. .#...",
    "%": "##..# ##.#. ...#. ..#.. .#... .#.## #..##",
    "/": "....# ...#. ...#. ..#.. .#... .#... #....",
}
# styles: plain, on a panel, highlighted on a panel, gold (no panel), dimmed on a panel, teal
FONT_STYLES = [(1, 2, 0), (1, 2, 3), (6, 2, 3), (6, 2, 0), (8, 2, 3), (9, 2, 0)]   # ... and teal (LCD labels)
font_tiles = []
for fg, sh, bgc in FONT_STYLES:
    for ch in FONT_CHARS:
        img = [[bgc] * 8 for _ in range(8)]
        rows = GLYPHS.get(ch, ". " * 7).split()
        for r, row in enumerate(rows):
            for c, p in enumerate(row):
                if p == "#":
                    if img[r + 1][c + 2] == bgc:
                        img[r + 1][c + 2] = sh
                    img[r][c + 1] = fg
        font_tiles += to_tiles(img, 8, 8)


# panel frame: TL, T, TR, L, R, BL, B, BR
def frame_tile(top, bottom, left, right):
    img = [[3] * 8 for _ in range(8)]
    for y in range(8):
        for x in range(8):
            if (top and y == 0) or (bottom and y == 7) or (left and x == 0) or (right and x == 7):
                img[y][x] = 5
            elif (top and y == 1) or (bottom and y == 6) or (left and x == 1) or (right and x == 6):
                img[y][x] = 4
            elif (top and y == 2) or (left and x == 2):
                img[y][x] = 7
    return to_tiles(img, 8, 8)


for spec in [(1, 0, 1, 0), (1, 0, 0, 0), (1, 0, 0, 1), (0, 0, 1, 0),
             (0, 0, 0, 1), (0, 1, 1, 0), (0, 1, 0, 0), (0, 1, 0, 1)]:
    font_tiles += frame_tile(*spec)



# ---------------------------------------------------------------- emit C
def c_array(ctype, name, vals, per_line=12, fmt="{}"):
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append("    " + ", ".join(fmt.format(v) for v in vals[i:i + per_line]) + ",")
    return "const %s %s[%d] = {\n%s\n};\n" % (ctype, name, len(vals), "\n".join(lines))



def write_png(path, rows):
    raw = b"".join(b"\x00" + bytes(int(v) for px in row for v in px) for row in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))






# ================================================================ Nobble artwork
# must match include/game.h
BOARD_L, BOARD_R = 10, 246
LAUNCH_X, LAUNCH_Y = 128, 22
PEG_R = 9
PIT_Y = 182
LIGHT = (-0.6, -0.8)

INK = (28, 20, 36)
HAZARD_Y = (240, 196, 40)


def rivet(cv, x, y, base):
    cv.put(x, y, scale(base, 1.5))
    cv.put(x + 1, y, scale(base, 1.1))
    cv.put(x, y + 1, scale(base, 1.1))
    cv.put(x + 1, y + 1, scale(base, 0.55))


def steel_panel(cv, x0, x1, base, seed):
    for y in range(H):
        for x in range(x0, x1):
            n = 0.88 + 0.2 * fbm(x / 3, y / 9, seed, 2)          # brushed metal
            c = scale(base, n)
            if (y % 40) in (0, 39) or (x - x0) in (0, x1 - x0 - 1):
                c = scale(base, 0.6 if (y % 40) == 39 or x == x1 - 1 else 1.3)
            cv.put(x, y, c)
    for y in range(4, H, 40):
        for x in (x0 + 3, x1 - 5):
            rivet(cv, x, y, base)
            rivet(cv, x, y + 32, base)


def hazard(cv, x0, x1, y0=0, y1=H):
    for y in range(y0, y1):
        for x in range(x0, x1):
            cv.put(x, y, HAZARD_Y if ((x + y) // 4) % 2 == 0 else (30, 26, 24))


def pegboard(cv, base, seed):
    for y in range(H):
        for x in range(BOARD_L, BOARD_R):
            t = y / H
            c = scale(base, 1.12 - 0.3 * t + 0.08 * fbm(x / 20, y / 20, seed, 2))
            hx, hy = (x - BOARD_L) % 8, y % 8
            if hx in (3, 4) and hy in (3, 4):                   # perforation holes
                c = scale(c, 0.45 if (hx, hy) != (4, 4) else 0.6)
            cv.put(x, y, c)


def pipe(cv, y0, x0, x1, base):
    for y in range(y0, y0 + 6):
        k = [0.55, 1.35, 1.15, 0.95, 0.75, 0.5][y - y0]
        for x in range(x0, x1):
            cv.put(x, y, scale(base, k))
    for x in range(x0 + 10, x1, 30):                           # joints
        for y in range(y0 - 1, y0 + 7):
            cv.put(x, y, scale(base, 0.45))
            cv.put(x + 1, y, scale(base, 1.4))


def shredder(cv):
    """The pit Nobble falls into at the bottom of the board."""
    for y in range(PIT_Y, H):
        for x in range(BOARD_L, BOARD_R):
            if y < PIT_Y + 2:
                c = HAZARD_Y if ((x + y) // 4) % 2 == 0 else (30, 26, 24)
            else:
                k = (y - PIT_Y) / (H - PIT_Y)
                c = scale((50, 40, 56), 1 - 0.8 * k)
                if (x // 6 + y // 3) % 3 == 0 and y < H - 2:
                    c = scale((150, 150, 165), 1 - 0.7 * k)        # shredder teeth
            cv.put(x, y, c)


def launcher(cv, base):
    """Nozzle hanging from the pipe that Nobble is fired from."""
    for y in range(6, 14):
        w = 7 if y < 11 else 6
        for x in range(LAUNCH_X - w, LAUNCH_X + w):
            k = 1.35 if x < LAUNCH_X - w + 2 else 0.6 if x > LAUNCH_X + w - 3 else 1.0
            cv.put(x, y, scale(base, k * (0.8 if y == 13 else 1)))
    for x in range(LAUNCH_X - 5, LAUNCH_X + 5):
        cv.put(x, 13, (30, 26, 34))


def gear(cv, cx, cy, r, teeth, col):
    for y in range(int(cy - r - 3), int(cy + r + 4)):
        for x in range(int(cx - r - 3), int(cx + r + 4)):
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            d = math.hypot(dx, dy)
            a = math.atan2(dy, dx)
            tooth = (math.cos(a * teeth) > 0.2)
            if d < r * 0.3:
                continue
            if d < r or (tooth and d < r + 3):
                lit = -(dx * LIGHT[0] + dy * LIGHT[1]) / max(d, 1)
                cv.put(x, y, scale(col, 0.9 - 0.25 * lit))



# ---------------------------------------------------------------- title
LOGO = {
    "N": ["##...##", "###..##", "####.##", "##.####", "##..###", "##...##", "##...##", "##...##"],
    "U": ["##...##", "##...##", "##...##", "##...##", "##...##", "##...##", "#######", ".#####."],
    "B": ["######.", "##...##", "##...##", "######.", "##...##", "##...##", "##...##", "######."],
    "Y": ["##...##", "##...##", ".##.##.", "..###..", "...#...", "...#...", "..###..", "..###.."],
    "O": [".#####.", "##...##", "##...##", "##...##", "##...##", "##...##", "##...##", ".#####."],
    "L": ["##.....", "##.....", "##.....", "##.....", "##.....", "##.....", "#######", "#######"],
    "E": ["#######", "##.....", "##.....", "######.", "##.....", "##.....", "#######", "#######"],
}
SMALL_GLYPHS = {
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
}
NOBBLE_BODY = [(120, 40, 110), (230, 110, 170), (255, 170, 210), (255, 240, 250)]


def draw_big_nobble(cv, cx, cy, r):
    def body(x, y, d):
        dx, dy = x + 0.5 - cx, y + 0.5 - cy
        if d > r - 1.2:
            return NOBBLE_BODY[0]
        lit = -(dx * LIGHT[0] + dy * LIGHT[1]) / r
        c = mix(NOBBLE_BODY[1], NOBBLE_BODY[2], max(0, lit) * 0.9)
        if math.hypot(dx + r * 0.4, dy + r * 0.45) < r * 0.18:
            c = NOBBLE_BODY[3]
        return scale(c, 0.8 + 0.2 * (1 - (dy / r + 1) / 2) + 0.1)
    disc(cv, cx + 3, cy + 4, r, lambda x, y, d: scale(cv.get(x, y), 0.5) if 0 <= x < W and 0 <= y < H else None)
    disc(cv, cx, cy, r, body)
    for ex in (cx - r * 0.35, cx + r * 0.35):                    # eyes
        disc(cv, ex, cy - r * 0.15, r * 0.24, (255, 255, 255))
        disc(cv, ex + r * 0.05, cy - r * 0.1, r * 0.13, INK)
        cv.put(int(ex - r * 0.05), int(cy - r * 0.25), (255, 255, 255))
    for bx in (cx - r * 0.6, cx + r * 0.6):                      # blush
        disc(cv, bx, cy + r * 0.2, r * 0.12, (255, 120, 150))
    for k in range(-4, 5):                                        # smile
        x = cx + k * r * 0.06
        y = cy + r * 0.28 + (16 - k * k) * r * 0.012
        cv.put(int(x), int(y), INK)


# 3x5 digits drawn onto pegs in game (must match digits3x5 in source/main.c)
DIGITS3 = [r.split() for r in [
    "### #.# #.# #.# ###", ".#. ##. .#. .#. ###", "### ..# ### #.. ###", "### ..# ### ..# ###",
    "#.# #.# ### ..# ..#", "### #.. ### ..# ###", "### #.. ### #.# ###", "### ..# ..# .#. .#.",
    "### #.# ### #.# ###", "### #.# ### ..# ###"]]


def bubble_mask(text, glyphs, cell, gap, top, bounce):
    """Chunky rounded letters: every filled glyph cell becomes a blob."""
    gw = len(glyphs[text[0]][0])
    width = len(text) * gw * cell + (len(text) - 1) * gap * cell
    x0 = (W - width) // 2
    field = [[0.0] * W for _ in range(H)]
    sig = cell * 0.62
    for i, ch in enumerate(text):
        lx = x0 + i * (gw + gap) * cell
        ly = top + bounce[i]
        for gy, row in enumerate(glyphs[ch]):
            for gx, p in enumerate(row):
                if p != "#":
                    continue
                cx, cy = lx + (gx + 0.5) * cell, ly + (gy + 0.5) * cell
                for y in range(int(cy - 3 * sig), int(cy + 3 * sig) + 1):
                    for x in range(int(cx - 3 * sig), int(cx + 3 * sig) + 1):
                        if 0 <= x < W and 0 <= y < H:
                            d2 = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2) / (sig * sig)
                            field[y][x] += math.exp(-d2)
    m = [[field[y][x] > 0.55 for x in range(W)] for y in range(H)]
    return m


def bubble_text(cv, m, depth, face_top, face_bot, gloss, side, outline):
    """Glossy 3D letters like the Nubby's Number Factory logo."""
    def at(x, y):
        return 0 <= x < W and 0 <= y < H and m[y][x]

    def solid(x, y):
        return any(at(x - k, y - k) for k in range(depth + 1))
    ys = [y for y in range(H) if any(m[y])]
    y0, y1 = min(ys), max(ys)
    for y in range(H):                          # soft shadow on the sky
        for x in range(W):
            if not solid(x, y) and solid(x - 4, y - 5):
                cv.shade(x, y, 0.62)
    for y in range(H):
        for x in range(W):
            if solid(x, y):
                continue
            if any(solid(x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                cv.put(x, y, outline)
    for y in range(H):
        for x in range(W):
            if at(x, y):
                t = (y - y0) / max(1, y1 - y0)
                c = mix(face_top, face_bot, t)
                if not at(x - 1, y - 1) or not at(x, y - 1):
                    c = mix(c, (255, 255, 255), 0.55)       # lit rim
                elif not at(x, y - 3) and at(x, y + 2):
                    c = mix(c, gloss, 0.7)                  # glossy band near the top
                elif not at(x + 1, y + 1):
                    c = scale(c, 0.75)
                cv.put(x, y, c)
            elif solid(x, y):
                k = next(k for k in range(1, depth + 1) if at(x - k, y - k))
                cv.put(x, y, scale(side, 1.1 - 0.12 * k))


def glossy_ball(cv, cx, cy, r, col, label=None):
    """A shiny numbered ball, like the ones bouncing around the original game.s logo."""
    disc(cv, cx + r * 0.25, cy + r * 0.3, r, lambda x, y, d: scale(cv.get(x, y), 0.6)
         if 0 <= x < W and 0 <= y < H else None)

    def shade(x, y, d):
        dx, dy = (x + 0.5 - cx) / r, (y + 0.5 - cy) / r
        if d > r - 1:
            return scale(col, 0.35)
        lit = -(dx * LIGHT[0] + dy * LIGHT[1])
        c = scale(col, 0.72 + 0.4 * lit)
        if math.hypot(dx + 0.38, dy + 0.42) < 0.2:
            c = (255, 255, 255)
        elif math.hypot(dx + 0.34, dy + 0.38) < 0.32:
            c = mix(c, (255, 255, 255), 0.5)
        return c
    disc(cv, cx, cy, r, shade)
    if label is None:
        return
    disc(cv, cx, cy + r * 0.05, r * 0.55, lambda x, y, d: (250, 250, 244) if d < r * 0.55 - 1 else scale(col, 0.4))
    if r >= 14:                                     # big ball: 5x7 digits, doubled
        glyphs, gw, cell = {ch: GLYPHS[ch].split() for ch in label}, 5, 2
    else:                                           # small ball: 3x5 digits
        glyphs, gw, cell = {ch: DIGITS3[int(ch)] for ch in label}, 3, 1
    gh = len(next(iter(glyphs.values())))
    w = (len(label) * (gw + 1) - 1) * cell
    lx, ly = int(cx - w / 2 + 0.5), int(cy + r * 0.05 - gh * cell / 2 + 0.5)
    for i, ch in enumerate(label):
        for gy, row in enumerate(glyphs[ch]):
            for gx, p in enumerate(row):
                if p == "#":
                    for yy in range(cell):
                        for xx in range(cell):
                            cv.put(lx + (i * (gw + 1) + gx) * cell + xx, ly + gy * cell + yy, INK)



# ---------------------------------------------------------------- sprites
NOBBLE_PAL = [(0, 0, 0), NOBBLE_BODY[0], NOBBLE_BODY[1], NOBBLE_BODY[2], NOBBLE_BODY[3], INK, (255, 255, 255),
             (255, 120, 150)]
# peg colours by value: 1, 2, 4, ... 256+ (index 5 is the number)
TIERS = [(200, 204, 214), (110, 210, 110), (90, 200, 220), (90, 130, 240), (170, 100, 240),
         (240, 110, 190), (240, 80, 70), (250, 150, 50), (250, 214, 60)]
TIER_PALS = [[scale(c, 0.45), scale(c, 0.85), c, mix(c, (255, 255, 255), 0.6), INK] for c in TIERS]
FLASH_PAL = [(200, 200, 210), (240, 240, 250), (255, 255, 255), (255, 255, 255), INK]
ICON_PAL = [(0, 0, 0), INK, (255, 255, 255),
            (190, 194, 206), (110, 114, 130),     # gray
            (190, 110, 250), (110, 40, 170),      # purple
            (110, 220, 110), (30, 130, 60),       # green
            (255, 214, 80), (190, 120, 20),       # gold
            (110, 190, 255), (30, 90, 200),       # blue
            (255, 160, 200), (200, 70, 130),      # pink
            (255, 130, 60)]                        # orange
ICON_COLORS = {"gray": (3, 4), "purple": (5, 6), "green": (7, 8), "gold": (9, 10),
               "blue": (11, 12), "pink": (13, 14), "orange": (15, 10)}
UP_ARROW = "..#.. .###. #.#.# ..#.. ..#.. ..#.. ..#.."
ICONS = [   # items, in game.h order: (colour, 5x7 symbol)
    ("blue", UP_ARROW),                                           # springs
    ("green", GLYPHS["+"]),                                       # seeder
    ("purple", "..#.. .#.#. #...# ..#.. .#.#. #...# ....."),        # pump: double chevron
    ("gold", "...## ..##. .##.. ##### ..##. .##.. ##..."),          # zapper: lightning
    ("orange", GLYPHS["X"]),                                      # doubler
    ("gray", GLYPHS["Z"]),                                        # ricochet
    ("pink", "..#.. .#### #.#.. .###. ..#.# ####. ..#.."),          # piggy: $
    ("gold", GLYPHS["E"]),                                        # encore
    ("purple", GLYPHS["8"]),                                      # chain
    ("orange", ".###. #...# #...# #...# #...# #...# .###."),        # big
    ("pink", "..... .#.#. ##### ##### .###. ..#.. ....."),          # heart
    ("blue", "..... ##### ..... ..... ##### ..... ....."),          # merger: =
    ("green", "#...# ..#.. .###. ##### .###. ..#.. #...#"),          # spawner: burst
    ("gold", "..#.. .##.. ##### .##.# ..#.# ...## ..#.."),           # surge
    ("orange", "..#.. .###. #.#.# ##### #.#.# .###. ..#.."),         # sniper: crosshair
    ("purple", "..#.. ..#.. .#.#. .#.#. #...# #...# #####"),         # alchemy: flask
    ("gold", "..#.. .#### #.#.. .###. ..#.# ####. ..#.."),           # payroll: $
    ("pink", ".###. #.#.# #.#.# #.### #...# #...# .###."),           # overtime: clock
    ("gray", "##### #...# #...# #...# .#.#. ..#.. ....."),           # shield
    ("green", ".###. #...# #.#.# #...# .###. ..... ....."),          # scope
]
PERK_ICONS = [   # perks, in game.h order
    ("gold", GLYPHS["C"]),                                        # conveyor
    ("purple", ".###. #...# ...#. ..#.. ..#.. ..... ..#.."),        # gremlin: ?
    ("orange", GLYPHS["I"]),                                      # ignition
    ("pink", GLYPHS["R"]),                                        # recycler
    ("blue", GLYPHS["B"]),                                        # bumper
    ("gold", GLYPHS["P"]),                                        # payday
    ("gray", GLYPHS["J"]),                                        # jackpot
    ("green", GLYPHS["D"]),                                       # domino
    ("orange", "..#.. ..#.. .###. ##### .###. ..#.. ..#.."),         # spark
    ("blue", GLYPHS["F"]),                                        # flurry
    ("purple", "..#.. .###. .###. ##### ##### ##### ....."),         # prism
    ("gold", "..#.. .#### #.#.. .###. ..#.# ####. ..#.."),           # cashback: $
    ("blue", GLYPHS["W"]),                                        # whale
    ("pink", GLYPHS["E"]),                                        # echo
    ("gold", GLYPHS["!"]),                                        # finale
]


def peg_sprite(size, r):
    img = [[0] * size for _ in range(size)]
    c = size / 2
    for y in range(size):
        for x in range(size):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            d = math.hypot(dx, dy)
            if d > r:
                continue
            hl = math.hypot(dx + r * 0.35, dy + r * 0.35)
            if hl < r * 0.3:
                img[y][x] = 4
            elif d > r - 1 or dx + dy > r * 1.15:
                img[y][x] = 1
            elif hl < r * 0.75:
                img[y][x] = 3
            else:
                img[y][x] = 2
    return img


def nobble_sprite(blink, size=8, r=4.1):
    img = [[0] * size for _ in range(size)]
    c = size / 2
    k = r / 4.1                      # scale features with the body
    for y in range(size):
        for x in range(size):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            d = math.hypot(dx, dy)
            if d > r:
                continue
            if d > r - 0.8:
                img[y][x] = 1
            elif math.hypot(dx + 1.5 * k, dy + 1.6 * k) < 1.0 * k:
                img[y][x] = 4
            elif dx + dy < 0:
                img[y][x] = 3
            else:
                img[y][x] = 2
    ex = [int(c - 1.5 * k), int(c + 1.2 * k)]
    ey = int(c - 1 * k)
    for x in ex:
        img[ey][x] = 5
        if not blink:
            img[ey - 1][x] = 5
    for x in (int(c - 3 * k + 0.5), int(c + 2.6 * k)):
        img[int(c + 1.3 * k)][x] = 7        # cheeks
    return img


def icon_sprite(colour, symbol, round_badge=False):
    light, dark = ICON_COLORS[colour]
    img = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            if round_badge:
                d = math.hypot(x + 0.5 - 8, y + 0.5 - 8)
                if d > 7.2:
                    continue
                edge = d > 6.2
            else:
                inside = 1 <= x <= 14 and 1 <= y <= 14 and not ((x in (1, 14)) and (y in (1, 14)))
                if not inside:
                    continue
                edge = x in (1, 14) or y in (1, 14) or ((x in (2, 13)) and (y in (2, 13)))
            img[y][x] = 1 if edge else (light if y < 8 else dark)
    rows = symbol.split()
    for r, row in enumerate(rows):
        for c, p in enumerate(row):
            if p == "#":
                img[5 + r][6 + c] = 1        # shadow
    for r, row in enumerate(rows):
        for c, p in enumerate(row):
            if p == "#":
                img[4 + r][5 + c] = 2
    return img




# ---------------------------------------------------------------- DS board (bottom screen)
def launcher(cv, base):
    """Nozzle hanging from the pipe that Nobble is fired from."""
    for y in range(9, 18):
        w = 8 if y < 15 else 7
        for x in range(LAUNCH_X - w, LAUNCH_X + w):
            k = 1.35 if x < LAUNCH_X - w + 2 else 0.6 if x > LAUNCH_X + w - 3 else 1.0
            cv.put(x, y, scale(base, k * (0.8 if y == 17 else 1)))
    for x in range(LAUNCH_X - 6, LAUNCH_X + 6):
        cv.put(x, 17, (30, 26, 34))


BOARD_THEMES = [
    dict(name="steel", wall=(54, 62, 84), board=(78, 96, 128)),
    dict(name="copper", wall=(84, 54, 40), board=(150, 96, 62)),
    dict(name="lab", wall=(40, 72, 70), board=(84, 150, 138)),
]


def render_board(t, seed):
    cv = Canvas()
    pegboard(cv, t["board"], seed)
    steel_panel(cv, 0, BOARD_L, t["wall"], seed + 1)
    steel_panel(cv, BOARD_R, W, t["wall"], seed + 2)
    for y in range(H):                     # frame where the panels meet the board
        cv.put(BOARD_L - 1, y, scale(t["wall"], 0.45))
        cv.put(BOARD_L, y, (200, 204, 214))
        cv.put(BOARD_R - 1, y, (200, 204, 214))
        cv.put(BOARD_R, y, scale(t["wall"], 0.45))
    pipe(cv, 3, 0, W, (170, 176, 190))
    launcher(cv, (170, 176, 190))
    shredder(cv)
    return cv


# ---------------------------------------------------------------- title (both screens)
SMALL_GLYPHS["D"] = ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."]
SMALL_GLYPHS["S"] = [".####", "#....", "#....", ".###.", "....#", "....#", "####."]
RED_LOGO = dict(face_top=(255, 96, 80), face_bot=(196, 18, 30), gloss=(255, 206, 196),
                side=(120, 8, 22), outline=(56, 0, 12))


def sky(y_off=0, seed=17):
    def f(x, y, c):
        yy = y + y_off
        t = yy / (2 * H)
        c = mix((36, 96, 214), (170, 222, 252), t)
        n = fbm(x / 34, yy / 14, seed, 4) + 0.35 * fbm(x / 9, yy / 7, seed + 6, 2) - 0.2 * (1 - t)
        if n > 0.62:                                     # fluffy clouds
            k = min(1.0, (n - 0.62) / 0.14)
            cloud = mix((196, 214, 240), (255, 255, 255), min(1.0, max(0.0, (0.95 - t) * 1.3)))
            c = mix(c, scale(cloud, 0.93 + 0.07 * fbm(x / 4, yy / 4, 5, 2)), k)
        return c
    return f


def render_title_top():
    cv = Canvas()
    cv.each(sky(0))
    bubble_text(cv, bubble_mask("NOBBLE", LOGO, 5, 1, 12, [3, 0, 2, -1, 1, 3]), 5, **RED_LOGO)
    bubble_text(cv, bubble_mask("DS", SMALL_GLYPHS, 4, 1, 68, [0, 2]), 4, **RED_LOGO)
    draw_big_nobble(cv, 46, 140, 28)
    glossy_ball(cv, 208, 132, 20, (60, 190, 80), "8")
    glossy_ball(cv, 166, 170, 12, (250, 130, 40), "2")
    glossy_ball(cv, 236, 176, 11, (60, 140, 240), "16")
    glossy_ball(cv, 100, 172, 11, (190, 90, 230), "4")
    glossy_ball(cv, 124, 124, 8, (240, 80, 70), "1")
    return cv


def render_title_bottom():
    cv = Canvas()
    cv.each(sky(H + 40))
    glossy_ball(cv, 22, 24, 12, (250, 214, 60), "32")
    glossy_ball(cv, 234, 36, 13, (240, 110, 190), "64")
    glossy_ball(cv, 18, 150, 10, (90, 200, 220), "4")
    glossy_ball(cv, 240, 150, 9, (110, 210, 110), "2")
    for y in range(172, H):                              # a conveyor along the bottom
        for x in range(W):
            c = (70, 66, 72) if ((x + y) // 3) % 2 else (46, 42, 50)
            if y == 172:
                c = (150, 150, 160)
            cv.put(x, y, c)
    hazard(cv, 0, W, 170, 172)
    return cv


# ---------------------------------------------------------------- top screen dashboard
# Windows in tile units (x, y, w, h); must match source/hud.c
WIN_SCORE = (1, 3, 20, 8)
WIN_FACE = (22, 3, 9, 8)
WIN_ITEMS = (1, 13, 14, 11)
WIN_PERKS = (16, 13, 15, 11)
LCD = (22, 40, 44)


def inset(cv, win, fill, bevel=(20, 22, 30), light=(150, 156, 176)):
    x0, y0, w, h = (v * 8 for v in win)
    x0 -= 3
    y0 -= 3
    x1, y1 = x0 + w + 6, y0 + h + 3
    for y in range(y0, min(H, y1)):
        for x in range(x0, x1):
            e = min(x - x0, x1 - 1 - x, y - y0, y1 - 1 - y)
            if e == 0:
                c = light if (x == x1 - 1 or y == y1 - 1) else bevel
            elif e == 1:
                c = bevel
            else:
                c = fill(x, y) if callable(fill) else fill
            cv.put(x, y, c)


def render_dashboard():
    cv = Canvas()
    steel_panel(cv, 0, W, (66, 72, 96), 55)
    for y in range(3, 21):                               # header band
        for x in range(W):
            cv.put(x, y, scale((34, 30, 44), 1.1 if y < 5 else 0.95))
    hazard(cv, 0, W, 21, 23)

    def lcd(x, y):
        c = scale(LCD, 1.0 + 0.25 * (1 - y / H))
        return scale(c, 0.85) if y % 2 else c            # scanlines
    inset(cv, WIN_SCORE, lcd)
    face_sky = sky(0, 29)
    inset(cv, WIN_FACE, lambda x, y: face_sky(x * 2, y * 2 - 40, None))
    inset(cv, WIN_ITEMS, lcd)
    inset(cv, WIN_PERKS, lcd)
    return cv


# ---------------------------------------------------------------- DS sprites
UI_PAL = [(0, 0, 0), (80, 24, 70), (170, 70, 140), (230, 110, 170), (255, 170, 210), (255, 236, 248),
          INK, (255, 255, 255), (255, 120, 150), (230, 40, 60), (130, 10, 30), (240, 190, 50),
          (150, 100, 20), (255, 240, 150), (120, 190, 255), (120, 124, 146)]


def portrait(expr):
    """64x64 Nobble face for the top screen: happy, blink, wow or worry."""
    img = [[0] * 64 for _ in range(64)]
    cx, cy, r = 32, 34, 27

    def put(x, y, v):
        if 0 <= x < 64 and 0 <= y < 64:
            img[y][x] = v

    def blob(px, py, rx, ry, v, keep_inside=True):
        for y in range(int(py - ry - 1), int(py + ry + 2)):
            for x in range(int(px - rx - 1), int(px + rx + 2)):
                if ((x + 0.5 - px) / rx) ** 2 + ((y + 0.5 - py) / ry) ** 2 <= 1:
                    put(x, y, v)
    for y in range(64):
        for x in range(64):
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            d = math.hypot(dx, dy)
            if d > r:
                continue
            lit = -(dx * LIGHT[0] + dy * LIGHT[1]) / r
            v = 1 if d > r - 1.6 else 2 if lit < -0.35 else 3 if lit < 0.3 else 4
            if math.hypot(dx + r * 0.42, dy + r * 0.48) < r * 0.16:
                v = 5
            img[y][x] = v
    for s in (-1, 1):
        blob(cx + s * 15, 43, 3.6, 2.4, 8)               # blush
        ex = cx + s * 9
        if expr == "blink":
            for k in range(-4, 5):
                put(ex + k, 31 - (16 - k * k) // 8, 6)
                put(ex + k, 32 - (16 - k * k) // 8, 6)
        else:
            big = expr == "wow"
            blob(ex, 29, 5.2 + big, 6.3 + big, 7)
            if expr == "worry":
                blob(ex + s * 0.5, 32, 2.6, 3, 6)
                for k in range(6):                        # worried brows
                    put(ex - 3 + k, 20 + (k if s < 0 else 5 - k) // 2, 6)
            else:
                blob(ex + 1, 30 + (0 if big else 1), 2.2 if big else 3, 2.6 if big else 3.6, 6)
                put(ex - 1, 26, 7)
                put(ex, 26, 7)
    if expr == "wow":
        blob(cx, 47, 5, 6, 6)
        blob(cx, 50, 3.2, 2.5, 9)
    elif expr == "worry":
        for k in range(-7, 8):
            put(cx + k, 47 + (1 if (k // 2) % 2 else 0), 6)
        blob(cx + 22, 18, 2.5, 3.6, 14)                   # sweat drop
        put(cx + 21, 16, 7)
    else:
        for k in range(-8, 9):
            y = 44 + int((1 - (k / 8.5) ** 2) * 4.5)
            put(cx + k, y, 6)
            put(cx + k, y + 1, 6)
    return img


def heart(full):
    img = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            u, v = (x + 0.5 - 8) / 6.5, -(y + 0.5 - 7.5) / 6.5
            f = (u * u + v * v - 1) ** 3 - u * u * v ** 3
            if f <= 0:
                edge = any(((((x + dx + 0.5 - 8) / 6.5) ** 2 + ((-(y + dy + 0.5 - 7.5) / 6.5)) ** 2 - 1) ** 3
                            - ((x + dx + 0.5 - 8) / 6.5) ** 2 * (-(y + dy + 0.5 - 7.5) / 6.5) ** 3) > 0
                           for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                if edge:
                    img[y][x] = 10 if full else 6
                elif full:
                    img[y][x] = 7 if (x, y) in ((4, 4), (5, 4), (4, 5)) else 8 if u < -0.2 and v > 0.2 else 9
                else:
                    img[y][x] = 15
    return img


def coin():
    img = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            d = math.hypot(x + 0.5 - 8, y + 0.5 - 8)
            if d <= 7:
                img[y][x] = 12 if d > 6 else 13 if (x + y) < 12 else 11
    for r, row in enumerate(".###. #.#.# #.#.. .###. ..#.# #.#.# .###.".split()):
        for c, p in enumerate(row):
            if p == "#":
                img[4 + r][6 + c] = 12
    return img


def icon_big(colour, symbol, round_badge=False):
    """32x32 version of an item or perk icon, for the shop and detail views."""
    light, dark = ICON_COLORS[colour]
    img = [[0] * 32 for _ in range(32)]
    for y in range(32):
        for x in range(32):
            if round_badge:
                d = math.hypot(x + 0.5 - 16, y + 0.5 - 16)
                if d > 15:
                    continue
                edge = d > 13.2
            else:
                if not (1 <= x <= 30 and 1 <= y <= 30):
                    continue
                cx, cy = min(max(x, 5), 26), min(max(y, 5), 26)
                if math.hypot(x - cx, y - cy) > 4.5:
                    continue                                  # rounded corners
                edge = x in (1, 2, 29, 30) or y in (1, 2, 29, 30) or math.hypot(x - cx, y - cy) > 3
            img[y][x] = 1 if edge else (light if y < 16 else dark)
            if not edge and y in (4, 5) and 6 < x < 25:
                img[y][x] = 2 if y == 4 else light              # gloss
    rows = symbol.split()
    for pass_, (ox, oy, v) in enumerate(((7, 8, 1), (6, 7, 2))):
        for r, row in enumerate(rows):
            for c, p in enumerate(row):
                if p == "#":
                    for yy in range(2):
                        for xx in range(2):
                            img[oy + r * 2 + yy][ox + 5 + c * 2 + xx] = v
    return img


PAUSE_SYMBOL = "##.## ##.## ##.## ##.## ##.## ##.## ##.##"


# ---------------------------------------------------------------- build everything
FONT_PAL = [(0, 0, 0), (255, 255, 255), (24, 18, 30), (40, 44, 62), (240, 196, 60),
            (150, 100, 20), (255, 226, 90), (70, 76, 100), (118, 122, 146), (80, 220, 200), (30, 110, 100)]
font_pal = pal16(FONT_PAL)

# progress bar tiles: 0-8 pixels filled, in teal and in gold (goal met)
BAR_TILE = len(font_tiles) // 8
for fill_c in (9, 6):
    for n in range(9):
        img = [[0] * 8 for _ in range(8)]
        for x in range(8):
            img[1][x] = img[6][x] = 7
            for y in range(2, 6):
                img[y][x] = (fill_c if y > 2 else 1) if x < n else 3
        font_tiles += to_tiles(img, 8, 8)

# big 16x16 digits for the score: 2x2 tiles each
BIGDIGIT_TILE = len(font_tiles) // 8
for d in "0123456789":
    img = [[0] * 16 for _ in range(16)]
    for r, row in enumerate(GLYPHS[d].split()):
        for c, p in enumerate(row):
            if p == "#":
                for yy in range(2):
                    for xx in range(2):
                        img[1 + r * 2 + yy + 1][3 + c * 2 + xx + 1] = 2          # shadow
    for r, row in enumerate(GLYPHS[d].split()):
        for c, p in enumerate(row):
            if p == "#":
                for yy in range(2):
                    for xx in range(2):
                        img[1 + r * 2 + yy][3 + c * 2 + xx] = 6 if r < 3 else 4
    font_tiles += to_tiles(img, 16, 16)       # tiles in order: TL, TR, BL, BR

STEEL = (118, 128, 156)
FX_PAL = [(0, 0, 0), (110, 0, 20), (230, 30, 40), (255, 130, 150), (255, 255, 255), (150, 200, 240), (225, 240, 255)]
obj_pal = pal16(NOBBLE_PAL)
for p in TIER_PALS:
    obj_pal += pal16([(0, 0, 0)] + p)
obj_pal += pal16([(0, 0, 0)] + FLASH_PAL)
obj_pal += pal16(ICON_PAL)
obj_pal += pal16([ICON_PAL[0]] + [mix(c, (255, 255, 255), 0.55) for c in ICON_PAL[1:]])   # flash
obj_pal += pal16([(0, 0, 0), scale(STEEL, 0.4), scale(STEEL, 0.75), STEEL, (220, 228, 245), (255, 255, 255)])  # armour
obj_pal += pal16(FX_PAL)                        # laser and wind
obj_pal += pal16(UI_PAL)                        # portrait, hearts, coins, score popups
assert len(obj_pal) == 256

# sprite tiles (4bpp, 1D mapping with 128-byte steps: every sprite starts on a multiple of 4 tiles)
obj_tiles, tile_of = [], {}


def add_sprite(name, img):
    while len(obj_tiles) % 32:
        obj_tiles.append(0)
    tile_of[name] = len(obj_tiles) // 8
    obj_tiles.extend(to_tiles(img, len(img[0]), len(img)))


add_sprite("nobble", nobble_sprite(False, 16, 5.4))
add_sprite("nobble_blink", nobble_sprite(True, 16, 5.4))
add_sprite("nobble_big", nobble_sprite(False, 16, 7.4))
add_sprite("nobble_big_blink", nobble_sprite(True, 16, 7.4))
add_sprite("peg", peg_sprite(32, 11.6))         # template; numbers are drawn on in game


def socket_sprite():
    """A faint ring marking an empty peg slot (armour palette)."""
    img = [[0] * 32 for _ in range(32)]
    for y in range(32):
        for x in range(32):
            d = math.hypot(x + 0.5 - 16, y + 0.5 - 16)
            if 9.5 < d < 11.8:
                lit = ((x - 16) * LIGHT[0] + (y - 16) * LIGHT[1]) / max(d, 1)
                img[y][x] = 2 if lit > 0.3 else 1
    return img


add_sprite("socket", socket_sprite())
dot = [[0] * 8 for _ in range(8)]
dot[3][3] = dot[3][4] = dot[4][3] = dot[4][4] = 6    # white
dot[5][4] = dot[4][5] = dot[5][5] = 5
add_sprite("dot", dot)
add_sprite("spark", [[0] * 8, [0] * 8, [0, 0, 0, 3, 3, 0, 0, 0], [0, 0, 3, 4, 3, 3, 0, 0],
                     [0, 0, 3, 3, 3, 2, 0, 0], [0, 0, 0, 3, 2, 0, 0, 0], [0] * 8, [0] * 8])
add_sprite("laser_warn", [[2 if y in (3, 4) and x % 6 < 3 else 0 for x in range(32)] for y in range(8)])
add_sprite("laser", [[[1, 2, 3, 4, 4, 3, 2, 1][y]] * 32 for y in range(8)])
add_sprite("wind", [[0] * 16, [0] * 16, [0] * 16, [0, 0] + [5] * 8 + [6] * 5 + [0], [0, 0, 0, 0] + [5] * 8 + [0] * 4,
                    [0] * 16, [0] * 16, [0] * 16])
add_sprite("pause", icon_sprite("gray", PAUSE_SYMBOL))
for i, (colour, sym) in enumerate(ICONS):
    add_sprite(f"icon{i}", icon_sprite(colour, sym))
for i, (colour, sym) in enumerate(PERK_ICONS):
    add_sprite(f"perk{i}", icon_sprite(colour, sym, True))
for i, (colour, sym) in enumerate(ICONS):
    add_sprite(f"bigicon{i}", icon_big(colour, sym))
for i, (colour, sym) in enumerate(PERK_ICONS):
    add_sprite(f"bigperk{i}", icon_big(colour, sym, True))
FACES = ["happy", "blink", "wow", "worry"]
for f in FACES:
    add_sprite(f"face_{f}", portrait(f))
add_sprite("heart", heart(True))
add_sprite("heart_empty", heart(False))
add_sprite("coin", coin())
while len(obj_tiles) % 32:
    obj_tiles.append(0)
TILE_FREE = len(obj_tiles) // 8
PEG_TILES = 16                                    # 32x32 4bpp
POPUP_TILES = 8                                   # 32x16 4bpp
NUM_POPUPS = 6
assert TILE_FREE + 20 * PEG_TILES + NUM_POPUPS * POPUP_TILES <= 4096, "out of sprite VRAM"


def bitmap16(cv):
    """Raw RGB15 pixels with the alpha bit set, little-endian."""
    return b"".join(struct.pack("<H", rgb15(*c) | 0x8000) for row in cv.px for c in row)


def write_icon_bmp(path, img, pal):
    """32x32 16-colour BMP for the DS menu icon (magenta = transparent)."""
    rows = b""
    for y in range(31, -1, -1):
        for x in range(0, 32, 2):
            rows += bytes([(img[y][x] << 4) | img[y][x + 1]])
    colours = b"".join(bytes([b, g, r, 0]) for (r, g, b) in pal)
    off = 14 + 40 + len(colours)
    hdr = b"BM" + struct.pack("<IHHI", off + len(rows), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, 32, 32, 1, 4, 0, len(rows), 2835, 2835, 16, 16)
    with open(path, "wb") as f:
        f.write(hdr + info + colours + rows)


face = portrait("happy")
write_icon_bmp(os.path.join(ROOT, "icon.bmp"),
               [[face[min(63, y * 2 + 1)][x * 2 + 1] for x in range(32)] for y in range(32)],
               [(255, 0, 255)] + [tuple(int(v) for v in c) for c in UI_PAL[1:]])

BITMAPS = {"title_top": render_title_top(), "title_bottom": render_title_bottom(), "dashboard": render_dashboard()}
for i, t in enumerate(BOARD_THEMES):
    BITMAPS[f"board{i}"] = render_board(t, 100 + i * 13)
os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
for name, cv in BITMAPS.items():
    with open(os.path.join(ROOT, "data", name + ".bin"), "wb") as f:
        f.write(bitmap16(cv))

# ---------------------------------------------------------------- emit C
hdr = f"""// Generated by tools/gen_assets.py - do not edit.
#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>

#define FONT_CHARS "{FONT_CHARS.replace(chr(39), chr(92) + chr(39))}"
#define FONT_NCHARS {len(FONT_CHARS)}
#define FRAME_TILE {len(FONT_CHARS) * len(FONT_STYLES)}
#define BAR_TILE(n, gold) ({BAR_TILE} + (gold) * 9 + (n))   // progress bar, n of 8 pixels full
#define BIGDIGIT_TILE(d) ({BIGDIGIT_TILE} + (d) * 4)        // 16x16 digit: TL, TR, BL, BR
#define NUM_BOARDS {len(BOARD_THEMES)}

// sprite tiles (32-byte units) and palette banks
#define TILE_NOBBLE {tile_of["nobble"]}
#define TILE_NOBBLE_BLINK {tile_of["nobble_blink"]}
#define TILE_NOBBLE_BIG {tile_of["nobble_big"]}
#define TILE_NOBBLE_BIG_BLINK {tile_of["nobble_big_blink"]}
#define TILE_PEG {tile_of["peg"]}           // 32x32 disc template
#define TILE_SOCKET {tile_of["socket"]}     // 32x32 ring for an empty slot
#define TILE_DOT {tile_of["dot"]}
#define TILE_SPARK {tile_of["spark"]}
#define TILE_LASER_WARN {tile_of["laser_warn"]}  // 32x8
#define TILE_LASER {tile_of["laser"]}        // 32x8
#define TILE_WIND {tile_of["wind"]}          // 16x8
#define TILE_PAUSE {tile_of["pause"]}
#define TILE_ICON(i) ({tile_of["icon0"]} + (i) * 4)
#define TILE_PERK(i) ({tile_of["perk0"]} + (i) * 4)
#define TILE_BIGICON(i) ({tile_of["bigicon0"]} + (i) * 16)
#define TILE_BIGPERK(i) ({tile_of["bigperk0"]} + (i) * 16)
#define TILE_FACE(f) ({tile_of["face_happy"]} + (f) * 64)   // 64x64: happy, blink, wow, worry
#define TILE_HEART {tile_of["heart"]}
#define TILE_HEART_EMPTY {tile_of["heart_empty"]}
#define TILE_COIN {tile_of["coin"]}
#define TILE_FREE {TILE_FREE}        // first unused sprite tile
#define PEG_TILES {PEG_TILES}
#define POPUP_TILES {POPUP_TILES}
#define NUM_POPUPS {NUM_POPUPS}
#define PAL_NOBBLE 0
#define PAL_TIER(t) (1 + (t))                   // peg colour by value tier
#define NUM_TIERS {len(TIERS)}
#define PAL_FLASH {1 + len(TIERS)}
#define PAL_ICON {2 + len(TIERS)}
#define PAL_ICON_FLASH {3 + len(TIERS)}
#define PAL_ARMOR {4 + len(TIERS)}
#define PAL_FX {5 + len(TIERS)}
#define PAL_UI {6 + len(TIERS)}

extern const uint16_t font_pal[16];
extern const uint16_t obj_pal[256];
extern const uint32_t obj_tiles[{len(obj_tiles)}];
extern const uint32_t font_tiles[{len(font_tiles)}];
extern const uint8_t glyphs5x7[10][7];                // digit bitmaps, bit 4 = left column

#endif
"""

src = "// Generated by tools/gen_assets.py - do not edit.\n#include \"assets.h\"\n\n"
src += c_array("uint16_t", "font_pal", font_pal, 8, "0x{:04X}")
src += c_array("uint16_t", "obj_pal", obj_pal, 8, "0x{:04X}")
src += c_array("uint32_t", "obj_tiles", obj_tiles, 8, "0x{:08X}")
src += c_array("uint32_t", "font_tiles", font_tiles, 8, "0x{:08X}")
src += "const uint8_t glyphs5x7[10][7] = {\n"
for d in "0123456789":
    rows = [int(r.replace("#", "1").replace(".", "0"), 2) for r in GLYPHS[d].split()]
    src += "    { " + ", ".join(f"0x{v:02X}" for v in rows) + " },\n"
src += "};\n"

with open(os.path.join(ROOT, "include", "assets.h"), "w") as f:
    f.write(hdr)
with open(os.path.join(ROOT, "source", "assets.c"), "w") as f:
    f.write(src)


# ---------------------------------------------------------------- previews
def rgb_of(cv):
    return [list(row) for row in cv.px]


def ds_preview(name, top, bottom, k=2):
    gap = [[(12, 12, 16)] * W for _ in range(12)]
    rows = top + gap + bottom
    write_png(os.path.join(ROOT, "build", name),
              [[rows[y // k][x // k] for x in range(W * k)] for y in range(len(rows) * k)])


os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
ds_preview("preview_title.png", rgb_of(BITMAPS["title_top"]), rgb_of(BITMAPS["title_bottom"]))
for i, t in enumerate(BOARD_THEMES):
    ds_preview(f"preview_board_{t['name']}.png", rgb_of(BITMAPS["dashboard"]), rgb_of(BITMAPS[f"board{i}"]))
sheet = [[(0, 0, 0)] * 64 for _ in range(64)]


# ================================================================ README screenshots
# A small model of the DS display, drawing each scene the way source/main.c
# does: the picture, then sprites (priority 1+), the text layer, then
# priority-0 sprites. Names, rules and layouts are read from the C source.
GAME_C = open(os.path.join(ROOT, "source", "game.c")).read()
MAIN_C = open(os.path.join(ROOT, "source", "main.c")).read()
ITEMS = [dict(name=m[0], effect=m[1], trig=m[2], price=int(m[3])) for m in re.findall(
    r'\[ITEM_\w+\]\s*=\s*\{\s*"([^"]*)",\s*"([^"]*)",\s*(TRIG_\w+),\s*(\d+)\s*(?:,\s*\d+\s*)?\}', GAME_C)]
PERKS = [dict(name=m[0], line1=m[1], line2=m[2]) for m in re.findall(
    r'\[PERK_\w+\]\s*=\s*\{\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\s*(?:,\s*\d+\s*)?\}', GAME_C)]
TRIGGERS = re.findall(r'\[(TRIG_\w+)\]\s*=\s*"([^"]*)"', GAME_C)
TRIG_INDEX = {name: i for i, (name, _) in enumerate(TRIGGERS)}
BOSSES = re.findall(r'\[BOSS_\w+\]\s*=\s*\{\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"', GAME_C)
LAYOUT_TABLE = {m[0]: [(int(x), int(y)) for x, y in re.findall(r'\{\s*(\d+),\s*(\d+)\s*\}', m[2])]
                for m in re.findall(r'\{\s*"(\w+)",\s*(\d+),\s*\{(.*?)\}\s*\}', GAME_C, re.S)}


def c_strings(name, src):
    body = re.search(name + r'\[\w+\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    return re.findall(r'"([^"]*)"', body)


TRIGGER_SHORT = c_strings("trigger_short", MAIN_C)
PERK_SHORT = c_strings("perk_short", MAIN_C)
ITEM_ID = {it["name"]: i for i, it in enumerate(ITEMS)}
assert len(ITEMS) == len(ICONS) and len(PERKS) == len(PERK_ICONS), "icons out of step with game.c"
PERK_ID = {pk["name"]: i for i, pk in enumerate(PERKS)}


def unpack_tiles(words):
    tiles = []
    for t in range(len(words) // 8):
        tiles.append([[(words[t * 8 + r] >> (4 * c)) & 15 for c in range(8)] for r in range(8)])
    return tiles


FONT_IMG = unpack_tiles(font_tiles)
OBJ_IMG = unpack_tiles(obj_tiles)


def rgb15_to_rgb(c):
    return ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)


FONT_RGB = [rgb15_to_rgb(c) for c in font_pal]
OBJ_RGB = [rgb15_to_rgb(c) for c in obj_pal]
PAL = dict(nobble=0, flash=1 + len(TIERS), icon=2 + len(TIERS), icon_flash=3 + len(TIERS),
           armor=4 + len(TIERS), fx=5 + len(TIERS), ui=6 + len(TIERS))
NCH = len(FONT_CHARS)
FRAME = NCH * len(FONT_STYLES)
PLAIN, PANEL_S, HILITE, GOLD_S, DIM_S, LCD_S = range(6)


def sprite_img(tile, w, h):
    tw = w // 8
    return [[OBJ_IMG[tile + (y // 8) * tw + x // 8][y % 8][x % 8] for x in range(w)] for y in range(h)]


def tier_of(v):
    t = 0
    while v > 1 and t < len(TIERS) - 1:
        v >>= 1
        t += 1
    return t


DIGITS3x5 = [0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7252, 0x7BEF, 0x7BCF, 0x5D35]


def peg_img(v):
    """The same drawing as render_peg() in main.c."""
    img = sprite_img(tile_of["peg"], 32, 32)
    if v < 1000:
        ds = [int(c) for c in str(v)]
        x0 = 16 - (len(ds) * 6 - 1) // 2
        for g, d in enumerate(ds):
            for r, row in enumerate(GLYPHS[str(d)].split()):
                for c, p in enumerate(row):
                    if p == "#":
                        img[13 + r][x0 + g * 6 + c] = 5
    else:
        ds = [int(c) for c in str(v // 1000 if v >= 10000 else v)] + ([10] if v >= 10000 else [])
        x0 = 16 - (len(ds) * 4 - 1) // 2
        for g, d in enumerate(ds):
            for r in range(5):
                for c in range(3):
                    if DIGITS3x5[d] & (1 << (14 - r * 3 - c)):
                        img[14 + r][x0 + g * 4 + c] = 5
    return img


def popup_img(v):
    img = [[0] * 32 for _ in range(16)]
    plus = [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."]
    glyphs = [plus] + [GLYPHS[c].split() for c in str(v)[:4]]
    x0 = 16 - (len(glyphs) * 6 - 1) // 2
    for pass_ in range(2):
        for g, rows in enumerate(glyphs):
            for r, row in enumerate(rows):
                for c, p in enumerate(row):
                    if p != "#":
                        continue
                    px, py = x0 + g * 6 + c, 4 + r
                    if pass_ == 0:
                        for oy in (-1, 0, 1):
                            for ox in (-1, 0, 1):
                                if 0 <= px + ox < 32 and 0 <= py + oy < 16:
                                    img[py + oy][px + ox] = 6
                    else:
                        img[py][px] = 13 if r < 3 else 11
    return img


class Screen:
    def __init__(self, picture):
        self.pic = [list(row) for row in picture.px]
        self.map = [[0] * 32 for _ in range(24)]
        self.half = [[0] * 32 for _ in range(24)]      # BG1: shifted 4 pixels right
        self.sprites = []
        self.dim, self.dim_sprites = 0, False

    def text(self, x, y, s, style=PLAIN, layer=None):
        layer = self.map if layer is None else layer
        for i, ch in enumerate(s):
            if 0 <= x + i < 32:
                layer[y][x + i] = style * NCH + FONT_CHARS.index(ch)

    def center_in(self, x, w, y, s, style=PLAIN):
        start = x * 8 + w * 4 - len(s) * 4
        if start % 8:
            self.text((start - 4) // 8, y, s, style, self.half)
        else:
            self.text(start // 8, y, s, style)

    def center(self, y, s, style=PLAIN):
        self.center_in(0, 32, y, s, style)

    def num(self, x, y, v, width, style):
        self.text(x + width - len(str(v)), y, str(v), style)

    def big_number(self, x, y, v, digits):
        ds = str(v)[-digits:]
        for i, d in enumerate(ds):
            cx = x + (digits - len(ds) + i) * 2
            t = BIGDIGIT_TILE + int(d) * 4
            self.map[y][cx], self.map[y][cx + 1] = t, t + 1
            self.map[y + 1][cx], self.map[y + 1][cx + 1] = t + 2, t + 3

    def bar(self, x, y, tiles, num, den, gold):
        px = min(tiles * 8, num * tiles * 8 // den)
        for i in range(tiles):
            self.map[y][x + i] = BAR_TILE + gold * 9 + max(0, min(8, px - i * 8))

    def button(self, y, w, h, label, style):
        w += (w - len(label)) & 1
        if w & 1:
            self.panel((31 - w) // 2, y, w, h, self.half)
        else:
            self.panel((32 - w) // 2, y, w, h)
        self.center(y + h // 2, label, style)

    def panel(self, x, y, w, h, layer=None):
        layer = self.map if layer is None else layer
        for j in range(h):
            for i in range(w):
                top, bot, left, right = j == 0, j == h - 1, i == 0, i == w - 1
                t = PANEL_S * NCH
                if top:
                    t = FRAME + (0 if left else 2 if right else 1)
                elif bot:
                    t = FRAME + (5 if left else 7 if right else 6)
                elif left:
                    t = FRAME + 3
                elif right:
                    t = FRAME + 4
                layer[y + j][x + i] = t

    def spr(self, x, y, img, pal, prio=1):
        self.sprites.append((x, y, img, pal, prio))

    def render(self):
        k = 1 - self.dim / 16
        out = [[scale(c, k) for c in row] for row in self.pic] if self.dim else [list(r) for r in self.pic]

        def draw_sprites(want_zero):
            for x, y, img, pal, prio in reversed(self.sprites):       # first added draws on top
                if (prio == 0) != want_zero:
                    continue
                for yy, row in enumerate(img):
                    for xx, v in enumerate(row):
                        px, py = x + xx, y + yy
                        if v and 0 <= px < W and 0 <= py < H:
                            c = OBJ_RGB[pal * 16 + v]
                            out[py][px] = scale(c, k) if self.dim and self.dim_sprites else c
        draw_sprites(False)
        for layer, shift in ((self.map, 0), (self.half, 4)):
            for ty in range(24):
                for tx in range(32):
                    t = layer[ty][tx]
                    if not t:
                        continue
                    for yy in range(8):
                        for xx in range(8):
                            v = FONT_IMG[t][yy][xx]
                            px = tx * 8 + xx + shift
                            if v and px < W:
                                out[ty * 8 + yy][px] = FONT_RGB[v]
        draw_sprites(True)
        return out


def icon(i):
    return sprite_img(tile_of[f"icon{i}"], 16, 16)


def perk_icon(i, big=False):
    return sprite_img(tile_of[f"bigperk{i}" if big else f"perk{i}"], 32 if big else 16, 32 if big else 16)


def dashboard(st):
    """The top screen, as hud_text() and hud_sprites() draw it."""
    sc = Screen(BITMAPS["dashboard"])
    sc.text(1, 1, f"ROUND {st['round']}", GOLD_S if st.get("boss") else PLAIN)
    sc.num(27, 1, st["coins"], 4, GOLD_S)
    det = st.get("detail")
    if det:
        kind, name, hint = det
        sc.panel(0, 2, 32, 9)
        if kind == "item":
            it = ITEMS[ITEM_ID[name]]
            sc.text(7, 4, it["name"], HILITE)
            sc.text(20, 4, f"{it['price']} COINS", PANEL_S if st["coins"] >= it["price"] else DIM_S)
            sc.text(7, 6, TRIGGERS[TRIG_INDEX[it["trig"]]][1], PANEL_S)
            sc.text(7, 7, it["effect"], HILITE)
            sc.spr(16, 36, sprite_img(tile_of[f"bigicon{ITEM_ID[name]}"], 32, 32), PAL["icon"], 0)
        else:
            pk = PERKS[PERK_ID[name]]
            sc.text(7, 4, pk["name"], HILITE)
            sc.text(7, 6, pk["line1"], PANEL_S)
            sc.text(7, 7, pk["line2"], HILITE)
            sc.spr(16, 36, perk_icon(PERK_ID[name], True), PAL["icon"], 0)
        sc.center_in(0, 32, 9, hint, PANEL_S)
    else:
        sc.text(2, 3, "SCORE", LCD_S)
        sc.big_number(2, 4, st["score"], 8)
        sc.text(2, 7, f"GOAL {st['goal']}", LCD_S)
        met = st["score"] >= st["goal"]
        sc.bar(2, 8, 18, st["score"], st["goal"], int(met))
        if met:
            sc.text(2, 9, f"RESTOCKS {min(9, st['score'] // st['goal'])}", GOLD_S)
        elif st.get("flying"):
            sc.text(2, 9, f"NEED {st['goal'] - st['score']} MORE", LCD_S)
    if st.get("boss"):
        sc.center(11, "BOSS: " + BOSSES[st["boss"]][0], GOLD_S)
    else:
        until = st.get("shop_in", 2)
        sc.center(11, f"NEXT SHOP IN {until} ROUND" + ("" if until == 1 else "S"))
    items, perks = st.get("items", []), st.get("perks", [])
    sc.text(1, 13, "ITEMS FULL" if len(items) >= 5 else f"ITEMS {len(items)}/5", LCD_S)
    for i, name in enumerate(items):
        it = ITEMS[ITEM_ID[name]]
        lit = name in st.get("flash", ())
        sc.text(4, 14 + i * 2, it["name"], GOLD_S if lit else PLAIN)
        sc.text(4, 15 + i * 2, TRIGGER_SHORT[TRIG_INDEX[it["trig"]]], LCD_S)
        sc.spr(10, 112 + i * 16, icon(ITEM_ID[name]), PAL["icon_flash"] if lit else PAL["icon"])
    sc.text(16, 13, "PERKS", LCD_S)
    if not perks:
        sc.text(16, 15, "ONE EVERY", LCD_S)
        sc.text(16, 16, "5 ROUNDS", LCD_S)
    for i, name in enumerate(perks):
        sc.text(19, 14 + i * 2, name, PLAIN)
        sc.text(19, 15 + i * 2, PERK_SHORT[PERK_ID[name]], LCD_S)
        sc.spr(130, 112 + i * 16, perk_icon(PERK_ID[name]), PAL["icon"])
    face = FACES.index(st.get("face", "happy"))
    sc.spr(180, 24, sprite_img(tile_of["face_happy"] + face * 64, 64, 64), PAL["ui"])
    for h in range(st.get("max_lives", 3)):
        sc.spr(80 + h * 14, 4, sprite_img(tile_of["heart" if h < st["lives"] else "heart_empty"], 16, 16), PAL["ui"])
    sc.spr(200, 4, sprite_img(tile_of["coin"], 16, 16), PAL["ui"])
    return sc


def predict(layout, pegs, angle, n=12):
    """game_predict(): the aim guide, up to the first peg."""
    x, y = 128 * 256, 22 * 256
    vx = int(math.sin(angle * math.pi / 128) * 256) * 4
    vy = int(math.cos(angle * math.pi / 128) * 256) * 4
    pts = []
    for f in range(1, 61):
        for _ in range(2):
            vy += 11
            vx, vy = max(-1792, min(1792, vx)), max(-1792, min(1792, vy))
            x += vx // 2
            y += vy // 2
            if x < (10 + 5) * 256 or x > (246 - 5) * 256:
                vx = -vx
            for (sx, sy), v in zip(layout, pegs):
                if v and math.hypot(x / 256 - sx, y / 256 - sy) < 16:
                    return pts
        if f % 4 == 0 and len(pts) < n:
            pts.append((x // 256, y // 256))
    return pts


def board(st):
    """The bottom screen, as board_sprites() draws it."""
    sc = Screen(BITMAPS[f"board{st.get('theme', 0)}"])
    layout = LAYOUT_TABLE[st["layout"]]
    pegs = st["pegs"]
    for k, (px, py, v, pal) in enumerate(st.get("sparks", [])):
        sc.spr(px - 4, py - 4, sprite_img(tile_of["spark"], 8, 8), 1 + tier_of(v) if pal is None else pal)
    for (px, py, v) in st.get("popups", []):
        sc.spr(px - 16, py - 20, popup_img(v), PAL["ui"])
    if "nobble" in st:
        nx, ny = st["nobble"]
        sc.sprites.insert(0, (nx - 8, ny - 8, sprite_img(tile_of["nobble"], 16, 16), PAL["nobble"], 1))
    elif st.get("aim") is not None:
        sc.sprites.insert(0, (120, 14, sprite_img(tile_of["nobble"], 16, 16), PAL["nobble"], 1))
        for (dx, dy) in predict(layout, pegs, st["aim"]):
            sc.spr(dx - 4, dy - 4, sprite_img(tile_of["dot"], 8, 8), PAL["nobble"])
    if st.get("pause", True):
        sc.spr(236, 1, sprite_img(tile_of["pause"], 16, 16), PAL["icon"])
    for i, ((sx, sy), v) in enumerate(zip(layout, pegs)):
        if not v:
            sc.spr(sx - 16, sy - 16, sprite_img(tile_of["socket"], 32, 32), PAL["armor"], 2)
            continue
        pal = PAL["flash"] if i in st.get("flash", ()) else PAL["armor"] if i in st.get("armor", ()) else 1 + tier_of(v)
        sc.spr(sx - 16, sy - 16, peg_img(v), pal)
    if st.get("laser") is not None:
        for k in range(8):
            sc.spr(k * 32, st["laser"] - 4, sprite_img(tile_of["laser"], 32, 8), PAL["fx"], 0)
    if st.get("wind"):
        for k in range(10):
            x = (k * 67 + 40) % 236
            sc.spr(10 + x - 8, 30 + k * 15, sprite_img(tile_of["wind"], 16, 8), PAL["fx"], 2)
    return sc


def buttons(sc, labels, sel=0):
    for i, (y, w, h, label) in enumerate(labels):
        sc.button(y, w, h, label, HILITE if i == sel else PANEL_S)


def save_ds(name, top, bottom):
    ds_preview(name, top.render(), bottom.render())


# --- title: logo on top, menu on the touch screen
top = Screen(BITMAPS["title_top"])
bot = Screen(BITMAPS["title_bottom"])
buttons(bot, [(3, 20, 5, "PLAY"), (9, 20, 3, "HOW TO PLAY"), (13, 20, 3, "CREDITS")])
bot.panel(2, 18, 28, 3)
bot.center(19, "BEST ROUND 17  LAUNCH 4096", HILITE)
save_ds("shot_title.png", top, bot)

# --- aiming on the Pyramid layout
PYR = [16, 8, 4, 2, 32, 1, 4, 8, 0, 16, 2, 64, 4, 1, 8]
st = dict(round=4, coins=7, lives=3, max_lives=3, score=0, goal=48, items=["ZAPPER", "RICOCHET", "ENCORE"],
          perks=["CONVEYOR"], shop_in=3)
save_ds("shot_aim.png", dashboard(st), board(dict(layout="PYRAMID", pegs=PYR, aim=-48)))

# --- mid-launch: pops throw sparks and scores, the goal bar fills, Nobble cheers
DIA = [32, 16, 64, 8, 32, 16, 128, 0, 64, 8, 32, 16, 256, 32, 16, 64]
st = dict(round=9, coins=12, lives=3, max_lives=4, score=688, goal=412, flying=True, face="wow",
          items=["MERGER", "ZAPPER", "SNIPER", "CHAIN", "SHIELD"], flash=("SNIPER",),
          perks=["FLURRY", "WHALE"], shop_in=1)
sparks = [(106, 102, 64, None), (96, 94, 64, None), (116, 96, 64, None), (100, 112, 64, None), (114, 110, 64, None),
          (60, 98, 16, None), (68, 108, 16, None)]
save_ds("shot_launch.png", dashboard(st),
        board(dict(layout="DIAMOND", pegs=DIA, theme=2, nobble=(118, 118), flash=(7,), sparks=sparks,
                   popups=[(106, 96, 128), (62, 96, 32)])))

# --- the shop: cards on the touch screen, details on top
st = dict(round=7, coins=9, lives=3, max_lives=3, score=0, goal=1, items=["ZAPPER", "RICOCHET", "ENCORE"],
          perks=["CONVEYOR", "IGNITION"], detail=("item", "OVERTIME", "TAP AGAIN TO BUY"))
bot = Screen(BITMAPS["board0"])
bot.dim = 12
bot.center(1, "SHOP", GOLD_S)
bot.center(2, "COINS 9   ITEMS 3/5", GOLD_S)
for s_, (name, on) in enumerate((("SURGE", 0), ("OVERTIME", 1), ("SCOPE", 0))):
    it = ITEMS[ITEM_ID[name]]
    row = 4 + s_ * 5
    bot.panel(1, row, 30, 5)
    bot.text(7, row + 1, name, HILITE if on else PANEL_S)
    bot.text(21, row + 1, f"{it['price']} COINS", PANEL_S)
    bot.text(7, row + 3, TRIGGER_SHORT[TRIG_INDEX[it["trig"]]], DIM_S)
    if on:
        bot.text(2, row + 2, ">", HILITE)
    bot.spr(16, row * 8 + 4, sprite_img(tile_of[f"bigicon{ITEM_ID[name]}"], 32, 32), PAL["icon"], 0)
buttons(bot, [(19, 18, 3, "NEXT ROUND")], sel=-1)
save_ds("shot_shop.png", dashboard(st), bot)

# --- Laser Grid boss: the beam wipes out a band of pegs
COP = [8, 16, 32, 16, 0, 0, 0, 0, 8, 32, 64, 8, 16, 4, 32, 16]     # the beam just wiped out row two
st = dict(round=5, boss=1, coins=6, lives=2, max_lives=3, score=96, goal=204, flying=True, face="worry",
          items=["PUMP", "ZAPPER", "SPRINGS"], perks=["GREMLIN"])
save_ds("shot_laser.png", dashboard(st),
        board(dict(layout="FUNNEL", pegs=COP, theme=1, nobble=(146, 104), laser=86,
                   sparks=[(62, 84, 0, PAL["fx"]), (100, 86, 0, PAL["fx"]), (156, 86, 0, PAL["fx"]),
                           (194, 84, 0, PAL["fx"]), (70, 90, 0, PAL["fx"])])))

# --- Armour Plating boss: steel pegs need a hit before they score
COL = [128, 64, 256, 64, 32, 512, 64, 128, 64, 128, 32, 256, 128, 64, 256, 64, 128, 32, 1024]
st = dict(round=15, boss=3, coins=4, lives=2, max_lives=4, score=1216, goal=3740, flying=True,
          items=["ZAPPER", "RICOCHET", "ENCORE", "CHAIN", "HEART"], perks=["CONVEYOR", "IGNITION", "JACKPOT"])
save_ds("shot_armor.png", dashboard(st),
        board(dict(layout="COLUMNS", pegs=COL, theme=2, nobble=(128, 120), armor=(2, 5, 11, 14, 18))))

# --- the items and perks screen from the pause menu
st = dict(round=12, coins=14, lives=3, max_lives=3, score=0, goal=1,
          items=["SPAWNER", "ZAPPER", "RICOCHET", "PAYROLL"], perks=["CONVEYOR", "ECHO"],
          detail=("perk", "ECHO", ""))
bot = Screen(BITMAPS["board1"])
bot.dim = 12
bot.center(1, "ITEMS AND PERKS", GOLD_S)
bot.center(2, "TOUCH ONE TO READ ABOUT IT", GOLD_S)
bot.panel(1, 4, 30, 7)
bot.text(3, 4, "ITEMS", HILITE)
bot.panel(1, 11, 30, 7)
bot.text(3, 11, "PERKS", HILITE)
buttons(bot, [(19, 14, 3, "BACK")], sel=-1)
owned = [("i", n) for n in st["items"]] + [("p", n) for n in st["perks"]]
for k, (kind, name) in enumerate(owned):
    row = 0 if kind == "i" else 1
    col = k if row == 0 else k - len(st["items"])
    x, y = 26 + col * 44, 108 if row else 52
    sel = k == len(st["items"]) + 1
    img = sprite_img(tile_of[f"bigicon{ITEM_ID[name]}"], 32, 32) if kind == "i" else perk_icon(PERK_ID[name], True)
    bot.spr(x, y - (3 if sel else 0), img, PAL["icon_flash"] if sel else PAL["icon"], 0)
save_ds("shot_inventory.png", dashboard(st), bot)

# --- a boss intro
st = dict(round=10, boss=2, coins=8, lives=3, max_lives=3, score=0, goal=486,
          items=["SEEDER", "PUMP", "PIGGY"], perks=["RECYCLER", "BUMPER"])
bot = board(dict(layout="RING", pegs=[32, 16, 64, 8, 32, 16, 128, 16, 32, 8, 256, 16, 32], theme=0, aim=0,
                 wind=True, pause=False))
bot.dim = 9
bot.dim_sprites = True
bot.panel(3, 5, 26, 13)
bot.center(6, "BOSS ROUND!", HILITE)
bot.center(8, BOSSES[2][0], HILITE)
bot.center(10, BOSSES[2][1], PANEL_S)
bot.center(11, BOSSES[2][2], PANEL_S)
bot.center(13, "WIN FOR +3 COINS", PANEL_S)
bot.center(15, "TOUCH TO START", HILITE)
save_ds("shot_boss.png", dashboard(st), bot)
print("sprite tiles:", TILE_FREE, "+ runtime", 20 * PEG_TILES + NUM_POPUPS * POPUP_TILES,
      " font tiles:", len(font_tiles) // 8)
