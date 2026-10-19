#!/usr/bin/env python3
"""Genera la LiveArea desde los assets del propio juego (Icon.png / Default.png, en CgBI).

Salida: extras/livearea_game/ (ignorada por git: es contenido del IPA). CMake la usa en lugar
de los placeholders de extras/livearea/ cuando existe. PNG de 8 bits indexados, como exige la Vita.

Uso: python3 -I tools/make_livearea.py [carpeta .app]
"""
import os, shutil, struct, sys, zlib
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'game_app')
OUT = os.path.join(ROOT, 'extras', 'livearea_game')


def load_png(path):
    """PNG normal o CgBI de Apple (deflate crudo, BGRA premultiplicado)."""
    d = open(path, 'rb').read()
    if d[12:16] != b'CgBI':
        return Image.open(path).convert('RGBA')
    i, idat, w, h = 8, b'', 0, 0
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        i += 12 + n
        if t == b'IHDR':
            w, h = struct.unpack('>II', c[:8])
        elif t == b'IDAT':
            idat += c
    raw, prev, px, o = zlib.decompress(idat, -15), bytearray(w * 4), bytearray(), 0
    for _ in range(h):
        f, line = raw[o], bytearray(raw[o + 1:o + 1 + w * 4])
        o += 1 + w * 4
        for x in range(w * 4):
            a, b, c = line[x - 4] if x >= 4 else 0, prev[x], prev[x - 4] if x >= 4 else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(0, w * 4, 4):
            B, G, R, A = line[x:x + 4]
            if A:
                R, G, B = (min(255, v * 255 // A) for v in (R, G, B))
            px += bytes((R, G, B, A))
    return Image.frombytes('RGBA', (w, h), bytes(px))


def canvas(size, bg, items):
    """Fondo bg con cada (imagen, alto relativo, centro x relativo) encima."""
    img = Image.new('RGBA', size, bg)
    for src, rel_h, cx in items:
        hh = int(size[1] * rel_h)
        s = src.resize((max(1, src.width * hh // src.height), hh), Image.LANCZOS)
        img.alpha_composite(s, (int(size[0] * cx - s.width / 2), (size[1] - hh) // 2))
    return img


def save_indexed(img, name):
    img.convert('RGB').quantize(256, Image.MEDIANCUT).save(os.path.join(OUT, name), optimize=True)


icon = load_png(os.path.join(APP, 'Icon.png'))
logo = load_png(os.path.join(APP, 'Default.png')).rotate(90, expand=True)  # splash vertical -> horizontal
os.makedirs(OUT, exist_ok=True)
dark = (20, 16, 14, 255)
save_indexed(canvas((128, 128), dark, [(icon, 1.0, 0.5)]), 'icon0.png')
save_indexed(canvas((960, 544), dark, [(icon, 0.45, 0.28), (logo, 0.45, 0.68)]), 'pic0.png')
save_indexed(canvas((840, 500), dark, [(icon, 0.5, 0.5)]), 'bg0.png')
save_indexed(canvas((280, 158), dark, [(icon, 0.8, 0.5)]), 'startup.png')
shutil.copy(os.path.join(ROOT, 'extras', 'livearea', 'template.xml'), OUT)
print(f'LiveArea generada en {OUT}')
