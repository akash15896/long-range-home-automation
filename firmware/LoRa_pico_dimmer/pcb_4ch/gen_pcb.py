"""LoRa Pico dimmer node, 4-channel carrier board (rev A) -> Gerbers + Excellon drill + preview PNG.

Pure Python, no libraries. Run:  python gen_pcb.py
Output goes to ./gerbers/ and ./LoRa_pico_dimmer_4ch_rev_A_gerbers.zip

Board: 108 x 84 mm, 2 layers, 1.6 mm FR4, 1 oz.
  - Raspberry Pi Pico (on 2 x 20 female headers), USB end at the right edge
  - Ai-Thinker Ra-02 (SX1278, 433 MHz) soldered straight onto the board, IPEX antenna lead
  - Hi-Link HLK-10M05 (5 V 2 A) with fuse + varistor on the AC side
  - J3: 7-pin header to a 4-channel RobotDyn-style AC dimmer module (3V3, GND, ZC, CH1-CH4)
  - Reset button (on board) + 2-pin header for the panel reset button, LED header, spare button header
Coordinates below are in mm, Y up, before the (OX, OY) offset is added.
"""
import math, os, zlib, struct, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "gerbers")
NAME = "LoRa_pico_dimmer_4ch"
BW, BH = 108.0, 84.0
OX, OY = 4.0, 4.0
def P(x, y): return (round(x + OX, 4), round(y + OY, 4))

CLR = 0.20          # general copper clearance checked by the DRC
POUR_CLR = 0.40     # bottom GND pour clearance to other nets
MAINS_CLR = 8.0     # mains copper to any low-voltage copper
LN_CLR = 2.5        # L to N
EDGE_CLR = 0.40
VIA_D, VIA_DRILL = 0.6, 0.3
MAINS = {"AC_L", "AC_LF", "AC_N"}

pads = []    # dict(ref, pin, net, x, y, shape 'C'|'R', w, h, drill, layers)
tracks = []  # dict(net, layer 'F'|'B', x1,y1,x2,y2, w)
vias = []    # dict(net, x, y)
silk = []    # ('seg', x1,y1,x2,y2,w)
npth = []    # (x, y, d)

def pad(ref, pin, net, x, y, shape="C", w=1.7, h=None, drill=1.0, smd=False):
    X, Y = P(x, y)
    pads.append(dict(ref=ref, pin=str(pin), net=net or f"{ref}.{pin}", x=X, y=Y, shape=shape,
                     w=w, h=h if h is not None else w, drill=None if smd else drill,
                     layers={"F"} if smd else {"F", "B"}))

def route(net, layer, w, *pts):
    pts = [P(*p) for p in pts]
    for a, b in zip(pts, pts[1:]):
        tracks.append(dict(net=net, layer=layer, x1=a[0], y1=a[1], x2=b[0], y2=b[1], w=w))

def via(net, x, y):
    X, Y = P(x, y)
    vias.append(dict(net=net, x=X, y=Y))

# ---------------------------------------------------------------- font (stroke, 4 x 6 grid)
FONT = {
 "0": [[(0,0),(4,0),(4,6),(0,6),(0,0),(4,6)]], "1": [[(1,5),(2,6),(2,0)],[(1,0),(3,0)]],
 "2": [[(0,6),(4,6),(4,3),(0,3),(0,0),(4,0)]], "3": [[(0,6),(4,6),(4,0),(0,0)],[(1,3),(4,3)]],
 "4": [[(0,6),(0,3),(4,3)],[(4,6),(4,0)]], "5": [[(4,6),(0,6),(0,3),(4,3),(4,0),(0,0)]],
 "6": [[(4,6),(0,6),(0,0),(4,0),(4,3),(0,3)]], "7": [[(0,6),(4,6),(1,0)]],
 "8": [[(0,0),(4,0),(4,6),(0,6),(0,0)],[(0,3),(4,3)]], "9": [[(4,3),(0,3),(0,6),(4,6),(4,0),(0,0)]],
 "A": [[(0,0),(0,4),(2,6),(4,4),(4,0)],[(0,3),(4,3)]],
 "B": [[(0,0),(0,6),(3,6),(4,5),(4,4),(3,3),(0,3)],[(3,3),(4,2),(4,1),(3,0),(0,0)]],
 "C": [[(4,6),(0,6),(0,0),(4,0)]], "D": [[(0,0),(0,6),(3,6),(4,5),(4,1),(3,0),(0,0)]],
 "E": [[(4,6),(0,6),(0,0),(4,0)],[(0,3),(3,3)]], "F": [[(4,6),(0,6),(0,0)],[(0,3),(3,3)]],
 "G": [[(4,6),(0,6),(0,0),(4,0),(4,3),(2,3)]], "H": [[(0,0),(0,6)],[(4,0),(4,6)],[(0,3),(4,3)]],
 "I": [[(1,6),(3,6)],[(2,6),(2,0)],[(1,0),(3,0)]], "J": [[(4,6),(4,0),(0,0),(0,2)]],
 "K": [[(0,0),(0,6)],[(4,6),(0,3),(4,0)]], "L": [[(0,6),(0,0),(4,0)]],
 "M": [[(0,0),(0,6),(2,3),(4,6),(4,0)]], "N": [[(0,0),(0,6),(4,0),(4,6)]],
 "O": [[(0,0),(4,0),(4,6),(0,6),(0,0)]], "P": [[(0,0),(0,6),(4,6),(4,3),(0,3)]],
 "Q": [[(0,0),(4,0),(4,6),(0,6),(0,0)],[(2,2),(4,0)]], "R": [[(0,0),(0,6),(4,6),(4,3),(0,3),(4,0)]],
 "S": [[(4,5),(3,6),(1,6),(0,5),(0,4),(1,3),(3,3),(4,2),(4,1),(3,0),(1,0),(0,1)]],
 "T": [[(0,6),(4,6)],[(2,6),(2,0)]], "U": [[(0,6),(0,0),(4,0),(4,6)]], "V": [[(0,6),(2,0),(4,6)]],
 "W": [[(0,6),(1,0),(2,3),(3,0),(4,6)]], "X": [[(0,0),(4,6)],[(0,6),(4,0)]],
 "Y": [[(0,6),(2,3),(4,6)],[(2,3),(2,0)]], "Z": [[(0,6),(4,6),(0,0),(4,0)]],
 "+": [[(0,3),(4,3)],[(2,1),(2,5)]], "-": [[(1,3),(3,3)]], ".": [[(2,0),(2,0.4)]],
 "/": [[(0,0),(4,6)]], "(": [[(3,6),(2,5),(2,1),(3,0)]], ")": [[(1,6),(2,5),(2,1),(1,0)]],
 ":": [[(2,1),(2,1.5)],[(2,4.5),(2,5)]], "~": [[(0,3),(1,4),(3,2),(4,3)]],
 "!": [[(2,6),(2,2)],[(2,0.4),(2,0)]], "=": [[(0,2),(4,2)],[(0,4),(4,4)]],
 ">": [[(0,6),(4,3),(0,0)]], "<": [[(4,6),(0,3),(4,0)]], "_": [[(0,0),(4,0)]],
}

def text(s, x, y, h=1.2, w=0.15, rot=0, anchor="l"):
    """Silkscreen text; (x, y) is the baseline start (design coords). rot 0 or 90 (reads bottom-up)."""
    k = h / 6.0
    adv = 6 * k
    total = len(s) * adv - 2 * k
    off = {"l": 0, "c": -total / 2, "r": -total}[anchor]
    for i, ch in enumerate(s.upper()):
        for stroke in FONT.get(ch, []):
            pts = []
            for (u, v) in stroke:
                lx, ly = off + i * adv + u * k, v * k
                if rot == 90:
                    lx, ly = -ly, lx
                pts.append(P(x + lx, y + ly))
            if len(pts) == 1:
                pts.append(pts[0])
            for a, b in zip(pts, pts[1:]):
                silk.append(("seg", a[0], a[1], b[0], b[1], w))

def sline(w, *pts):
    pts = [P(*p) for p in pts]
    for a, b in zip(pts, pts[1:]):
        silk.append(("seg", a[0], a[1], b[0], b[1], w))

def srect(x1, y1, x2, y2, w=0.15):
    sline(w, (x1, y1), (x2, y1), (x2, y2), (x1, y2), (x1, y1))

def scircle(cx, cy, r, w=0.15, n=36):
    pts = [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]
    sline(w, *pts)

# ---------------------------------------------------------------- parts
# U1 Raspberry Pi Pico, rotated so USB points right (+x). Pin 1/40 at the USB end.
PCX, PCY = 74.5, 61.0
def pico_xy(p):
    if p <= 20:
        return PCX + 24.13 - (p - 1) * 2.54, PCY + 8.89
    return PCX - 24.13 + (p - 21) * 2.54, PCY - 8.89
PICO_NETS = {3: "GND", 8: "GND", 13: "GND", 18: "GND", 23: "GND", 28: "GND", 33: "GND", 38: "GND",
             17: "CH4", 19: "BTN", 20: "LED_GP",
             21: "MISO", 22: "NSS", 24: "SCK", 25: "MOSI", 26: "DIO0", 27: "LRST",
             29: "CH3", 30: "RUN", 31: "ZC", 32: "CH1", 34: "CH2", 36: "3V3", 39: "VSYS"}
for p in range(1, 41):
    x, y = pico_xy(p)
    pad("U1", p, PICO_NETS.get(p), x, y, "R" if p == 1 else "C", 1.7, drill=1.0)
srect(PCX - 25.5, PCY - 10.5, PCX + 25.5, PCY + 10.5)
srect(PCX + 25.5 - 5.5, PCY - 4.0, PCX + 25.5 + 0.8, PCY + 4.0)        # USB socket
text("RASPBERRY PI PICO", PCX - 6, PCY + 2.2, 1.6, 0.2, anchor="c")
text("USB >", PCX + 17.6, PCY - 0.6, 1.4, 0.2, anchor="r")
text("U1", PCX - 22.5, PCY + 2.2, 1.4, 0.2)
text("1", pico_xy(1)[0] - 0.5, PCY + 11.0, 1.0)

# U2 Ai-Thinker Ra-02, rotated 180 deg (pin 1 bottom-right). 2.0 mm pitch, pads 2.5 x 1.0.
RCX, RCY = 60.0, 36.0
RA_NETS = {1: "GND", 2: "GND", 3: "3V3", 4: "LRST", 5: "DIO0", 9: "GND",
           12: "SCK", 13: "MISO", 14: "MOSI", 15: "NSS", 16: "GND"}
def ra_xy(n):
    if n <= 8:   # original left side, top to bottom -> after 180 deg: right side, bottom to top
        lx, ly = -8.25, 7 - (n - 1) * 2
    else:        # original right side, bottom to top -> left side, top to bottom
        lx, ly = 8.25, -7 + (n - 9) * 2
    return RCX - lx, RCY - ly
for n in range(1, 17):
    x, y = ra_xy(n)
    pad("U2", n, RA_NETS.get(n), x, y, "R", 2.5, 1.0, smd=True)
srect(RCX - 8.5, RCY - 8.0, RCX + 8.5, RCY + 8.0)
text("RA-02", RCX, RCY + 2.0, 1.6, 0.2, anchor="c")
text("SX1278 433M", RCX, RCY - 1.0, 1.0, 0.15, anchor="c")
text("U2", RCX, RCY - 4.5, 1.2, 0.15, anchor="c")
scircle(ra_xy(1)[0] + 2.2, ra_xy(1)[1], 0.35, 0.2, 12)     # pin-1 dot
text("1", ra_xy(1)[0] + 2.0, ra_xy(1)[1] - 2.4, 1.0)

# PS1 Hi-Link HLK-10M05 (datasheet V1.5: body 46.9 x 27.8, pins 0.8 mm). Body corner at (2, 2).
HX, HY = 2.0, 2.0
pad("PS1", 1, "AC_N", HX + 2.2, HY + 17.8, "C", 2.6, drill=1.3)
pad("PS1", 2, "AC_LF", HX + 2.2, HY + 10.0, "C", 2.6, drill=1.3)
pad("PS1", 3, "GND", HX + 44.7, HY + 25.3, "C", 2.6, drill=1.3)
pad("PS1", 4, "5V", HX + 44.7, HY + 2.5, "C", 2.6, drill=1.3)
srect(HX, HY, HX + 46.9, HY + 27.8, 0.2)
text("HLK-10M05", HX + 25.0, HY + 15.5, 2.0, 0.25, anchor="c")
text("PS1  5V 2A", HX + 25.0, HY + 11.5, 1.4, 0.2, anchor="c")
text("AC", HX + 5.0, HY + 13.4, 1.2, 0.15)
text("-VO", HX + 37.8, HY + 24.7, 1.2, 0.15)
text("+VO", HX + 37.8, HY + 1.9, 1.2, 0.15)

# J1 mains input, 2-pin 5.08 mm screw terminal (wire entry at the left edge)
pad("J1", 1, "AC_L", 5.0, 70.0, "R", 2.4, drill=1.3)
pad("J1", 2, "AC_N", 5.0, 64.92, "C", 2.4, drill=1.3)
srect(0.6, 61.4, 9.0, 73.5)
text("L", 1.2, 69.3, 1.4, 0.2)
text("N", 1.2, 64.2, 1.4, 0.2)
text("J1 AC IN", 0.8, 74.6, 1.2, 0.15)
# F1 fuse, radial 5.08 mm (TR5 / 1 A slow)
pad("F1", 1, "AC_L", 12.5, 70.0, "C", 2.2, drill=1.0)
pad("F1", 2, "AC_LF", 12.5, 64.92, "C", 2.2, drill=1.0)
scircle(12.5, 67.46, 4.2)
text("F1", 17.2, 69.0, 1.2, 0.15)
text("T1A", 17.2, 66.6, 1.2, 0.15)
# RV1 varistor 10D561K, 7.5 mm pitch
pad("RV1", 1, "AC_N", 5.0, 42.0, "C", 2.2, drill=1.0)
pad("RV1", 2, "AC_LF", 12.5, 42.0, "C", 2.2, drill=1.0)
srect(3.2, 39.6, 14.3, 44.4)
text("RV1 10D561K", 2.4, 45.6, 1.2, 0.15)

# D1 1N5817 (5 V -> VSYS, so USB and the Hi-Link can both be connected). DO-41, 10.16 mm.
pad("D1", "A", "5V", 52.0, 4.5, "C", 2.2, drill=1.1)
pad("D1", "K", "VSYS", 62.16, 4.5, "R", 2.2, drill=1.1)
srect(54.4, 3.1, 59.8, 5.9)
sline(0.4, (58.8, 3.2), (58.8, 5.8))
text("D1 1N5817", 54.0, 6.8, 1.2, 0.15)
# C1 470 uF 10 V radial, 3.5 mm pitch, 8 mm can
pad("C1", "+", "5V", 54.25, 13.0, "R", 1.8, drill=0.9)
pad("C1", "-", "GND", 57.75, 13.0, "C", 1.8, drill=0.9)
scircle(56.0, 13.0, 4.2)
text("+", 52.3, 15.6, 1.2, 0.15)
text("C1 470U", 51.6, 18.0, 1.2, 0.15)

# C3 10 uF, C4 100 nF, 1206, next to the Ra-02 supply pin
for ref, x, val in [("C3", 67.6, "10U"), ("C4", 70.6, "100N")]:
    pad(ref, 1, "3V3", x, 24.05, "R", 1.8, 1.2, smd=True)
    pad(ref, 2, "GND", x, 20.95, "R", 1.8, 1.2, smd=True)
text("C3", 66.6, 18.2, 1.0, 0.15)
text("C4", 69.6, 18.2, 1.0, 0.15)
text("10U 100N", 65.8, 16.6, 1.0, 0.15)

# R1 330R 1206 (GP15 -> LED header)
pad("R1", 1, "LEDA", 43.45, 69.89, "R", 1.2, 1.8, smd=True)
pad("R1", 2, "LED_GP", 46.55, 69.89, "R", 1.2, 1.8, smd=True)
text("R1 330R", 42.4, 71.6, 1.0, 0.15)
# J6 panel LED (+ / -), J7 spare button (GP14 / GND), 2.54 mm headers
pad("J6", 1, "LEDA", 40.0, 69.89, "R", 1.7, drill=1.0)
pad("J6", 2, "GND", 40.0, 67.35, "C", 1.7, drill=1.0)
pad("J7", 1, "BTN", 40.0, 64.81, "R", 1.7, drill=1.0)
pad("J7", 2, "GND", 40.0, 62.27, "C", 1.7, drill=1.0)
srect(38.7, 66.0, 41.3, 71.2)
srect(38.7, 60.9, 41.3, 66.1)
text("J6 LED", 28.8, 70.4, 1.1, 0.15)
text("+ -", 32.0, 67.6, 1.1, 0.15)
text("J7 BTN", 28.8, 64.2, 1.1, 0.15)
text("GP14 GND", 28.8, 61.8, 1.0, 0.15)

# J3 dimmer module header, 1 x 7, 2.54 mm, at the right edge
J3X = 98.0
J3 = [("3V3", 42.0), ("GND", 39.46), ("ZC", 36.92), ("CH1", 34.38), ("CH2", 31.84), ("CH3", 29.30), ("CH4", 26.76)]
for i, (net, y) in enumerate(J3):
    pad("J3", i + 1, net, J3X, y, "R" if i == 0 else "C", 1.7, drill=1.0)
    text(net, J3X + 1.5, y - 0.5, 1.0, 0.15)
srect(J3X - 1.3, 25.4, J3X + 1.3, 43.4)
text("J3 DIMMER", J3X - 6.2, 44.4, 1.2, 0.15)

# J5 panel reset button (RUN / GND), SW1 on-board reset (6 x 6 tactile, diagonal pins used)
pad("J5", 1, "RUN", 77.0, 11.75, "R", 1.7, drill=1.0)
pad("J5", 2, "GND", 77.0, 14.29, "C", 1.7, drill=1.0)
srect(75.7, 10.4, 78.3, 15.6)
text("J5", 75.4, 16.6, 1.0, 0.15)
text("RESET", 74.4, 8.4, 1.0, 0.15)
pad("SW1", 1, "RUN", 81.75, 11.75, "C", 2.0, drill=1.1)
pad("SW1", 2, None, 88.25, 11.75, "C", 2.0, drill=1.1)
pad("SW1", 3, None, 81.75, 16.25, "C", 2.0, drill=1.1)
pad("SW1", 4, "GND", 88.25, 16.25, "C", 2.0, drill=1.1)
srect(82.0, 11.0, 88.0, 17.0)
scircle(85.0, 14.0, 1.7)
text("SW1 RST", 81.2, 18.2, 1.0, 0.15)

# Mounting holes, M3 (final coordinates)
for (x, y) in [(3.2, 3.2), (BW - 3.2, 3.2), (3.2, BH - 3.2), (BW - 3.2, BH - 3.2)]:
    npth.append((x, y, 3.2))

# ---------------------------------------------------------------- routing
S, PW, VW, MW = 0.30, 0.50, 0.80, 1.50
pico = {p: pico_xy(p) for p in range(1, 41)}
def ra(n): return ra_xy(n)

# LoRa SPI: Pico drops on top, cross-overs on the bottom, lanes beside the Ra-02 on top
for net, pin, lane_y, lane_x, ra_pin in [("MISO", 21, 50.0, 49.0, 13), ("NSS", 22, 49.2, 47.4, 15),
                                          ("SCK", 24, 47.6, 49.8, 12), ("MOSI", 25, 48.4, 48.2, 14)]:
    px, py = pico[pin]
    rx, ry = ra(ra_pin)
    route(net, "F", S, (px, py), (px, lane_y)); via(net, px, lane_y)
    route(net, "B", S, (px, lane_y), (lane_x, lane_y)); via(net, lane_x, lane_y)
    route(net, "F", S, (lane_x, lane_y), (lane_x, ry), (rx, ry))
for net, pin, lane_y, lane_x, ra_pin in [("DIO0", 26, 49.2, 70.3, 5), ("LRST", 27, 48.4, 71.1, 4)]:
    px, py = pico[pin]
    rx, ry = ra(ra_pin)
    route(net, "F", S, (px, py), (px, lane_y)); via(net, px, lane_y)
    route(net, "B", S, (px, lane_y), (lane_x, lane_y)); via(net, lane_x, lane_y)
    route(net, "F", S, (lane_x, lane_y), (lane_x, ry), (rx, ry))

# 3V3: Pico pin 36 -> Ra-02 pin 3 + C3/C4, and -> J3 pin 1
px, py = pico[36]
route("3V3", "F", PW, (px, py), (px, 42.0))
via("3V3", px, 46.2); via("3V3", px, 42.0)
route("3V3", "B", PW, (px, 46.2), (71.9, 46.2)); via("3V3", 71.9, 46.2)
route("3V3", "F", PW, (71.9, 46.2), (71.9, 33.0), ra(3))
route("3V3", "F", PW, (71.9, 33.0), (71.9, 24.05), (67.6, 24.05))
route("3V3", "B", PW, (px, 42.0), (J3X, 42.0))

# Dimmer signals: drop on top, via, bottom straight to J3
J3Y = dict(J3)
for net, pin in [("ZC", 31), ("CH1", 32), ("CH2", 34)]:
    px, py = pico[pin]
    route(net, "F", S, (px, py), (px, J3Y[net])); via(net, px, J3Y[net])
    route(net, "B", S, (px, J3Y[net]), (J3X, J3Y[net]))
px, py = pico[29]                                              # CH3 = GP22, jog right first
route("CH3", "F", S, (px, py), (px, 50.8)); via("CH3", px, 50.8)
route("CH3", "B", S, (px, 50.8), (74.5, 50.8)); via("CH3", 74.5, 50.8)
route("CH3", "F", S, (74.5, 50.8), (74.5, J3Y["CH3"])); via("CH3", 74.5, J3Y["CH3"])
route("CH3", "B", S, (74.5, J3Y["CH3"]), (J3X, J3Y["CH3"]))
px, py = pico[17]                                              # CH4 = GP13, from the far pin row
route("CH4", "F", 0.25, (px, py), (px, 56.0), (56.72, 54.72), (56.72, 45.3)); via("CH4", 56.72, 45.3)
route("CH4", "B", S, (56.72, 45.3), (77.04, 45.3)); via("CH4", 77.04, 45.3)
route("CH4", "F", S, (77.04, 45.3), (77.04, J3Y["CH4"])); via("CH4", 77.04, J3Y["CH4"])
route("CH4", "B", S, (77.04, J3Y["CH4"]), (J3X, J3Y["CH4"]))

# RUN -> J5 -> SW1
px, py = pico[30]
route("RUN", "F", S, (px, py), (px, 11.75), (81.75, 11.75))

# Power: HLK +Vo -> D1 -> VSYS (Pico pin 39); C1 on the 5 V side
route("5V", "F", 1.0, (46.7, 4.5), (52.0, 4.5))
route("5V", "F", VW, (49.6, 4.5), (49.6, 13.0), (54.25, 13.0))
px, py = pico[39]
route("VSYS", "F", VW, (62.16, 4.5), (px, 4.5), (px, py))

# GND stubs from the SMD pads to vias (bottom layer is a GND pour)
route("GND", "F", S, ra(1), (70.5, 29.0), (70.5, 31.0), ra(2)); via("GND", 70.5, 30.0)
route("GND", "F", S, ra(16), (49.6, 29.0)); via("GND", 49.6, 29.0)
route("GND", "F", S, ra(9), (ra(9)[0], 45.0)); via("GND", ra(9)[0], 45.0)
route("GND", "F", S, (67.6, 20.95), (70.6, 20.95)); via("GND", 69.1, 20.95)

# LED and spare button
route("LED_GP", "F", S, pico[20], (46.55, 69.89))
route("LEDA", "F", S, (43.45, 69.89), (40.0, 69.89))
route("BTN", "F", S, pico[19], (pico[19][0], 64.81), (40.0, 64.81))

# Mains (top layer, 1.5 mm)
route("AC_L", "F", MW, (5.0, 70.0), (12.5, 70.0))
route("AC_LF", "F", MW, (12.5, 64.92), (12.5, 42.0), (12.5, 12.0), (HX + 2.2, 12.0))
route("AC_N", "F", MW, (5.0, 64.92), (5.0, 42.0), (5.0, 19.8), (HX + 2.2, 19.8))

# Mains warning area on the silkscreen
sline(0.3, (0.8, 34.6), (19.5, 34.6), (19.5, 76.6), (0.8, 76.6), (0.8, 34.6))
text("230V AC", 1.6, 49.5, 2.0, 0.3)
text("DANGER", 1.6, 52.5, 1.6, 0.25)
text("! LIVE !", 1.6, 30.8, 1.6, 0.25)

# Title / notes in the empty middle area
text("LORA PICO DIMMER 4CH", 23.5, 57.0, 1.8, 0.25)
text("REV A  2026-11", 23.5, 54.2, 1.4, 0.2)
text("ZC GP26 CH1 GP27", 23.5, 50.0, 1.2, 0.15)
text("CH2 GP28 CH3 GP22", 23.5, 48.0, 1.2, 0.15)
text("CH4 GP13 LED GP15", 23.5, 46.0, 1.2, 0.15)
text("J3 VCC = 3V3 ONLY", 23.5, 43.0, 1.2, 0.15)
text("ANT: IPEX ON RA-02", 23.5, 40.5, 1.2, 0.15)

# ---------------------------------------------------------------- geometry helpers
def seg_seg(a, b, c, d):
    """min distance between segments ab and cd"""
    def pt_seg(p, a, b):
        ax, ay = a; bx, by = b; px, py = p
        dx, dy = bx - ax, by - ay
        L = dx * dx + dy * dy
        t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
        return math.hypot(px - ax - t * dx, py - ay - t * dy)
    def inter(a, b, c, d):
        def cr(o, p, q): return (p[0]-o[0])*(q[1]-o[1]) - (p[1]-o[1])*(q[0]-o[0])
        d1, d2, d3, d4 = cr(c, d, a), cr(c, d, b), cr(a, b, c), cr(a, b, d)
        return (d1 * d2 < 0) and (d3 * d4 < 0)
    if a != b and c != d and inter(a, b, c, d):
        return 0.0
    return min(pt_seg(a, c, d), pt_seg(b, c, d), pt_seg(c, a, b), pt_seg(d, a, b))

def shapes():
    """Every copper object as (net, layers, kind, data). kind: 'cap' (seg + radius) or 'rect'."""
    out = []
    for p in pads:
        if p["shape"] == "C":
            out.append((p["net"], p["layers"], "cap", ((p["x"], p["y"]), (p["x"], p["y"]), p["w"] / 2), p))
        else:
            out.append((p["net"], p["layers"], "rect", (p["x"] - p["w"]/2, p["y"] - p["h"]/2,
                                                         p["x"] + p["w"]/2, p["y"] + p["h"]/2), p))
    for t in tracks:
        out.append((t["net"], {t["layer"]}, "cap", ((t["x1"], t["y1"]), (t["x2"], t["y2"]), t["w"] / 2), t))
    for v in vias:
        out.append((v["net"], {"F", "B"}, "cap", ((v["x"], v["y"]), (v["x"], v["y"]), VIA_D / 2), v))
    return out

def rect_edges(r):
    x1, y1, x2, y2 = r
    c = [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]
    return [(c[i], c[(i + 1) % 4]) for i in range(4)]

def in_rect(p, r): return r[0] <= p[0] <= r[2] and r[1] <= p[1] <= r[3]

def dist(s1, s2):
    k1, d1 = s1[2], s1[3]
    k2, d2 = s2[2], s2[3]
    if k1 == "cap" and k2 == "cap":
        return seg_seg(d1[0], d1[1], d2[0], d2[1]) - d1[2] - d2[2]
    if k1 == "rect" and k2 == "rect":
        if any(in_rect(c[0], d2) for c in rect_edges(d1)) or any(in_rect(c[0], d1) for c in rect_edges(d2)):
            return 0.0
        return min(seg_seg(a, b, c, d) for a, b in rect_edges(d1) for c, d in rect_edges(d2))
    if k1 == "rect":
        s1, s2 = s2, s1; d1, d2 = d2, d1
    a, b, r = d1
    if in_rect(a, d2) or in_rect(b, d2):
        return -r
    return min(seg_seg(a, b, c, d) for c, d in rect_edges(d2)) - r

def bbox(s):
    if s[2] == "cap":
        (a, b, r) = s[3]
        return (min(a[0], b[0]) - r, min(a[1], b[1]) - r, max(a[0], b[0]) + r, max(a[1], b[1]) + r)
    return s[3]

# ---------------------------------------------------------------- DRC
def drc():
    errs = []
    sh = shapes()
    boxes = [bbox(s) for s in sh]
    n = len(sh)
    for i in range(n):
        for j in range(i + 1, n):
            a, b = sh[i], sh[j]
            if a[0] == b[0]:
                continue
            ma, mb = a[0] in MAINS, b[0] in MAINS
            if ma and mb:
                if {a[0], b[0]} == {"AC_L", "AC_LF"}:
                    req, any_layer = CLR, False
                else:
                    req, any_layer = LN_CLR, True
            elif ma or mb:
                req, any_layer = MAINS_CLR, True
            else:
                req, any_layer = CLR, False
            if not any_layer and not (a[1] & b[1]):
                continue
            ba, bb = boxes[i], boxes[j]
            if ba[0] - req > bb[2] or bb[0] - req > ba[2] or ba[1] - req > bb[3] or bb[1] - req > ba[3]:
                continue
            d = dist(a, b)
            if d < req - 1e-6:
                errs.append(f"clearance {d:.3f} < {req} between {a[0]} {desc(a)} and {b[0]} {desc(b)}")
    # board edge
    for s, bx in zip(sh, boxes):
        if bx[0] < EDGE_CLR or bx[1] < EDGE_CLR or bx[2] > BW - EDGE_CLR or bx[3] > BH - EDGE_CLR:
            errs.append(f"edge clearance: {s[0]} {desc(s)}")
    # mounting holes vs copper (hole edge + 0.5)
    for (hx, hy, hd) in npth:
        hole = ("H", {"F", "B"}, "cap", ((hx, hy), (hx, hy), hd / 2), None)
        for s in sh:
            d = dist(hole, s)
            if d < 0.5:
                errs.append(f"copper {s[0]} {desc(s)} {d:.2f} mm from mounting hole at {hx},{hy}")
    # drill to drill
    holes = [(p["x"], p["y"], p["drill"]) for p in pads if p["drill"]] + [(v["x"], v["y"], VIA_DRILL) for v in vias]
    for i in range(len(holes)):
        for j in range(i + 1, len(holes)):
            a, b = holes[i], holes[j]
            if math.hypot(a[0] - b[0], a[1] - b[1]) - a[2] / 2 - b[2] / 2 < 0.5:
                errs.append(f"drill spacing at {a} {b}")
    # annular ring
    for p in pads:
        if p["drill"] and min(p["w"], p["h"]) - p["drill"] < 0.3:
            errs.append(f"annular ring {p['ref']}.{p['pin']}")
    return errs

def desc(s):
    o = s[4]
    if o is None:
        return ""
    if "ref" in o:
        return f"pad {o['ref']}.{o['pin']} @({o['x']:.2f},{o['y']:.2f})"
    if "layer" in o:
        return f"track {o['layer']} ({o['x1']:.2f},{o['y1']:.2f})-({o['x2']:.2f},{o['y2']:.2f})"
    return f"via ({o['x']:.2f},{o['y']:.2f})"

def connectivity(pour_gnd_ids):
    """Union-find over touching same-net copper. pour_gnd_ids: shape indices joined by the pour."""
    sh = shapes()
    parent = list(range(len(sh)))
    def f(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    def u(i, j): parent[f(i)] = f(j)
    for i in range(len(sh)):
        for j in range(i + 1, len(sh)):
            if sh[i][0] == sh[j][0] and (sh[i][1] & sh[j][1]) and dist(sh[i], sh[j]) <= 1e-6:
                u(i, j)
    ids = sorted(pour_gnd_ids)
    for a, b in zip(ids, ids[1:]):
        u(a, b)
    problems = []
    nets = {}
    for i, s in enumerate(sh):
        nets.setdefault(s[0], set()).add(f(i))
    for net, comps in nets.items():
        named = any(p["net"] == net for p in pads if not p["net"].startswith(p["ref"] + "."))
        if len(comps) > 1 and named:
            parts = []
            for c in comps:
                parts.append([desc(sh[i]) for i in range(len(sh)) if f(i) == c and sh[i][0] == net][:2])
            problems.append(f"net {net} is split into {len(comps)} pieces: {parts}")
    return problems

# ---------------------------------------------------------------- layer operation lists
POUR_RECT = (22.5 + OX, 0.5, BW - 0.5, BH - 0.5)   # LV area only, > 8 mm from mains copper

def copper_ops(layer):
    ops = []  # (polarity 'D'|'C', kind, data)
    sh = shapes()
    if layer == "B":
        x1, y1, x2, y2 = POUR_RECT
        ops.append(("D", "poly", [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]))
        for (hx, hy, hd) in npth:
            ops.append(("C", "circle", (hx, hy, hd + 2 * 0.5)))
        # thermal relief for GND through-hole pads
        for p in pads:
            if p["net"] == "GND" and p["drill"]:
                r = max(p["w"], p["h"]) / 2 + POUR_CLR
                ops.append(("C", "circle", (p["x"], p["y"], 2 * r)))
                L = r + 0.3
                for dx, dy in [(1, 0), (-1, 0), (0, 1), (0, -1)]:
                    ops.append(("D", "seg", (p["x"], p["y"], p["x"] + dx * L, p["y"] + dy * L, 0.6)))
        for s in sh:
            if "B" in s[1] and s[0] != "GND":
                ops.append(("C",) + grow(s, POUR_CLR))
    for s in sh:
        if layer in s[1]:
            ops.append(("D",) + grow(s, 0))
    return ops

def grow(s, g):
    if s[2] == "cap":
        (a, b, r) = s[3]
        if a == b:
            return ("circle", (a[0], a[1], 2 * (r + g)))
        return ("seg", (a[0], a[1], b[0], b[1], 2 * (r + g)))
    x1, y1, x2, y2 = s[3]
    if g == 0:
        return ("rect", ((x1 + x2) / 2, (y1 + y2) / 2, x2 - x1, y2 - y1))
    return ("rrect", ((x1 + x2) / 2, (y1 + y2) / 2, x2 - x1, y2 - y1, g))

def mask_ops(layer):
    ops = []
    for p in pads:
        if layer in p["layers"]:
            if p["shape"] == "C":
                ops.append(("D", "circle", (p["x"], p["y"], p["w"] + 0.1)))
            else:
                ops.append(("D", "rect", (p["x"], p["y"], p["w"] + 0.1, p["h"] + 0.1)))
    return ops

def silk_ops():
    return [("D", "seg", s[1:]) for s in silk]

# ---------------------------------------------------------------- Gerber writer
def gerber(fname, func, ops, polarity="Positive"):
    ap = {}
    lines = []
    def code(kind, key):
        if (kind, key) not in ap:
            ap[(kind, key)] = 10 + len(ap)
        return ap[(kind, key)]
    def c(v): return int(round(v * 1e6))
    body = []
    cur = None
    pol = "D"
    for (p, kind, d) in ops:
        if p != pol:
            body.append(f"%LP{p}*%"); pol = p
        if kind == "circle":
            x, y, dia = d
            n = code("C", round(dia, 4))
            if cur != n: body.append(f"D{n}*"); cur = n
            body.append(f"X{c(x)}Y{c(y)}D03*")
        elif kind == "rect":
            x, y, w, h = d
            n = code("R", (round(w, 4), round(h, 4)))
            if cur != n: body.append(f"D{n}*"); cur = n
            body.append(f"X{c(x)}Y{c(y)}D03*")
        elif kind == "seg":
            x1, y1, x2, y2, w = d
            n = code("C", round(w, 4))
            if cur != n: body.append(f"D{n}*"); cur = n
            body.append(f"X{c(x1)}Y{c(y1)}D02*")
            body.append(f"X{c(x2)}Y{c(y2)}D01*")
        elif kind == "rrect":   # rect grown by g: rect + 4 edge strokes
            x, y, w, h, g = d
            n = code("R", (round(w, 4), round(h, 4)))
            if cur != n: body.append(f"D{n}*"); cur = n
            body.append(f"X{c(x)}Y{c(y)}D03*")
            n = code("C", round(2 * g, 4))
            body.append(f"D{n}*"); cur = n
            cs = [(x - w/2, y - h/2), (x + w/2, y - h/2), (x + w/2, y + h/2), (x - w/2, y + h/2), (x - w/2, y - h/2)]
            body.append(f"X{c(cs[0][0])}Y{c(cs[0][1])}D02*")
            for q in cs[1:]:
                body.append(f"X{c(q[0])}Y{c(q[1])}D01*")
        elif kind == "poly":
            body.append("G36*")
            body.append(f"X{c(d[0][0])}Y{c(d[0][1])}D02*")
            for q in d[1:] + [d[0]]:
                body.append(f"X{c(q[0])}Y{c(q[1])}D01*")
            body.append("G37*")
    hdr = ["G04 LoRa Pico dimmer 4ch rev A, generated by gen_pcb.py*",
           f"%TF.FileFunction,{func}*%", f"%TF.FilePolarity,{polarity}*%",
           "%FSLAX46Y46*%", "%MOMM*%", "%LPD*%", "G01*"]
    for (kind, key), n in sorted(ap.items(), key=lambda kv: kv[1]):
        if kind == "C":
            hdr.append(f"%ADD{n}C,{key:.4f}*%")
        else:
            hdr.append(f"%ADD{n}R,{key[0]:.4f}X{key[1]:.4f}*%")
    with open(os.path.join(OUT, fname), "w", newline="\n") as fh:
        fh.write("\n".join(hdr + body + ["M02*"]) + "\n")

def outline_gerber(fname):
    with open(os.path.join(OUT, fname), "w", newline="\n") as fh:
        fh.write("\n".join([
            "G04 board outline*", "%TF.FileFunction,Profile,NP*%", "%FSLAX46Y46*%", "%MOMM*%",
            "%ADD10C,0.1000*%", "D10*", "G01*",
            "X0Y0D02*", f"X{int(BW*1e6)}Y0D01*", f"X{int(BW*1e6)}Y{int(BH*1e6)}D01*",
            f"X0Y{int(BH*1e6)}D01*", "X0Y0D01*", "M02*"]) + "\n")

def excellon(fname, holes, plated):
    tools = sorted({round(d, 3) for (_, _, d) in holes})
    out = ["M48", f"; LoRa Pico dimmer 4ch rev A, {'plated' if plated else 'non-plated'} holes",
           f"; #@! TF.FileFunction,{'Plated' if plated else 'NonPlated'},1,2,{'PTH' if plated else 'NPTH'}",
           "FMAT,2", "METRIC,TZ"]
    for i, d in enumerate(tools):
        out.append(f"T{i+1}C{d:.3f}")
    out += ["%", "G90", "G05"]
    for i, d in enumerate(tools):
        out.append(f"T{i+1}")
        for (x, y, dd) in holes:
            if round(dd, 3) == d:
                out.append(f"X{x:.3f}Y{y:.3f}")
    out.append("M30")
    with open(os.path.join(OUT, fname), "w", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")

# ---------------------------------------------------------------- raster (pour check + preview)
RES = 0.1
GW, GH = int(BW / RES), int(BH / RES)

def raster(ops):
    img = bytearray(GW * GH)
    for (p, kind, d) in ops:
        val = 1 if p == "D" else 0
        if kind == "poly":
            xs = [q[0] for q in d]; ys = [q[1] for q in d]   # only axis-aligned rects are used
            fill_box(img, min(xs), min(ys), max(xs), max(ys), val)
        elif kind == "rect":
            x, y, w, h = d
            fill_box(img, x - w/2, y - h/2, x + w/2, y + h/2, val)
        elif kind == "rrect":
            x, y, w, h, g = d
            fill_box(img, x - w/2 - g, y - h/2, x + w/2 + g, y + h/2, val)
            fill_box(img, x - w/2, y - h/2 - g, x + w/2, y + h/2 + g, val)
            for cx, cy in [(x - w/2, y - h/2), (x + w/2, y - h/2), (x + w/2, y + h/2), (x - w/2, y + h/2)]:
                fill_cap(img, cx, cy, cx, cy, g, val)
        elif kind == "circle":
            x, y, dia = d
            fill_cap(img, x, y, x, y, dia / 2, val)
        elif kind == "seg":
            x1, y1, x2, y2, w = d
            fill_cap(img, x1, y1, x2, y2, w / 2, val)
    return img

def fill_box(img, x1, y1, x2, y2, val):
    i1, i2 = max(0, int(math.ceil(x1 / RES - 0.5))), min(GW - 1, int(math.floor(x2 / RES - 0.5)))
    j1, j2 = max(0, int(math.ceil(y1 / RES - 0.5))), min(GH - 1, int(math.floor(y2 / RES - 0.5)))
    for j in range(j1, j2 + 1):
        base = j * GW
        img[base + i1: base + i2 + 1] = bytes([val]) * (i2 - i1 + 1) if i2 >= i1 else b""

def fill_cap(img, x1, y1, x2, y2, r, val):
    i1 = max(0, int((min(x1, x2) - r) / RES) - 1); i2 = min(GW - 1, int((max(x1, x2) + r) / RES) + 1)
    j1 = max(0, int((min(y1, y2) - r) / RES) - 1); j2 = min(GH - 1, int((max(y1, y2) + r) / RES) + 1)
    dx, dy = x2 - x1, y2 - y1
    L = dx * dx + dy * dy
    r2 = r * r
    for j in range(j1, j2 + 1):
        py = (j + 0.5) * RES
        base = j * GW
        for i in range(i1, i2 + 1):
            px = (i + 0.5) * RES
            t = 0 if L == 0 else max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / L))
            ex, ey = px - x1 - t * dx, py - y1 - t * dy
            if ex * ex + ey * ey <= r2:
                img[base + i] = val

def flood(img, si, sj):
    seen = bytearray(GW * GH)
    st = [sj * GW + si]
    seen[st[0]] = 1
    while st:
        k = st.pop()
        i, j = k % GW, k // GW
        for ni, nj in ((i+1, j), (i-1, j), (i, j+1), (i, j-1)):
            if 0 <= ni < GW and 0 <= nj < GH:
                q = nj * GW + ni
                if img[q] and not seen[q]:
                    seen[q] = 1; st.append(q)
    return seen

def px(x, y): return int(x / RES), int(y / RES)

def write_png(path, rgb, w, h):
    raw = b"".join(b"\x00" + bytes(rgb[(h - 1 - j) * w * 3:(h - j) * w * 3]) for j in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

# ---------------------------------------------------------------- main
def main():
    os.makedirs(OUT, exist_ok=True)
    errs = drc()
    opsF, opsB = copper_ops("F"), copper_ops("B")
    imgB = raster(opsB)
    # pour connectivity: flood from the Hi-Link -Vo pad on the bottom layer
    gnd0 = next(p for p in pads if p["ref"] == "PS1" and p["pin"] == "3")
    seen = flood(imgB, *px(gnd0["x"], gnd0["y"]))
    sh = shapes()
    pour_ids = []
    for i, s in enumerate(sh):
        if "B" not in s[1]:
            continue
        o = s[4]
        x, y = (o["x"], o["y"]) if "x" in o else (o["x1"], o["y1"])
        hit = seen[px(x, y)[1] * GW + px(x, y)[0]]
        if s[0] == "GND" and ("ref" in o or "layer" not in o):
            if hit: pour_ids.append(i)
            else: errs.append(f"GND {desc(s)} is not connected to the pour")
        elif s[0] != "GND" and hit:
            errs.append(f"SHORT: {s[0]} {desc(s)} touches the GND pour")
    errs += connectivity(pour_ids)
    # islands of pour not connected to GND
    rest = sum(1 for k in range(GW * GH) if imgB[k] and not seen[k])
    print(f"bottom copper pixels not in the GND net (other nets + islands): {rest}")

    gerber(f"{NAME}-F_Cu.gtl", "Copper,L1,Top", opsF)
    gerber(f"{NAME}-B_Cu.gbl", "Copper,L2,Bot", opsB)
    gerber(f"{NAME}-F_Mask.gts", "Soldermask,Top", mask_ops("F"), "Negative")
    gerber(f"{NAME}-B_Mask.gbs", "Soldermask,Bot", mask_ops("B"), "Negative")
    gerber(f"{NAME}-F_Silkscreen.gto", "Legend,Top", silk_ops())
    gerber(f"{NAME}-B_Silkscreen.gbo", "Legend,Bot", [])
    outline_gerber(f"{NAME}-Edge_Cuts.gm1")
    pth = [(p["x"], p["y"], p["drill"]) for p in pads if p["drill"]] + [(v["x"], v["y"], VIA_DRILL) for v in vias]
    excellon(f"{NAME}-PTH.drl", pth, True)
    excellon(f"{NAME}-NPTH.drl", npth, False)

    # preview: green board, bottom copper blue, top copper red/orange, silk white, holes black
    imgF = raster(opsF)
    imgS = raster([(p, k, (d[0], d[1], d[2], d[3], max(d[4], 0.2))) for (p, k, d) in silk_ops()])
    holes = raster([("D", "circle", (x, y, d)) for (x, y, d) in pth + npth])
    rgb = bytearray(GW * GH * 3)
    for k in range(GW * GH):
        c = (20, 70, 40)
        if imgB[k]: c = (40, 90, 170)
        if imgF[k]: c = (200, 80, 50) if not imgB[k] else (210, 120, 60)
        if imgS[k]: c = (240, 240, 240)
        if holes[k]: c = (0, 0, 0)
        rgb[3*k:3*k+3] = bytes(c)
    write_png(os.path.join(HERE, "preview.png"), rgb, GW, GH)

    zp = os.path.join(HERE, f"{NAME}_rev_A_gerbers.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(OUT)):
            z.write(os.path.join(OUT, f), f)
    print(f"pads {len(pads)}, tracks {len(tracks)}, vias {len(vias)}, holes {len(pth)} PTH + {len(npth)} NPTH")
    if errs:
        print(f"{len(errs)} DRC problems:")
        for e in errs:
            print("  " + e)
    else:
        print("DRC: no problems")

if __name__ == "__main__":
    main()
