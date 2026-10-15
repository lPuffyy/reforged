"""Draws the app icon (an angular steel "R" heated toward ember at the bottom, a molten crack and sparks, on a dark forge background) and writes the PNG sizes iOS wants into ios/icons/.
usage: python -I tools/make_icon.py        (needs numpy only; the PNGs are committed, this is how they were made)"""
import os, struct, sys, zlib
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N = 2048   # drawn at 2x and averaged down


def sd_box(x, y, cx, cy, hw, hh, r=0.0):
    qx = np.abs(x - cx) - (hw - r); qy = np.abs(y - cy) - (hh - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def sd_seg(x, y, ax, ay, bx, by, r):
    px, py = x - ax, y - ay; dx, dy = bx - ax, by - ay
    t = np.clip((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1)
    return np.hypot(px - t * dx, py - t * dy) - r


def inside(x, y, poly):   # even-odd point in polygon
    c = np.zeros(x.shape, bool); n = len(poly)
    for k in range(n):
        x1, y1 = poly[k]; x2, y2 = poly[(k + 1) % n]
        cond = ((y1 > y) != (y2 > y)) & (x < (x2 - x1) * (y - y1) / (y2 - y1 + 1e-12) + x1)
        c ^= cond
    return c


def edge_dist(x, y, poly):
    d = np.full(x.shape, 9.0)
    for k in range(len(poly)):
        a = poly[k]; b = poly[(k + 1) % len(poly)]
        d = np.minimum(d, sd_seg(x, y, a[0], a[1], b[0], b[1], 0.0))
    return d


def smooth(a, b, v): t = np.clip((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)


def draw():
    g = (np.arange(N) + 0.5) / N
    x, y = np.meshgrid(g, g)
    # background: near-black blue steel with a furnace glow rising from the bottom
    glow = np.exp(-(((x - 0.55) / 0.6) ** 2 + ((y - 1.1) / 0.5) ** 2))
    vign = 1 - 0.5 * np.hypot(x - 0.5, y - 0.45)
    img = np.stack([0.035 + 0.40 * glow, 0.045 + 0.13 * glow, 0.07 + 0.03 * glow], -1) * vign[..., None]
    # faint diagonal forging lines
    lines = (np.sin((x + y) * 90) > 0.985) * 0.04
    img = img + lines[..., None] * np.array([0.5, 0.6, 0.9])
    # the letter: an angular, sharp-cut R
    outer = [(0.25, 0.19), (0.63, 0.19), (0.78, 0.26), (0.82, 0.40), (0.77, 0.53), (0.67, 0.59), (0.83, 0.81), (0.64, 0.81), (0.52, 0.62), (0.42, 0.62), (0.42, 0.81), (0.25, 0.81)]
    hole = [(0.42, 0.33), (0.58, 0.33), (0.64, 0.385), (0.58, 0.47), (0.42, 0.47)]
    m = inside(x, y, outer) & ~inside(x, y, hole)
    de = np.minimum(edge_dist(x, y, outer), edge_dist(x, y, hole))
    cover = np.where(m, 1.0, 0.0)
    # shadow and outer glow
    halo = np.exp(-np.maximum(de, 0) / 0.05) * (~m)
    img = img + halo[..., None] * np.array([1.0, 0.42, 0.10]) * 0.30
    # steel body (bright at the top, dark below) heated to ember toward the bottom
    steel_t = np.clip((y - 0.19) / 0.62, 0, 1)
    steel = np.array([0.70, 0.75, 0.86]) * (1 - steel_t[..., None]) + np.array([0.30, 0.33, 0.42]) * steel_t[..., None]
    heat = smooth(0.50, 0.84, y)[..., None]
    ember = np.array([1.0, 0.42, 0.10]) * (1 - smooth(0.7, 0.84, y)[..., None]) + np.array([1.0, 0.72, 0.25]) * smooth(0.7, 0.84, y)[..., None]
    body = steel * (1 - heat) + ember * heat
    # brushed-metal streaks
    body = body * (1 + 0.05 * np.sin(x * 700 + y * 40))[..., None]
    # bevel: bright rim on the edges, hot orange on the lower-right ones
    rim = np.clip(1 - de / 0.022, 0, 1)[..., None]
    lit = np.clip(0.5 + 1.3 * (0.5 - (x + y) / 2 + 0.0), 0, 1)[..., None]
    body = body + rim * (0.16 * lit * np.array([1, 1, 1]) + 0.35 * (1 - lit) * np.array([1.0, 0.45, 0.1]))
    # molten crack running up through the letter
    pts = [(0.30, 0.80), (0.38, 0.70), (0.45, 0.66), (0.52, 0.55), (0.60, 0.50), (0.66, 0.38), (0.73, 0.30)]
    dc = np.full(x.shape, 9.0)
    for k in range(len(pts) - 1): dc = np.minimum(dc, sd_seg(x, y, pts[k][0], pts[k][1], pts[k + 1][0], pts[k + 1][1], 0.0))
    core = np.exp(-(dc / 0.006) ** 2)[..., None]; bloom = np.exp(-(dc / 0.03))[..., None]
    body = body + bloom * 0.35 * np.array([1.0, 0.45, 0.1]) + core * np.array([1.0, 0.85, 0.5]) * 0.95
    img = np.where(m[..., None], body, img)
    # sparks flying up and to the right
    rng = np.random.RandomState(7)
    for _ in range(46):
        sx = rng.uniform(0.55, 0.95); sy = rng.uniform(0.05, 0.5) + (sx - 0.55) * 0.0
        ln = rng.uniform(0.012, 0.045); ang = rng.uniform(0.9, 1.25)
        ex, ey = sx + ln * np.cos(ang), sy - ln * np.sin(ang)
        r = rng.uniform(0.0016, 0.0042)
        dd = sd_seg(x, y, sx, sy, ex, ey, r)
        a = np.clip(0.5 - dd * N, 0, 1) * rng.uniform(0.6, 1.0)
        gl = np.exp(-np.maximum(dd, 0) / 0.01) * 0.25
        col = np.array([1.0, 0.8, 0.4]) if rng.rand() < 0.6 else np.array([1.0, 0.5, 0.15])
        img = img + gl[..., None] * col * (~m)[..., None]
        img = np.where((a > 0)[..., None] & (~m)[..., None], img * (1 - a[..., None]) + col * a[..., None], img)
    img = np.clip(img, 0, 1)
    return img.reshape(N // 2, 2, N // 2, 2, 3).mean(axis=(1, 3))   # 1024 x 1024


def resize(img, size):
    n = img.shape[0]
    if size == n: return img
    f = n / size
    idx = ((np.arange(size) + 0.5) * f).astype(int)
    # box average for clean downscaling
    k = int(np.ceil(f)); out = np.zeros((size, size, 3))
    for j in range(size):
        y0 = int(j * f); y1 = min(n, max(y0 + 1, int((j + 1) * f)))
        rows = img[y0:y1]
        for i in range(size):
            x0 = int(i * f); x1 = min(n, max(x0 + 1, int((i + 1) * f)))
            out[j, i] = rows[:, x0:x1].mean(axis=(0, 1))
    return out


def write_png(path, img):   # opaque RGB (iOS rejects icons with an alpha channel)
    a = (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)
    raw = b''.join(b'\x00' + a[j].tobytes() for j in range(a.shape[0]))
    def chunk(tag, data): return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', a.shape[1], a.shape[0], 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    out = os.path.join(ROOT, 'ios', 'icons'); os.makedirs(out, exist_ok=True)
    big = draw()
    for name, size in (('AppIcon1024.png', 1024), ('AppIcon60x60@3x.png', 180), ('AppIcon60x60@2x.png', 120), ('AppIcon76x76@2x~ipad.png', 152), ('AppIcon83.5x83.5@2x~ipad.png', 167)):
        write_png(os.path.join(out, name), resize(big, size)); print('wrote', name)


if __name__ == '__main__': main()
