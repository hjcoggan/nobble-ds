#!/usr/bin/env python3
"""Generate source/assets.c and include/assets.h for Nubby GBA: palettes,
256-color background images (title + factory boards), sprite and font tiles.
Also writes build/preview_*.png for a quick look.

Run from the repo root:  python3 tools/gen_assets.py
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 240, 160
BG_FIRST_COLOR = 32        # palette 0-31 is shared with the text layer
BG_MAX_COLORS = 256 - BG_FIRST_COLOR


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



# ---------------------------------------------------------------- quantize
def quantize(cv, max_colors):
    """Median-cut the image down to max_colors. Returns (palette, index rows)."""
    counts = {}
    for row in cv.px:
        for c in row:
            k = rgb15(*c)
            counts[k] = counts.get(k, 0) + 1

    def comps(k):
        return (k & 31, (k >> 5) & 31, (k >> 10) & 31)

    boxes = [list(counts)]
    while len(boxes) < max_colors:
        best, best_score, best_ch = None, -1, 0
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            for ch in range(3):
                vals = [comps(k)[ch] for k in b]
                rng = max(vals) - min(vals)
                score = rng * sum(counts[k] for k in b)
                if score > best_score:
                    best, best_score, best_ch = i, score, ch
        if best is None:
            break
        b = sorted(boxes[best], key=lambda k: comps(k)[best_ch])
        tot = sum(counts[k] for k in b)
        acc, cut = 0, 1
        for j, k in enumerate(b):
            acc += counts[k]
            if acc >= tot / 2:
                cut = max(1, min(len(b) - 1, j))
                break
        boxes[best:best + 1] = [b[:cut], b[cut:]]

    pal, lut = [], {}
    for b in boxes:
        tot = sum(counts[k] for k in b)
        avg = [round(sum(comps(k)[ch] * counts[k] for k in b) / tot) for ch in range(3)]
        idx = len(pal)
        pal.append(avg[0] | (avg[1] << 5) | (avg[2] << 10))
        for k in b:
            lut[k] = idx
    rows = [[lut[rgb15(*c)] for c in row] for row in cv.px]
    return pal, rows


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


def bg_image(cv):
    pal, rows = quantize(cv, BG_MAX_COLORS)
    full = [0] * 256
    for i, c in enumerate(pal):
        full[BG_FIRST_COLOR + i] = c
    idx = [[BG_FIRST_COLOR + v for v in row] for row in rows]
    return full, to_tiles(idx, W, H, 8), idx


def pal_to_rgb(pal):
    return [((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3) for c in pal]


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
# styles: plain, on a panel, highlighted on a panel, gold (no panel), dimmed on a panel
FONT_STYLES = [(1, 2, 0), (1, 2, 3), (6, 2, 3), (6, 2, 0), (8, 2, 3)]
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




# ================================================================ Nubby artwork
BOARD_L, BOARD_R = 40, 200
LAUNCH_X, LAUNCH_Y = 120, 18
PIT_Y = 150
# must match slots[] in source/game.c
SLOTS = [(66, 42), (102, 42), (138, 42), (174, 42), (84, 60), (120, 60), (156, 60),
         (66, 78), (102, 78), (138, 78), (174, 78), (84, 96), (120, 96), (156, 96),
         (66, 114), (102, 114), (138, 114), (174, 114), (84, 132), (120, 132), (156, 132)]
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
    """The pit Nubby falls into at the bottom of the board."""
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
    """Nozzle hanging from the pipe that Nubby is fired from."""
    for y in range(6, 14):
        w = 7 if y < 11 else 6
        for x in range(LAUNCH_X - w, LAUNCH_X + w):
            k = 1.35 if x < LAUNCH_X - w + 2 else 0.6 if x > LAUNCH_X + w - 3 else 1.0
            cv.put(x, y, scale(base, k * (0.8 if y == 13 else 1)))
    for x in range(LAUNCH_X - 5, LAUNCH_X + 5):
        cv.put(x, 13, (30, 26, 34))


def slot_rings(cv, base):
    """Faint sockets so empty peg slots are still visible."""
    for sx, sy in SLOTS:
        for y in range(sy - 9, sy + 10):
            for x in range(sx - 9, sx + 10):
                d = math.hypot(x + 0.5 - sx, y + 0.5 - sy)
                if 5.5 < d < 7.5:
                    lit = ((x - sx) * LIGHT[0] + (y - sy) * LIGHT[1]) / max(d, 1)
                    cv.put(x, y, scale(cv.get(x, y), 0.7 + 0.25 * lit))


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
    pipe(cv, 2, BOARD_L, BOARD_R, (170, 176, 190))
    launcher(cv, (170, 176, 190))
    slot_rings(cv, t["board"])
    shredder(cv)
    return cv


# ---------------------------------------------------------------- title
LOGO = {
    "N": ["##...##", "###..##", "####.##", "##.####", "##..###", "##...##", "##...##", "##...##"],
    "U": ["##...##", "##...##", "##...##", "##...##", "##...##", "##...##", "#######", ".#####."],
    "B": ["######.", "##...##", "##...##", "######.", "##...##", "##...##", "##...##", "######."],
    "Y": ["##...##", "##...##", ".##.##.", "..###..", "...#...", "...#...", "..###..", "..###.."],
}
SMALL_GLYPHS = {
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
}
NUBBY_BODY = [(120, 40, 110), (230, 110, 170), (255, 170, 210), (255, 240, 250)]


def draw_big_nubby(cv, cx, cy, r):
    def body(x, y, d):
        dx, dy = x + 0.5 - cx, y + 0.5 - cy
        if d > r - 1.2:
            return NUBBY_BODY[0]
        lit = -(dx * LIGHT[0] + dy * LIGHT[1]) / r
        c = mix(NUBBY_BODY[1], NUBBY_BODY[2], max(0, lit) * 0.9)
        if math.hypot(dx + r * 0.4, dy + r * 0.45) < r * 0.18:
            c = NUBBY_BODY[3]
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
    """A shiny numbered ball, like the ones bouncing around the Nubby's logo."""
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


def render_title():
    cv = Canvas()

    def sky(x, y):
        t = y / H
        c = mix((36, 96, 214), (150, 214, 252), t)
        n = fbm(x / 34, y / 14, 17, 4) + 0.35 * fbm(x / 9, y / 7, 23, 2) - 0.25 * (1 - t)
        if n > 0.62:                                     # fluffy clouds
            k = min(1.0, (n - 0.62) / 0.14)
            cloud = mix((196, 214, 240), (255, 255, 255), min(1.0, max(0.0, (0.95 - t) * 1.2)))
            c = mix(c, scale(cloud, 0.93 + 0.07 * fbm(x / 4, y / 4, 5, 2)), k)
        return c
    cv.each(lambda x, y, c: sky(x, y))

    red = dict(face_top=(255, 96, 80), face_bot=(196, 18, 30), gloss=(255, 206, 196),
               side=(120, 8, 22), outline=(56, 0, 12))
    bubble_text(cv, bubble_mask("NUBBY", LOGO, 5, 1, 5, [3, 0, 2, -1, 1]), 4, **red)
    bubble_text(cv, bubble_mask("GBA", SMALL_GLYPHS, 3, 1, 50, [0, 1, 0]), 3, **red)

    draw_big_nubby(cv, 30, 104, 19)
    glossy_ball(cv, 206, 98, 16, (60, 190, 80), "8")
    glossy_ball(cv, 186, 125, 9, (250, 130, 40), "2")
    glossy_ball(cv, 224, 126, 9, (60, 140, 240), "16")
    glossy_ball(cv, 60, 127, 9, (190, 90, 230), "4")
    return cv


# ---------------------------------------------------------------- sprites
NUBBY_PAL = [(0, 0, 0), NUBBY_BODY[0], NUBBY_BODY[1], NUBBY_BODY[2], NUBBY_BODY[3], INK, (255, 255, 255),
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
]
PERK_ICONS = [   # perks, in game.h order
    ("gold", GLYPHS["C"]),                                        # cheesy
    ("purple", ".###. #...# ...#. ..#.. ..#.. ..... ..#.."),        # chaotic: ?
    ("orange", GLYPHS["W"]),                                      # waffle
    ("pink", GLYPHS["K"]),                                        # kebab
    ("blue", GLYPHS["S"]),                                        # springy
    ("gold", GLYPHS["T"]),                                        # trophy
    ("gray", GLYPHS["B"]),                                        # buckshot
    ("green", GLYPHS["H"]),                                       # house of cards
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


def nubby_sprite(blink, size=8, r=4.1):
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


# ---------------------------------------------------------------- build everything
FONT_PAL = [(0, 0, 0), (255, 255, 255), (24, 18, 30), (40, 44, 62), (240, 196, 60),
            (150, 100, 20), (255, 226, 90), (70, 76, 100), (118, 122, 146)]
font_pal = pal16(FONT_PAL)
obj_pal = pal16(NUBBY_PAL)
for p in TIER_PALS:
    obj_pal += pal16([(0, 0, 0)] + p)
obj_pal += pal16([(0, 0, 0)] + FLASH_PAL)
obj_pal += pal16(ICON_PAL)
obj_pal += pal16([ICON_PAL[0]] + [mix(c, (255, 255, 255), 0.55) for c in ICON_PAL[1:]])   # flash
STEEL = (118, 128, 156)
obj_pal += pal16([(0, 0, 0), scale(STEEL, 0.4), scale(STEEL, 0.75), STEEL, (220, 228, 245), (255, 255, 255)])  # armour
FX_PAL = [(0, 0, 0), (110, 0, 20), (230, 30, 40), (255, 130, 150), (255, 255, 255), (150, 200, 240), (225, 240, 255)]
obj_pal += pal16(FX_PAL)                        # laser and wind

# sprite tiles (4bpp, 1D mapping); record where each sprite starts
obj_tiles, tile_of = [], {}


def add_sprite(name, img):
    tile_of[name] = len(obj_tiles) // 8
    obj_tiles.extend(to_tiles(img, len(img[0]), len(img)))


add_sprite("nubby", nubby_sprite(False))
add_sprite("nubby_blink", nubby_sprite(True))
add_sprite("nubby_big", nubby_sprite(False, 16, 6.1))
add_sprite("nubby_big_blink", nubby_sprite(True, 16, 6.1))
add_sprite("peg", peg_sprite(16, 7.6))          # template; numbers are drawn on in game
dot = [[0] * 8 for _ in range(8)]
dot[3][3] = dot[3][4] = dot[4][3] = dot[4][4] = 6    # white
dot[5][4] = dot[4][5] = dot[5][5] = 5
add_sprite("dot", dot)
add_sprite("laser_warn", [[2 if y in (3, 4) and x % 4 < 2 else 0 for x in range(8)] for y in range(8)])
add_sprite("laser", [[[1, 2, 3, 4, 4, 3, 2, 1][y]] * 8 for y in range(8)])
add_sprite("wind", [[0] * 8, [0] * 8, [0] * 8, [0, 5, 5, 5, 6, 6, 6, 0], [0, 0, 5, 5, 5, 5, 0, 0], [0] * 8, [0] * 8, [0] * 8])
for i, (colour, sym) in enumerate(ICONS):
    add_sprite(f"icon{i}", icon_sprite(colour, sym))
for i, (colour, sym) in enumerate(PERK_ICONS):
    add_sprite(f"perk{i}", icon_sprite(colour, sym, True))

title_cv = render_title()
title_pal, title_tiles, title_idx = bg_image(title_cv)
board_imgs = [bg_image(render_board(t, 100 + i * 13)) for i, t in enumerate(BOARD_THEMES)]

# ---------------------------------------------------------------- emit C
hdr = f"""// Generated by tools/gen_assets.py - do not edit.
#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>

#define FONT_CHARS "{FONT_CHARS.replace(chr(39), chr(92) + chr(39))}"
#define FONT_NCHARS {len(FONT_CHARS)}
#define FRAME_TILE {len(FONT_CHARS) * len(FONT_STYLES)}
#define NUM_BOARDS {len(BOARD_THEMES)}
#define BG_FIRST_COLOR {BG_FIRST_COLOR}
#define BG_IMG_WORDS {len(title_tiles)}

// sprite tiles and palette banks
#define TILE_NUBBY {tile_of["nubby"]}
#define TILE_NUBBY_BLINK {tile_of["nubby_blink"]}
#define TILE_NUBBY_BIG {tile_of["nubby_big"]}
#define TILE_NUBBY_BIG_BLINK {tile_of["nubby_big_blink"]}
#define TILE_PEG {tile_of["peg"]}           // 16x16 disc template (4 tiles)
#define TILE_DOT {tile_of["dot"]}
#define TILE_ICON(i) ({tile_of["icon0"]} + (i) * 4)
#define TILE_PERK(i) ({tile_of["perk0"]} + (i) * 4)
#define TILE_LASER_WARN {tile_of["laser_warn"]}
#define TILE_LASER {tile_of["laser"]}
#define TILE_WIND {tile_of["wind"]}
#define TILE_FREE {len(obj_tiles) // 8}        // first unused sprite tile
#define PAL_NUBBY 0
#define PAL_TIER(t) (1 + (t))                   // peg colour by value tier
#define NUM_TIERS {len(TIERS)}
#define PAL_FLASH {1 + len(TIERS)}
#define PAL_ICON {2 + len(TIERS)}
#define PAL_ICON_FLASH {3 + len(TIERS)}
#define PAL_ARMOR {4 + len(TIERS)}
#define PAL_FX {5 + len(TIERS)}

extern const uint16_t font_pal[16];
extern const uint16_t obj_pal[{len(obj_pal)}];
extern const uint16_t title_pal[256];
extern const uint32_t title_tiles[BG_IMG_WORDS];
extern const uint16_t *const board_pal[NUM_BOARDS];
extern const uint32_t *const board_tiles[NUM_BOARDS];
extern const uint32_t obj_tiles[{len(obj_tiles)}];
extern const uint32_t font_tiles[{len(font_tiles)}];

#endif
"""

src = "// Generated by tools/gen_assets.py - do not edit.\n#include \"assets.h\"\n\n"
src += c_array("uint16_t", "font_pal", font_pal, 8, "0x{:04X}")
src += c_array("uint16_t", "obj_pal", obj_pal, 8, "0x{:04X}")
src += c_array("uint16_t", "title_pal", title_pal, 8, "0x{:04X}")
src += c_array("uint32_t", "title_tiles", title_tiles, 8, "0x{:08X}")
for i, (pal, tiles, _) in enumerate(board_imgs):
    src += c_array("uint16_t", f"board{i}_pal", pal, 8, "0x{:04X}").replace("const", "static const", 1)
    src += c_array("uint32_t", f"board{i}_tiles", tiles, 8, "0x{:08X}").replace("const", "static const", 1)
src += "const uint16_t *const board_pal[NUM_BOARDS] = { " + ", ".join(f"board{i}_pal" for i in range(len(board_imgs))) + " };\n"
src += "const uint32_t *const board_tiles[NUM_BOARDS] = { " + ", ".join(f"board{i}_tiles" for i in range(len(board_imgs))) + " };\n"
src += c_array("uint32_t", "obj_tiles", obj_tiles, 8, "0x{:08X}")
src += c_array("uint32_t", "font_tiles", font_tiles, 8, "0x{:08X}")

with open(os.path.join(ROOT, "include", "assets.h"), "w") as f:
    f.write(hdr)
with open(os.path.join(ROOT, "source", "assets.c"), "w") as f:
    f.write(src)


# ---------------------------------------------------------------- previews
def to_rgb(pal, idx):
    rgb = pal_to_rgb(pal)
    return [[rgb[idx[y][x]] for x in range(W)] for y in range(H)]


def blit(img, spr, sx, sy, pal):
    for y, row in enumerate(spr):
        for x, v in enumerate(row):
            if v and 0 <= sx + x < W and 0 <= sy + y < H:
                img[sy + y][sx + x] = pal[v]


def text(img, tx, ty, s, fg=(255, 255, 255)):
    for i, ch in enumerate(s):
        for r, row in enumerate(GLYPHS.get(ch, ". " * 7).split()):
            for c, p in enumerate(row):
                if p == "#":
                    img[ty * 8 + r + 1][(tx + i) * 8 + c + 2] = FONT_PAL[2]
                    img[ty * 8 + r][(tx + i) * 8 + c + 1] = fg


PEG_BLIT = peg_sprite(16, 7.6)
ICON_BLIT_PAL = [(0, 0, 0)] + ICON_PAL[1:]


def draw_peg(img, sx, sy, v, pal=None, digit=INK):
    tier = min(len(TIERS) - 1, v.bit_length() - 1)
    blit(img, PEG_BLIT, sx - 8, sy - 8, pal or [(0, 0, 0)] + TIER_PALS[tier])
    digits = str(v)
    w = len(digits) * 4 - 1
    for k, ch in enumerate(digits):
        for r, row in enumerate(DIGITS3[int(ch)]):
            for c, p in enumerate(row):
                if p == "#":
                    img[sy - 3 + r][sx - w // 2 + k * 4 + c] = digit


def draw_panel(img, y, w, h):
    """Same look as the in-game panel frame tiles."""
    x0 = (30 - w) // 2 * 8
    x1, y0, y1 = x0 + w * 8, y * 8, (y + h) * 8
    for py in range(y0, y1):
        for px in range(x0, x1):
            e = min(px - x0, x1 - 1 - px, py - y0, y1 - 1 - py)
            img[py][px] = FONT_PAL[5] if e == 0 else FONT_PAL[4] if e == 1 else \
                FONT_PAL[7] if e == 2 and (px - x0 == 2 or py - y0 == 2) else FONT_PAL[3]


def center(img, ty, s, fg=(255, 255, 255)):
    text(img, (30 - len(s)) // 2, ty, s, fg)


def shade_all(img, k):
    for row in img:
        row[:] = [scale(c, k) for c in row]


def scene(theme, values, round_="4", goal="38", score="12", lives="3", coins="7",
          items=(2, 3, 5, 7), perks=(0, 5), shop_in="2", boss=False, armor=(), nubby=None, aim=True):
    pal, _, idx = board_imgs[theme]
    img = to_rgb(pal, idx)
    armor_pal = [(0, 0, 0), scale(STEEL, 0.4), scale(STEEL, 0.75), STEEL, (220, 228, 245)]
    for i, ((sx, sy), v) in enumerate(zip(SLOTS, values)):
        if v:
            if i in armor:
                draw_peg(img, sx, sy, v, armor_pal, (255, 255, 255))
            else:
                draw_peg(img, sx, sy, v)
    nx, ny = nubby or (LAUNCH_X, LAUNCH_Y)
    blit(img, nubby_sprite(False), nx - 4, ny - 4, NUBBY_PAL)
    if aim:
        for k in range(1, 7):
            x, y = LAUNCH_X + k * 3, LAUNCH_Y + k * 4 + k * k // 4
            blit(img, dot, x - 4, y - 4, NUBBY_PAL)
    if boss:
        text(img, 0, 1, "BOSS!", FONT_PAL[6])
    else:
        text(img, 0, 1, "ROUND")
    for row, sv in ((2, round_), (5, goal), (8, score), (11, lives)):
        text(img, 0, row, sv.rjust(5))
    for row, sv in ((4, "GOAL"), (7, "SCORE"), (10, "LIVES")):
        text(img, 0, row, sv)
    text(img, 25, 1, "COINS")
    text(img, 25, 2, coins.rjust(5))
    text(img, 25, 3, "ITEMS")
    text(img, 25, 4, " FULL" if len(items) == 5 else f"  {len(items)}/5", FONT_PAL[6] if len(items) == 5 else (255, 255, 255))
    for i, it in enumerate(items):
        blit(img, icon_sprite(*ICONS[it]), 214, 41 + i * 15, ICON_BLIT_PAL)
    text(img, 25, 15, "SHOP")
    text(img, 25, 16, "IN" + shop_in.rjust(3))
    if perks:
        text(img, 0, 13, "PERKS")
    for i, pk in enumerate(perks):
        blit(img, icon_sprite(*PERK_ICONS[pk], True), 2 + (i % 2) * 18, 112 + (i // 2) * 18, ICON_BLIT_PAL)
    return img


BOARD_VALUES = [1, 2, 1, 4, 2, 8, 1, 0, 2, 16, 4, 1, 2, 32, 1, 0, 4, 2, 8, 1, 2]


def board_preview(n):
    return scene(n, BOARD_VALUES)


def laser_scene():
    vals = [4, 8, 4, 2, 8, 16, 8, 4, 0, 8, 4, 16, 8, 16, 4, 8, 4, 2, 16, 8, 4]
    img = scene(1, vals, round_="5", goal="96", score="40", coins="9", items=(3, 5, 1), perks=(1,),
                shop_in="2", boss=True, nubby=(96, 70), aim=False)
    for y in range(8):
        for x in range(BOARD_L, BOARD_R):
            img[96 - 4 + y][x] = FX_PAL[[1, 2, 3, 4, 4, 3, 2, 1][y]]
    return img


def armor_scene():
    vals = [16, 8, 16, 32, 8, 64, 16, 8, 32, 16, 8, 16, 64, 8, 32, 16, 8, 16, 32, 8, 16]
    img = scene(2, vals, round_="15", goal="412", score="136", lives="2", coins="4", items=(3, 5, 7, 8, 10),
                perks=(0, 2, 6), shop_in="1", boss=True, armor=(3, 5, 8, 12, 14, 18), nubby=(150, 108), aim=False)
    return img


def shop_scene():
    img = scene(0, BOARD_VALUES, aim=False)
    shade_all(img, 0.25)
    draw_panel(img, 1, 28, 18)
    center(img, 2, "SHOP", FONT_PAL[6])
    center(img, 3, "COINS 7   ITEMS 4/5", FONT_PAL[6])
    for s, (it, name, price) in enumerate(((0, "SPRINGS", 6), (4, "DOUBLER", 5), (10, "HEART", 7))):
        row = 5 + s * 3
        blit(img, icon_sprite(*ICONS[it]), 24, 36 + s * 24, ICON_BLIT_PAL)
        if s == 1:
            text(img, 2, row, ">", FONT_PAL[6])
        text(img, 6, row, name, FONT_PAL[6] if s == 1 else (255, 255, 255))
        text(img, 18, row, f"{price} COINS", (255, 255, 255) if price <= 7 else FONT_PAL[8])
    text(img, 6, 14, "NEXT ROUND")
    center(img, 16, "FIRST PEG POPPED:")
    center(img, 17, "DOUBLE A RANDOM PEG", FONT_PAL[6])
    return img


def boss_intro_scene():
    vals = [2, 4, 2, 8, 4, 16, 4, 2, 8, 4, 8, 2, 4, 8, 16, 4, 2, 8, 4, 2, 4]
    img = scene(0, vals, round_="10", goal="87", score="0", coins="6", items=(1, 2, 6), perks=(3,),
                shop_in="1", boss=True, aim=False)
    for k in range(10):                      # wind streaks
        x = (k * 53 + 40) % 160
        blit(img, [[0] * 8, [0] * 8, [0] * 8, [0, 5, 5, 5, 6, 6, 6, 0], [0, 0, 5, 5, 5, 5, 0, 0]],
             BOARD_L + x - 4, 24 + k * 13, FX_PAL)
    shade_all(img, 1 - 9 / 16)
    draw_panel(img, 4, 24, 12)
    center(img, 5, "BOSS ROUND!", FONT_PAL[6])
    center(img, 7, "WIND TUNNEL", FONT_PAL[6])
    center(img, 9, "GUSTS PUSH NUBBY")
    center(img, 10, "LEFT AND RIGHT")
    center(img, 12, "WIN FOR +3 COINS")
    center(img, 14, "PRESS A")
    return img


def inventory_scene():
    img = scene(1, BOARD_VALUES, items=(3, 5, 7, 8), perks=(0, 2), aim=False)
    shade_all(img, 0.25)
    draw_panel(img, 2, 28, 16)
    center(img, 3, "ITEMS AND PERKS", FONT_PAL[6])
    center(img, 4, "ITEM 2 OF 4")
    owned = [("i", 3), ("i", 5), ("i", 7), ("i", 8), ("p", 0), ("p", 2)]
    x = 120 - len(owned) * 9
    lit_pal = [ICON_PAL[0]] + [mix(c, (255, 255, 255), 0.55) for c in ICON_PAL[1:]]
    for k, (kind, n) in enumerate(owned):
        spr = icon_sprite(*ICONS[n]) if kind == "i" else icon_sprite(*PERK_ICONS[n], True)
        blit(img, spr, x + k * 18, 44 if k == 1 else 48, lit_pal if k == 1 else ICON_BLIT_PAL)
    center(img, 9, "RICOCHET", FONT_PAL[6])
    center(img, 11, "WALL BOUNCE:")
    center(img, 12, "POP A RANDOM PEG")
    center(img, 15, "LEFT AND RIGHT TO BROWSE")
    center(img, 16, "B TO GO BACK")
    return img


def save_scaled(name, img, k):
    write_png(os.path.join(ROOT, "build", name),
              [[img[y // k][x // k] for x in range(W * k)] for y in range(H * k)])


os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
title_rgb = to_rgb(title_pal, title_idx)
text(title_rgb, 9, 13, "PRESS START")
for x in range(8, 232):                 # the best-score strip drawn in game
    for y in range(136, 160):
        title_rgb[y][x] = FONT_PAL[3] if 8 < x < 231 and 136 < y < 159 else FONT_PAL[4]
text(title_rgb, 3, 18, "BEST ROUND 3  LAUNCH 347", FONT_PAL[6])
save_scaled("preview_title.png", title_rgb, 3)
for n, t in enumerate(BOARD_THEMES):
    save_scaled(f"preview_board_{t['name']}.png", board_preview(n), 2)
for name, fn in (("laser", laser_scene), ("armor", armor_scene), ("shop", shop_scene),
                 ("boss_intro", boss_intro_scene), ("inventory", inventory_scene)):
    save_scaled(f"preview_{name}.png", fn(), 2)
print("sprite tiles:", len(obj_tiles) // 8, " font tiles:", len(font_tiles) // 8)
