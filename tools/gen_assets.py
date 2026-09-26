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
FONT_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:!->+=.',"
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
}
# styles: plain, on a panel, highlighted on a panel, gold (no panel)
FONT_STYLES = [(1, 2, 0), (1, 2, 3), (6, 2, 3), (6, 2, 0)]
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
BUCKET_Y, NUM_BUCKETS = 140, 5
BUCKET_W = (BOARD_R - BOARD_L) // NUM_BUCKETS
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


def buckets(cv, base, accent):
    for y in range(BUCKET_Y, H):
        for x in range(BOARD_L, BOARD_R):
            slot = (x - BOARD_L) // BUCKET_W
            k = 0.55 + 0.25 * ((y - BUCKET_Y) / (H - BUCKET_Y))
            c = scale(accent if slot == 2 else base, k)
            if y >= H - 5:                                       # conveyor floor
                c = (70, 66, 72) if ((x + y) // 3) % 2 else (46, 42, 50)
            cv.put(x, y, c)
    for k in range(1, NUM_BUCKETS):                              # divider posts
        x = BOARD_L + k * BUCKET_W
        for y in range(BUCKET_Y, H - 5):
            cv.put(x - 1, y, (200, 200, 210))
            cv.put(x, y, (120, 120, 132))
        disc(cv, x, BUCKET_Y, 1.8, (230, 230, 240))


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
    dict(name="steel", wall=(54, 62, 84), board=(78, 96, 128), bucket=(60, 70, 96), accent=(150, 110, 40)),
    dict(name="copper", wall=(84, 54, 40), board=(150, 96, 62), bucket=(100, 64, 44), accent=(60, 120, 110)),
    dict(name="lab", wall=(40, 72, 70), board=(84, 150, 138), bucket=(52, 98, 92), accent=(150, 80, 150)),
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
    # the dropper nozzle hangs from the pipe
    buckets(cv, t["bucket"], t["accent"])
    # little gauges on the side panels
    for gx, gy in ((20, 150), (220, 150)):
        disc(cv, gx, gy, 6, (230, 226, 210))
        disc(cv, gx, gy, 6.5, lambda x, y, d: (40, 36, 40) if d > 5.3 else None)
        for k in range(5):
            cv.put(gx - 3 + k, gy - 1 + abs(k - 2) // 2, (200, 40, 40) if k == 4 else (40, 36, 40))
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


def render_title():
    cv = Canvas()
    t = BOARD_THEMES[0]
    def wall(x, y):
        n = 0.85 + 0.25 * fbm(x / 10, y / 10, 71, 3)
        vx, vy = (x - 120) / 150, (y - 70) / 120
        v = max(0.3, 1 - (vx * vx + vy * vy) * 1.2)
        return scale((58, 66, 92), n * v)
    cv.each(lambda x, y, c: wall(x, y))
    gear(cv, 18, 26, 22, 10, (90, 96, 118))
    gear(cv, 226, 120, 26, 12, (90, 96, 118))
    gear(cv, 200, 22, 12, 8, (110, 116, 140))
    pipe(cv, 94, 0, 70, (150, 156, 170))
    pipe(cv, 70, 176, 240, (150, 156, 170))
    # a few pegs and falling numbers for flavour
    for (px, py, col) in ((52, 120, (230, 230, 240)), (68, 108, (190, 90, 230)), (172, 104, (240, 196, 60)),
                          (188, 122, (230, 230, 240)), (40, 104, (230, 230, 240)), (200, 100, (80, 200, 240))):
        disc(cv, px, py, 4 if col != (80, 200, 240) else 6, lambda x, y, d, c=col: scale(c, 1.1 - d / 8))
    carved_text(cv, "NUBBY", LOGO, 5, 1, 10,
                face_top=(255, 236, 120), face_bot=(250, 130, 60),
                edge_lt=(255, 252, 210), edge_dk=(150, 60, 20), outline=INK, band=False)
    carved_text(cv, "GBA", SMALL_GLYPHS, 2, 1, 58,
                face_top=(150, 230, 255), face_bot=(60, 150, 230), edge_lt=(230, 250, 255),
                edge_dk=(20, 70, 140), outline=INK, band=False)
    draw_big_nubby(cv, 120, 100, 20)
    # conveyor belt
    for y in range(148, H):
        for x in range(W):
            c = (70, 66, 72) if ((x + y) // 3) % 2 else (46, 42, 50)
            if y == 148:
                c = (150, 150, 160)
            cv.put(x, y, c)
    hazard(cv, 0, W, 146, 148)
    return cv


# ---------------------------------------------------------------- sprites
NUBBY_PAL = [(0, 0, 0), NUBBY_BODY[0], NUBBY_BODY[1], NUBBY_BODY[2], NUBBY_BODY[3], INK, (255, 255, 255),
             (255, 120, 150)]
PEG_PALS = [
    [(80, 84, 96), (170, 176, 190), (225, 228, 238), (255, 255, 255)],      # plus
    [(200, 150, 40), (255, 236, 120), (255, 252, 200), (255, 255, 255)],    # plus lit
    [(70, 20, 110), (150, 60, 210), (210, 140, 250), (255, 230, 255)],      # mult
    [(200, 60, 200), (255, 140, 255), (255, 220, 255), (255, 255, 255)],    # mult lit
    [(130, 90, 10), (230, 176, 40), (255, 226, 110), (255, 255, 220)],      # coin
    [(230, 150, 20), (255, 230, 90), (255, 255, 190), (255, 255, 255)],     # coin lit
    [(20, 60, 130), (40, 150, 230), (140, 220, 255), (255, 255, 255)],      # bumper
    [(80, 150, 240), (150, 230, 255), (230, 250, 255), (255, 255, 255)],    # bumper flash
]
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
ICONS = [   # (colour, 5x7 symbol) in item order
    ("gray", GLYPHS["2"]),
    ("purple", GLYPHS["X"]),
    ("gold", GLYPHS["+"]),
    ("blue", ".###. ....# .###. #.... .###. ....# .###."),
    ("green", GLYPHS["7"]),
    ("pink", "..#.. .#### #.#.. .###. ..#.# ####. ..#.."),
    ("orange", "..#.. .###. #.#.# ..#.. ..#.. ..#.. ..#.."),
    ("gray", GLYPHS["5"]),
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
            elif d > r - 1 or dx + dy > r * 0.9:
                img[y][x] = 1
            elif hl < r * 0.75:
                img[y][x] = 3
            else:
                img[y][x] = 2
    return img


def bumper_sprite():
    img = peg_sprite(16, 7.2)
    for y in range(16):
        for x in range(16):
            d = math.hypot(x + 0.5 - 8, y + 0.5 - 8)
            if img[y][x] and 3.5 < d < 4.6:
                img[y][x] = 3            # inner ring
            if img[y][x] and d < 1.6:
                img[y][x] = 4
    return img


def nubby_sprite(blink):
    img = [[0] * 8 for _ in range(8)]
    for y in range(8):
        for x in range(8):
            dx, dy = x + 0.5 - 4, y + 0.5 - 4
            d = math.hypot(dx, dy)
            if d > 4.1:
                continue
            if d > 3.3:
                img[y][x] = 1
            elif math.hypot(dx + 1.5, dy + 1.6) < 1.0:
                img[y][x] = 4
            elif dx + dy < 0:
                img[y][x] = 3
            else:
                img[y][x] = 2
    if blink:
        img[3][2] = img[3][5] = 5
    else:
        img[3][2] = img[3][5] = 5
        img[2][2] = img[2][5] = 5
    img[5][1] = img[5][6] = 7            # cheeks
    return img


def icon_sprite(colour, symbol):
    light, dark = ICON_COLORS[colour]
    img = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
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
            (150, 100, 20), (255, 226, 90), (70, 76, 100)]
font_pal = pal16(FONT_PAL)
obj_pal = pal16(NUBBY_PAL)
for p in PEG_PALS:
    obj_pal += pal16([(0, 0, 0)] + p)
obj_pal += pal16(ICON_PAL)

# sprite tiles (4bpp, 1D mapping); record where each sprite starts
obj_tiles, tile_of = [], {}


def add_sprite(name, img):
    tile_of[name] = len(obj_tiles) // 8
    obj_tiles.extend(to_tiles(img, len(img[0]), len(img)))


add_sprite("nubby", nubby_sprite(False))
add_sprite("nubby_blink", nubby_sprite(True))
add_sprite("peg", peg_sprite(8, 3.6))
add_sprite("bumper", bumper_sprite())
for i, (colour, sym) in enumerate(ICONS):
    add_sprite(f"icon{i}", icon_sprite(colour, sym))

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
#define TILE_PEG {tile_of["peg"]}
#define TILE_BUMPER {tile_of["bumper"]}
#define TILE_ICON(i) ({tile_of["icon0"]} + (i) * 4)
#define PAL_NUBBY 0
#define PAL_PEG(type, lit) (1 + (type) * 2 + (lit))   // type: plus, mult, coin, bumper
#define PAL_ICON {1 + len(PEG_PALS)}

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


def board_preview(n):
    pal, _, idx = board_imgs[n]
    img = to_rgb(pal, idx)
    peg = peg_sprite(8, 3.6)
    bump = bumper_sprite()
    k = 0
    for row in range(8):
        y = 34 + row * 13
        for c in range(8 if row & 1 else 9):
            x = (60 if row & 1 else 52) + c * 16
            k += 1
            if hash2(x, y, n) < 0.14:
                continue
            kind = 1 if k % 11 == 0 else 2 if k % 13 == 0 else 0
            lit = 1 if (row in (0, 1, 2) and c in (3, 4)) else 0
            blit(img, peg, x - 4, y - 4, [(0, 0, 0)] + PEG_PALS[kind * 2 + lit])
    blit(img, bump, 100 - 8, 73 - 8, [(0, 0, 0)] + PEG_PALS[6])
    blit(img, nubby_sprite(False), 116 - 4, 44 - 4, NUBBY_PAL)
    for i, lab in enumerate(["X2", "X1", "X3", "X1", "X2"]):
        text(img, 6 + i * 4, 18, lab, FONT_PAL[6])
    for row, s in ((1, "ROUND"), (2, "   01"), (4, "GOAL"), (5, "  130"), (7, "SCORE"), (8, "   64"),
                   (10, "DROPS"), (11, "    3"), (13, "PTS"), (14, "   12"), (15, "MULT"), (16, "X   2")):
        text(img, 0, row, s)
    text(img, 25, 1, "COINS")
    text(img, 25, 2, "    7")
    text(img, 25, 4, "ITEMS")
    for i, it in enumerate((0, 5, 7)):
        light = [(0, 0, 0)] + ICON_PAL[1:]
        blit(img, icon_sprite(*ICONS[it]), 212, 44 + i * 20, light)
    return img


def save_scaled(name, img, k):
    write_png(os.path.join(ROOT, "build", name),
              [[img[y // k][x // k] for x in range(W * k)] for y in range(H * k)])


os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
title_rgb = to_rgb(title_pal, title_idx)
text(title_rgb, 9, 16, "PRESS START")
save_scaled("preview_title.png", title_rgb, 3)
for n, t in enumerate(BOARD_THEMES):
    save_scaled(f"preview_board_{t['name']}.png", board_preview(n), 2)
print("sprite tiles:", len(obj_tiles) // 8, " font tiles:", len(font_tiles) // 8)
