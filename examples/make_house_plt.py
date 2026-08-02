# Copyright (C) 2026 Leonid Astakhov
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version. See the LICENSE file for details.

"""Generate an A3 house drawing as native HP-GL/2, for testing the raw queue.

Everything is drawn in millimetres via an isotropic SC mapping. Isotropic
matters here: this plotter puts its X axis along the *longer* edge of the
plot, so if the axis mapping comes out the other way round the drawing is
rotated rather than stretched or clipped - which makes a first test cheap
to interpret instead of ruining the sheet.
"""

MM = 40  # plotter units per mm (1016 per inch)
PAGE_W, PAGE_H = 297.0, 420.0        # A3 portrait
M_SIDE, M_TOP, M_BOT = 5.0, 12.0, 15.0   # measured printable margins

out = []


def cmd(s):
    out.append(s)


def pu(x, y):
    cmd("PU%.1f,%.1f;" % (x, y))


def pd(*pts):
    cmd("PD" + ",".join("%.1f,%.1f" % p for p in pts) + ";")


def poly(pts, close=False):
    pu(*pts[0])
    seq = list(pts[1:])
    if close:
        seq.append(pts[0])
    pd(*seq)


def rect(x, y, w, h):
    poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], close=True)


def label(x, y, text, size=3.0):
    """size = capital height in mm; SI takes width,height in cm."""
    cmd("SI%.3f,%.3f;" % (size * 0.6 / 10.0, size / 10.0))
    pu(x, y)
    cmd("LB%s\x03" % text)   # ETX is the default label terminator


def pen(n, width_mm):
    cmd("SP%d;" % n)
    cmd("PW%.2f;" % width_mm)


# ---------------------------------------------------------------- header
cmd("IN;")
cmd("BP5,1;")                                  # no auto-rotate
cmd("PS%d,%d;" % (PAGE_H * MM, PAGE_W * MM))   # PS is length,width
# This plotter puts its X axis along the longer edge of the plot, so a
# portrait drawing comes out turned 90 degrees clockwise. RO rotates the
# coordinate system anticlockwise to cancel that. P1/P2 keep their old
# coordinates through a rotation and end up off the page, so IP has to
# follow RO to put them back on the hard-clip corners before SC maps
# millimetres onto them.
cmd("RO90;")
cmd("IP;")
cmd("SC0,%.0f,0,%.0f,1;" % (PAGE_W, PAGE_H))   # isotropic mm mapping
cmd("TR0;")
cmd("LO1;")                                    # label origin: lower left

L = M_SIDE                     # usable box in mm
R = PAGE_W - M_SIDE
B = M_BOT
T = PAGE_H - M_TOP

# ------------------------------------------------------------ page frame
pen(1, 0.7)
rect(L, B, R - L, T - B)

# --------------------------------------------------------- front elevation
# Drawn to a 1:50 feel: 10 m wide house across ~200 mm of paper.
ex, ey = 45.0, 250.0           # origin of the elevation, lower-left
sc = 20.0                      # mm of paper per metre of building

pen(1, 0.5)
label(L + 6, T - 12, "FRONT ELEVATION   1:50", 4.5)

pen(2, 0.5)
# ground line
pu(ex - 15, ey)
pd((ex + 10 * sc + 15, ey))

pen(1, 0.6)
# walls: 10 m wide, 3 m to eaves
rect(ex, ey, 10 * sc, 3 * sc)
# roof: ridge 2 m above eaves, 0.4 m overhang each side
poly([(ex - 0.4 * sc, ey + 3 * sc),
      (ex + 5 * sc, ey + 5 * sc),
      (ex + 10.4 * sc, ey + 3 * sc)])

pen(3, 0.35)
# door, 1 m x 2.1 m, centred
dx = ex + 4.5 * sc
rect(dx, ey, 1 * sc, 2.1 * sc)
pu(dx + 0.85 * sc, ey + 1.05 * sc)
cmd("CI%.2f;" % (0.06 * sc))   # door handle

# two windows, 1.4 m x 1.2 m, sill at 1 m
for wx in (ex + 1.2 * sc, ex + 7.4 * sc):
    rect(wx, ey + 1.0 * sc, 1.4 * sc, 1.2 * sc)
    pu(wx + 0.7 * sc, ey + 1.0 * sc)
    pd((wx + 0.7 * sc, ey + 2.2 * sc))
    pu(wx, ey + 1.6 * sc)
    pd((wx + 1.4 * sc, ey + 1.6 * sc))

# chimney
pen(1, 0.6)
rect(ex + 7.6 * sc, ey + 3.9 * sc, 0.7 * sc, 1.6 * sc)

# dimension line under the elevation
pen(4, 0.25)
dy = ey - 12
pu(ex, dy); pd((ex + 10 * sc, dy))
for t in (ex, ex + 10 * sc):
    pu(t, dy - 2.5); pd((t, dy + 2.5))
label(ex + 4.2 * sc, dy + 2, "10 000", 3.0)

# ------------------------------------------------------------- floor plan
px, py = 45.0, 70.0
pen(1, 0.5)
label(L + 6, 215, "GROUND FLOOR PLAN   1:50", 4.5)

pen(1, 0.6)
rect(px, py, 10 * sc, 7 * sc)                  # outer wall
pen(3, 0.35)
rect(px + 0.3 * sc, py + 0.3 * sc, 9.4 * sc, 6.4 * sc)   # inner face

# internal partitions
pen(2, 0.45)
pu(px + 5.5 * sc, py + 0.3 * sc); pd((px + 5.5 * sc, py + 6.7 * sc))
pu(px + 0.3 * sc, py + 4.0 * sc); pd((px + 5.5 * sc, py + 4.0 * sc))

# door openings with swing arcs
pen(4, 0.25)
# AA sweeps an arc from the current point around the given centre - the pen
# has to be DOWN or it just tracks the arc without drawing it.
pu(px + 5.5 * sc, py + 1.6 * sc + 0.9 * sc)
cmd("PD;")
cmd("AA%.1f,%.1f,90;" % (px + 5.5 * sc, py + 1.6 * sc))
cmd("PU;")
pu(px + 2.4 * sc + 0.9 * sc, py + 4.0 * sc)
cmd("PD;")
cmd("AA%.1f,%.1f,90;" % (px + 2.4 * sc, py + 4.0 * sc))
cmd("PU;")

pen(1, 0.3)
label(px + 1.0 * sc, py + 5.2 * sc, "LIVING", 3.5)
label(px + 1.0 * sc, py + 1.8 * sc, "KITCHEN", 3.5)
label(px + 6.4 * sc, py + 5.2 * sc, "BEDROOM 1", 3.5)
label(px + 6.4 * sc, py + 1.8 * sc, "BEDROOM 2", 3.5)

# plan dimensions
pen(4, 0.25)
dy = py - 12
pu(px, dy); pd((px + 10 * sc, dy))
for t in (px, px + 5.5 * sc, px + 10 * sc):
    pu(t, dy - 2.5); pd((t, dy + 2.5))
label(px + 2.2 * sc, dy + 2, "5 500", 3.0)
label(px + 7.2 * sc, dy + 2, "4 500", 3.0)

dxx = px - 12
pu(dxx, py); pd((dxx, py + 7 * sc))
for t in (py, py + 7 * sc):
    pu(dxx - 2.5, t); pd((dxx + 2.5, t))
cmd("DI0,1;")                                  # rotate text 90 degrees
label(dxx + 2, py + 2.6 * sc, "7 000", 3.0)
cmd("DI1,0;")

# ------------------------------------------------------------- title block
pen(1, 0.5)
tb_h, tb_w = 26.0, 110.0
tbx, tby = R - tb_w, B
rect(tbx, tby, tb_w, tb_h)
pu(tbx, tby + 13); pd((tbx + tb_w, tby + 13))
pu(tbx + 70, tby); pd((tbx + 70, tby + tb_h))

pen(1, 0.3)
label(tbx + 3, tby + 17, "DETACHED HOUSE - TEST PLOT", 3.6)
label(tbx + 3, tby + 4.5, "HP-GL/2 raw queue", 3.0)
label(tbx + 73, tby + 17, "A3  1:50", 3.6)
label(tbx + 73, tby + 4.5, "SHEET 1/1", 3.0)

# orientation key, so a rotated result is obvious at a glance
pen(1, 0.3)
label(L + 6, T - 22, "^ this edge is the TOP of the sheet", 3.0)

# ------------------------------------------------------------------ finish
cmd("PU;")
cmd("SP0;")
cmd("PG;")

import os
path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "house_a3.plt")
data = "".join(out)
open(path, "w").write(data)
print("wrote %s (%d bytes, %d commands)" % (path, len(data), len(out)))
