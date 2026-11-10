#!/usr/bin/env python3
"""Generate DRUMku's icon as HVIF: two drumsticks rolling on a snare.

HVIF is Haiku's vector icon format (what Icon-O-Matic exports and what
BIconUtils renders). The encoder and the look (soft shadow, dark outline, a
body lit from the top left, gloss) are those of Kasen's tools/make_icons.py.
Writes:

  standalone/icons/drumku.hvif       open in Icon-O-Matic to edit
  standalone/icons/drumku_icon.rdef  the vector_icon resource DRUMku.rdef includes

Run from the repository root:  python3 tools/make_icon.py
"""

import math
import os
import struct
import sys

# ---- format constants (FlatIconFormat.h) -----------------------------------

STYLE_TYPE_SOLID_COLOR = 1
STYLE_TYPE_GRADIENT = 2
SHAPE_TYPE_PATH_SOURCE = 10
TRANSFORMER_TYPE_STROKE = 23

GRADIENT_LINEAR = 0
GRADIENT_CIRCULAR = 1
GRADIENT_FLAG_TRANSFORM = 1 << 1

PATH_FLAG_CLOSED = 1 << 1
PATH_FLAG_NO_CURVES = 1 << 3

SHAPE_FLAG_HAS_TRANSFORMERS = 1 << 4
SHAPE_FLAG_TRANSLATION = 1 << 5

# agg line joins / caps, as the stroke transformer stores them
JOIN_MITER, JOIN_ROUND = 0, 2
CAP_BUTT, CAP_ROUND = 0, 2

MAGIC = 0x6669636E  # 'ficn', written little-endian


def write_coord(out, c):
    """FlatIconFormat.cpp write_coord: 7 bits for small whole numbers,
    else 15 bits of (c + 128) * 102 with the high bit set."""
    c = max(-128.0, min(192.0, c))
    if int(c * 100.0) == int(c) * 100 and -32.0 <= c <= 95.0:
        out.append(int(c + 32.0))
    else:
        v = int((c + 128.0) * 102.0) | 0x8000
        out.append(v >> 8)
        out.append(v & 0xFF)


def write_float24(out, value):
    """FlatIconFormat.cpp write_float_24: 1 sign, 6 exponent, 17 mantissa."""
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    sign = (bits & 0x80000000) >> 31
    exponent = ((bits & 0x7F800000) >> 23) - 127
    mantissa = bits & 0x007FFFFF
    if exponent >= 32 or exponent < -32:
        out += b"\0\0\0"
        return
    short = (sign << 23) | ((exponent + 32) << 17) | (mantissa >> 6)
    out += bytes(((short >> 16) & 0xFF, (short >> 8) & 0xFF, short & 0xFF))


class Icon:
    def __init__(self):
        self.styles = []
        self.paths = []
        self.shapes = []

    # -- styles --
    def solid(self, rgba):
        b = bytearray([STYLE_TYPE_SOLID_COLOR])
        b += bytes(rgba)
        self.styles.append(bytes(b))
        return len(self.styles) - 1

    def _gradient(self, gtype, matrix, stops):
        b = bytearray([STYLE_TYPE_GRADIENT, gtype, GRADIENT_FLAG_TRANSFORM, len(stops)])
        for v in matrix:
            write_float24(b, v)
        for offset, rgba in stops:
            b.append(int(round(offset * 255)))
            b += bytes(rgba)
        self.styles.append(bytes(b))
        return len(self.styles) - 1

    def linear(self, p0, p1, stops):
        """Gradient along p0 -> p1. The renderer runs a linear gradient over
        x = -64..64 in gradient space (IconRenderer.cpp); the matrix maps
        that span onto p0..p1. Matrix order is agg's: sx shy shx sy tx ty."""
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        cx, cy = (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2
        m = (dx / 128.0, dy / 128.0, -dy / 128.0, dx / 128.0, cx, cy)
        return self._gradient(GRADIENT_LINEAR, m, stops)

    def radial(self, center, radius, stops):
        """Circular gradient: radius 0..64 in gradient space."""
        s = radius / 64.0
        return self._gradient(GRADIENT_CIRCULAR, (s, 0.0, 0.0, s, center[0], center[1]), stops)

    # -- paths --
    def polygon(self, pts):
        b = bytearray([PATH_FLAG_CLOSED | PATH_FLAG_NO_CURVES, len(pts)])
        for x, y in pts:
            write_coord(b, x)
            write_coord(b, y)
        self.paths.append(bytes(b))
        return len(self.paths) - 1

    def curve_path(self, pts, closed):
        """pts: (point, control_in, control_out) triples."""
        b = bytearray([PATH_FLAG_CLOSED if closed else 0, len(pts)])
        for p, cin, cout in pts:
            for x, y in (p, cin, cout):
                write_coord(b, x)
                write_coord(b, y)
        self.paths.append(bytes(b))
        return len(self.paths) - 1

    def circle(self, c, r):
        return self.ellipse(c, r, r)

    def ellipse(self, c, rx, ry):
        kx, ky = 0.5523 * rx, 0.5523 * ry
        x, y = c
        return self.curve_path(
            [
                ((x, y - ry), (x - kx, y - ry), (x + kx, y - ry)),
                ((x + rx, y), (x + rx, y - ky), (x + rx, y + ky)),
                ((x, y + ry), (x + kx, y + ry), (x - kx, y + ry)),
                ((x - rx, y), (x - rx, y + ky), (x - rx, y - ky)),
            ],
            True,
        )

    def open_path(self, pts):
        """An open path from (point, in, out) triples (for strokes)."""
        return self.curve_path(pts, False)

    # -- shapes --
    def shape(self, style, paths, stroke=None, translate=None):
        flags = 0
        if stroke:
            flags |= SHAPE_FLAG_HAS_TRANSFORMERS
        if translate:
            flags |= SHAPE_FLAG_TRANSLATION
        b = bytearray([SHAPE_TYPE_PATH_SOURCE, style, len(paths)])
        b += bytes(paths)
        b.append(flags)
        if translate:
            write_coord(b, translate[0])
            write_coord(b, translate[1])
        if stroke:
            width, join, cap = stroke
            b.append(1)  # one transformer
            b += bytes([TRANSFORMER_TYPE_STROKE, int(width + 128), join | (cap << 4), 4])
        self.shapes.append(bytes(b))

    def encode(self):
        for name, items in (("styles", self.styles), ("paths", self.paths), ("shapes", self.shapes)):
            if len(items) > 255:
                raise ValueError("too many " + name)
        out = bytearray(struct.pack("<I", MAGIC))
        for items in (self.styles, self.paths, self.shapes):
            out.append(len(items))
            for it in items:
                out += it
        return bytes(out)


# ---- the look ----------------------------------------------------------------

SHADOW = (0, 0, 0, 70)
GLOSS_TOP = (255, 255, 255, 165)
GLOSS_END = (255, 255, 255, 0)

# (light, dark, outline) per colour family
PALETTES = {
    "green": ((196, 248, 160, 255), (34, 132, 40, 255), (8, 40, 10, 255)),
    "red": ((255, 150, 128, 255), (172, 12, 10, 255), (60, 0, 0, 255)),
    "blue": ((200, 218, 246, 255), (46, 78, 152, 255), (8, 18, 46, 255)),
    "amber": ((255, 226, 140, 255), (214, 118, 8, 255), (64, 30, 0, 255)),
    "steel": ((240, 240, 244, 255), (104, 106, 118, 255), (36, 36, 42, 255)),
    "graphite": ((150, 152, 164, 255), (38, 40, 48, 255), (10, 10, 14, 255)),
    "black": ((112, 114, 124, 255), (14, 14, 18, 255), (0, 0, 0, 255)),
    "paper": ((255, 255, 255, 255), (206, 208, 216, 255), (70, 72, 80, 255)),
}


def solid_glyph(icon, paths, family, bounds):
    """Filled glyph: shadow, outline, lit body, gloss on the upper half."""
    light, dark, outline = PALETTES[family]
    x0, y0, x1, y1 = bounds
    icon.shape(icon.solid(SHADOW), paths, translate=(2, 3))
    icon.shape(icon.solid(outline), paths, stroke=(4, JOIN_ROUND, CAP_ROUND))
    body = icon.linear((x0, y0), (x1, y1), [(0.0, light), (1.0, dark)])
    icon.shape(body, paths)
    mid = y0 + (y1 - y0) * 0.55
    gloss = icon.linear((x0, y0), (x0, mid), [(0.0, GLOSS_TOP), (1.0, GLOSS_END)])
    icon.shape(gloss, paths)


def stroked_glyph(icon, paths, family, bounds, width, heads=None):
    """A thick line glyph (rings, loop arrows): the outline is a wider stroke
    under the coloured stroke; optional filled arrowheads ride on top."""
    light, dark, outline = PALETTES[family]
    x0, y0, x1, y1 = bounds
    shadow = icon.solid(SHADOW)
    icon.shape(shadow, paths, stroke=(width, JOIN_ROUND, CAP_ROUND), translate=(2, 3))
    if heads:
        icon.shape(shadow, heads, translate=(2, 3))
    out = icon.solid(outline)
    icon.shape(out, paths, stroke=(width + 4, JOIN_ROUND, CAP_ROUND))
    if heads:
        icon.shape(out, heads, stroke=(4, JOIN_ROUND, CAP_ROUND))
    body = icon.linear((x0, y0), (x1, y1), [(0.0, light), (1.0, dark)])
    icon.shape(body, paths, stroke=(width, JOIN_ROUND, CAP_ROUND))
    if heads:
        icon.shape(body, heads)
    mid = y0 + (y1 - y0) * 0.5
    gloss = icon.linear((x0, y0), (x0, mid), [(0.0, GLOSS_TOP), (1.0, GLOSS_END)])
    icon.shape(gloss, paths, stroke=(width, JOIN_ROUND, CAP_ROUND))




def half_ellipse_shell(i, cx, top, bottom, rx, ry):
    """The drum shell: sides from the head's rim down, closed by the lower
    half of the bottom ellipse; the top edge is the head's front half."""
    k = 0.5523
    return i.curve_path([
        ((cx - rx, top), (cx - rx, top - k * ry), (cx - rx, top)),
        ((cx - rx, bottom), (cx - rx, bottom), (cx - rx, bottom + k * ry)),
        ((cx, bottom + ry), (cx - k * rx, bottom + ry), (cx + k * rx, bottom + ry)),
        ((cx + rx, bottom), (cx + rx, bottom + k * ry), (cx + rx, bottom)),
        ((cx + rx, top), (cx + rx, top), (cx + rx, top - k * ry)),
        ((cx, top - ry), (cx + k * rx, top - ry), (cx - k * rx, top - ry)),
    ], True)


def stick(i, butt, tip, wb, wt):
    """A tapered drumstick from butt to tip, with an oval bead at the tip."""
    dx, dy = tip[0] - butt[0], tip[1] - butt[1]
    ln = math.hypot(dx, dy)
    ux, uy = dx / ln, dy / ln
    nx, ny = -uy, ux
    # the neck stops short of the bead
    nk = (tip[0] - ux * 3.2, tip[1] - uy * 3.2)
    k = 0.5523
    rb = wb / 2
    body = i.curve_path([
        ((butt[0] + nx * rb, butt[1] + ny * rb), (butt[0] + nx * rb, butt[1] + ny * rb),
         (butt[0] + nx * rb, butt[1] + ny * rb)),
        ((nk[0] + nx * wt / 2, nk[1] + ny * wt / 2), (nk[0] + nx * wt / 2, nk[1] + ny * wt / 2),
         (nk[0] + nx * wt / 2, nk[1] + ny * wt / 2)),
        ((nk[0] - nx * wt / 2, nk[1] - ny * wt / 2), (nk[0] - nx * wt / 2, nk[1] - ny * wt / 2),
         (nk[0] - nx * wt / 2, nk[1] - ny * wt / 2)),
        ((butt[0] - nx * rb, butt[1] - ny * rb), (butt[0] - nx * rb, butt[1] - ny * rb),
         (butt[0] - nx * rb - ux * rb * k * 1.8, butt[1] - ny * rb - uy * rb * k * 1.8)),
        ((butt[0] - ux * rb, butt[1] - uy * rb),
         (butt[0] - ux * rb - nx * rb * k, butt[1] - uy * rb - ny * rb * k),
         (butt[0] - ux * rb + nx * rb * k, butt[1] - uy * rb + ny * rb * k)),
    ], True)
    # bead: an ellipse along the stick
    bc = (tip[0] - ux * 1.6, tip[1] - uy * 1.6)
    ra, rbd = 2.4, 1.7
    bead = i.curve_path([
        ((bc[0] + ux * ra, bc[1] + uy * ra), (bc[0] + ux * ra - nx * rbd * k, bc[1] + uy * ra - ny * rbd * k),
         (bc[0] + ux * ra + nx * rbd * k, bc[1] + uy * ra + ny * rbd * k)),
        ((bc[0] + nx * rbd, bc[1] + ny * rbd), (bc[0] + nx * rbd + ux * ra * k, bc[1] + ny * rbd + uy * ra * k),
         (bc[0] + nx * rbd - ux * ra * k, bc[1] + ny * rbd - uy * ra * k)),
        ((bc[0] - ux * ra, bc[1] - uy * ra), (bc[0] - ux * ra + nx * rbd * k, bc[1] - uy * ra + ny * rbd * k),
         (bc[0] - ux * ra - nx * rbd * k, bc[1] - uy * ra - ny * rbd * k)),
        ((bc[0] - nx * rbd, bc[1] - ny * rbd), (bc[0] - nx * rbd - ux * ra * k, bc[1] - ny * rbd - uy * ra * k),
         (bc[0] - nx * rbd + ux * ra * k, bc[1] - ny * rbd + uy * ra * k)),
    ], True)
    return body, bead


def line(i, a, b):
    return i.open_path([(a, a, a), (b, b, b)])


def arc(i, a, c, b):
    """Open quadratic-ish arc from a to b bulging toward control c."""
    return i.open_path([(a, a, (a[0] + (c[0] - a[0]) * 0.66, a[1] + (c[1] - a[1]) * 0.66)),
                        (b, (b[0] + (c[0] - b[0]) * 0.66, b[1] + (c[1] - b[1]) * 0.66), b)])


def icon_drumku(wood="maple"):
    i = Icon()
    cx, top, bottom, rx, ry = 32, 40, 51, 25, 8.5
    ground = i.radial((cx, 60), 28, [(0.0, (0, 0, 0, 90)), (1.0, (0, 0, 0, 0))])
    i.shape(ground, [i.ellipse((cx, 60), 28, 3.5)])

    # Shell: glossy black, chrome rims, a row of lugs.
    shell = half_ellipse_shell(i, cx, top, bottom, rx, ry)
    head = i.ellipse((cx, top), rx, ry)
    i.shape(i.solid(SHADOW), [shell, head], translate=(2, 3))
    i.shape(i.solid((0, 0, 0, 255)), [shell, head], stroke=(3, JOIN_ROUND, CAP_ROUND))
    i.shape(i.linear((cx - rx, 0), (cx + rx, 0),
                     [(0.0, (70, 72, 82, 255)), (0.35, (16, 16, 20, 255)), (0.7, (30, 31, 38, 255)),
                      (1.0, (8, 8, 10, 255))]), [shell])
    # bottom rim
    k = 0.5523
    brim = i.open_path([((cx - rx, bottom), (cx - rx, bottom), (cx - rx, bottom + k * ry)),
                        ((cx, bottom + ry), (cx - k * rx, bottom + ry), (cx + k * rx, bottom + ry)),
                        ((cx + rx, bottom), (cx + rx, bottom + k * ry), (cx + rx, bottom))])
    chrome = i.linear((cx - rx, 0), (cx + rx, 0),
                      [(0.0, (250, 250, 255, 255)), (0.5, (150, 152, 162, 255)), (1.0, (230, 232, 240, 255))])
    i.shape(chrome, [brim], stroke=(2, JOIN_ROUND, CAP_ROUND))
    # lugs on the front of the shell
    lug = i.solid((200, 202, 212, 255))
    lug_dark = i.solid((0, 0, 0, 255))
    for lx in (-17, -6, 6, 17):
        x = cx + lx
        yb = top + ry * math.sqrt(max(0.0, 1 - (lx / rx) ** 2))
        ym = yb + (bottom - top) * 0.5
        seg = [line(i, (x, ym - 1.6), (x, ym + 1.6))]
        i.shape(lug_dark, seg, stroke=(3.6, JOIN_ROUND, CAP_ROUND))
        i.shape(lug, seg, stroke=(2, JOIN_ROUND, CAP_ROUND))

    # Head: white, lit from the top left; chrome hoop.
    i.shape(i.radial((cx - 7, top - 3), 26, [(0.0, (255, 255, 255, 255)), (0.7, (226, 228, 234, 255)),
                                               (1.0, (176, 178, 190, 255))]), [head])
    i.shape(chrome, [head], stroke=(2.5, JOIN_ROUND, CAP_ROUND))

    # The roll: each tip strikes the head, sparks bursting out of the hit.
    hits = [(24, 38), (40, 38)]
    sparks = []
    for (hx, hy), side in zip(hits, (-1, 1)):
        # away from the stick (which comes in from the upper outside)
        for ang, r0, r1 in ((190, 5, 11), (212, 5, 10.5), (282, 4.5, 11.5), (305, 4.5, 10.5)):
            t = math.radians(ang if side < 0 else 180 - ang)
            sparks.append(line(i, (hx + r0 * math.cos(t), hy + r0 * math.sin(t)),
                                  (hx + r1 * math.cos(t), hy + r1 * math.sin(t))))
    # Sticks: crossing, tips on the head.
    woods = {
        "maple": ((250, 232, 196, 255), (196, 160, 104, 255), (70, 48, 20, 255)),
        "black": ((120, 122, 132, 255), (16, 16, 20, 255), (0, 0, 0, 255)),
    }
    light, dark, outline = woods[wood]
    sticks = [((6, 6), (24, 38)), ((58, 6), (40, 38))]
    for butt, tip in sticks:
        body, bead = stick(i, butt, tip, 5.2, 2.4)
        i.shape(i.solid(SHADOW), [body, bead], translate=(2, 2))
        i.shape(i.solid(outline), [body, bead], stroke=(2.5, JOIN_ROUND, CAP_ROUND))
        nx = -(tip[1] - butt[1]); ny = tip[0] - butt[0]; ln = math.hypot(nx, ny); nx /= ln; ny /= ln
        g = i.linear((butt[0] - nx * 2.3, butt[1] - ny * 2.3), (butt[0] + nx * 2.3, butt[1] + ny * 2.3),
                     [(0.0, light), (1.0, dark)])
        i.shape(g, [body, bead])

    i.shape(i.solid((8, 40, 10, 255)), sparks, stroke=(3.2, JOIN_ROUND, CAP_ROUND))
    i.shape(i.solid((120, 255, 140, 255)), sparks, stroke=(1.6, JOIN_ROUND, CAP_ROUND))
    return i



def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outdir = os.path.join(root, "standalone", "icons")
    os.makedirs(outdir, exist_ok=True)
    data = icon_drumku().encode()
    with open(os.path.join(outdir, "drumku.hvif"), "wb") as f:
        f.write(data)
    rdef = [
        "// Generated by tools/make_icon.py; edit the script (or drumku.hvif in",
        "// Icon-O-Matic and convert it back) instead.",
        "resource vector_icon {",
    ]
    for k in range(0, len(data), 32):
        rdef.append('\t$"%s"' % data[k:k + 32].hex().upper())
    rdef.append("};")
    rdef.append("")
    with open(os.path.join(outdir, "drumku_icon.rdef"), "w") as f:
        f.write("\n".join(rdef))
    print("drumku icon: %d bytes" % len(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
