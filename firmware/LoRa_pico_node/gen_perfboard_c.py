"""Generates the Pico node perfboard build sheet (rev C: Pico upright with its USB end at the
top edge of the board; HLK-10M05 + LoRa breakout on board)."""
import sys

COLS, ROWS = 59, 35          # 15 x 9 cm perfboard, 2.54 mm pitch
P, M = 17, 34                # px per hole, margin
W, H = M * 2 + (COLS - 1) * P, M * 2 + (ROWS - 1) * P

def X(c): return M + c * P
def Y(r): return M + r * P
out = []
a = out.append

def poly(cls, pts, extra=""):
    d = " ".join(f"{X(c)},{Y(r)}" for c, r in pts)
    a(f'<polyline points="{d}" class="tr {cls}{extra}"/>')

# Pico stands upright, USB end at the top. Left pin row (pins 1-20) on column 30,
# right pin row (pins 40-21) on column 37. Pin p sits on row p (left) or 41-p (right).
PL, PR = 30, 37
def pl(p): return (PL, p)
def pr(p): return (PR, 41 - p)

# ---------------- nets (solder-side bare wire) ----------------
traces = [
    # mains (all inside the mains zone)
    ("ac", [(1, 2), (3, 2)]),                      # L: J0 -> F1
    ("ac", [(5, 2), (9, 2), (9, 14)]),             # L after fuse -> HLK AC-L
    ("ac", [(6, 8), (9, 8)]),                      # MOV leg -> L
    ("ac", [(1, 4), (1, 12), (3, 12), (3, 14)]),   # N: J0 -> HLK AC-N
    ("ac", [(1, 8), (3, 8)]),                      # MOV leg -> N
    # HLK +Vo -> 5 V rail: along the bottom, up the right edge, along row 2 to D1
    ("v5", [(3, 30), (3, 32), (58, 32), (58, 2), (42, 2)]),
    # 5 V branch up to J4 VCC and J5 +5V
    ("v5", [(22, 32), (22, 8), (26, 8)]),
    ("v5", [(22, 14), (26, 14)]),
    # HLK -Vo -> GND bus along row 3 (J4 GND, Pico 3, Pico 38, C3/C4, LoRa GND)
    ("gnd", [(9, 30), (14, 30), (14, 3), (49, 3)]),
    # VSYS: D1 cathode -> Pico 39
    ("vsys", [(39, 2), pr(39)]),
    # 3V3: Pico 36 -> LoRa 3.3V
    ("v33", [pr(36), (38, 5), (38, 4), (49, 4)]),
    # LoRa control lines (bare)
    ("spi", [pr(27), (44, 14), (44, 5), (49, 5)]),   # RST  GP21
    ("spi", [pr(26), (45, 15), (45, 6), (49, 6)]),   # DIO0 GP20
    # RUN / reset button
    ("run", [pr(30), (42, 11)]),
    ("gnd", [pr(28), (42, 13)]),
    # GND pin 13 -> J5 GND
    ("gnd", [pl(13), (26, 13)]),
    # LED: GP15 -> R1 -> J6 +, GND pin 18 -> J6 -
    ("led", [pl(20), (30, 22)]),
    ("led", [(30, 25), (30, 26), (29, 26), (29, 27)]),
    ("gnd", [pl(18), (28, 18), (28, 27)]),
]
for p in range(4, 8):                      # GP2-GP5 -> J4 IN1-IN4
    traces.append(("rly", [pl(p), (26, p)]))
for p in range(9, 13):                     # GP6-GP9 -> J5 IN1-IN4
    traces.append(("fan", [pl(p), (26, p)]))
# SPI to the far pin row: insulated wires (they must cross each other)
spi_wires = [
    ("MOSI", [pr(25), (54, 16), (54, 8), (57, 8)]),
    ("SCK",  [pr(24), (51, 17), (51, 6), (57, 6)]),
    ("NSS",  [pr(22), (55, 19), (55, 9), (57, 9)]),
    ("MISO", [pr(21), (52, 20), (52, 7), (57, 7)]),
]

a(f'<svg viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" aria-label="Perfboard layout rev C, component side, Pico USB at the top edge">')
a(f'<rect x="4" y="4" width="{W-8}" height="{H-8}" rx="10" class="board"/>')
a(f'<rect x="{X(-0.6)}" y="{Y(0.9)}" width="{X(11.7)-X(-0.6)}" height="{Y(15.4)-Y(0.9)}" rx="6" class="mains"/>')
a(f'<text x="{X(5.5)}" y="{Y(0.55)}" class="mains-t" text-anchor="middle">⚡ 230 V AC ZONE</text>')
a(f'<rect x="{X(11.7)}" y="{Y(0.9)}" width="{X(12.6)-X(11.7)}" height="{Y(15.4)-Y(0.9)}" class="keepout"/>')
for c in range(COLS):
    for r in range(ROWS):
        a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="2.4" class="hole"/>')
for c, r in [(0, 0), (58, 0), (0, 34), (58, 34)]:
    a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="8" class="mnt"/>')

# HLK-10M05 body (vertical)
a(f'<rect x="{X(0.9)}" y="{Y(12.6)}" width="{X(11.1)-X(0.9)}" height="{Y(31.4)-Y(12.6)}" rx="5" class="hlk"/>')
a(f'<text x="{X(6)}" y="{Y(21.6)}" class="hlk-t" text-anchor="middle">HLK-10M05</text>')
a(f'<text x="{X(6)}" y="{Y(22.6)}" class="tiny" text-anchor="middle">5 V 2 A · 48 × 28 mm</text>')
a(f'<text x="{X(6)}" y="{Y(23.5)}" class="tiny" text-anchor="middle">AC end at top</text>')
# Pico body, upright, USB at the top
a(f'<rect x="{X(29.35)}" y="{Y(0.45)}" width="{X(37.65)-X(29.35)}" height="{Y(20.55)-Y(0.45)}" rx="5" class="pico"/>')
a(f'<rect x="{X(31.9)}" y="{Y(-0.15)}" width="{X(35.1)-X(31.9)}" height="{Y(0.75)-Y(-0.15)}" rx="2" class="usb"/>')
a(f'<text x="{X(33.5)}" y="{Y(-0.75)}" class="ref" text-anchor="middle">▲ USB cable plugs in here</text>')
tx, ty = X(33.1), Y(11.2)
a(f'<text x="{tx}" y="{ty}" class="pico-t" text-anchor="middle" transform="rotate(-90 {tx} {ty})" dominant-baseline="middle">Raspberry Pi Pico 2</text>')
tx2 = X(34.1)
a(f'<text x="{tx2}" y="{ty}" class="tiny" text-anchor="middle" transform="rotate(-90 {tx2} {ty})" dominant-baseline="middle">on 2 × 20 female headers · USB end up</text>')
# LoRa breakout outline (one row lower than rev B)
a(f'<rect x="{X(48.3)}" y="{Y(1.3)}" width="{X(57.7)-X(48.3)}" height="{Y(11.4)-Y(1.3)}" rx="4" class="lora"/>')
a(f'<rect x="{X(50.6)}" y="{Y(3.6)}" width="{X(55.4)-X(50.6)}" height="{Y(8.6)-Y(3.6)}" rx="2" class="can"/>')
a(f'<text x="{X(53)}" y="{Y(10.6)}" class="tiny" text-anchor="middle">Ra-02 breakout</text>')
a(f'<text x="{X(53)}" y="{Y(0.05)}" class="ref" text-anchor="middle">▲ antenna off this edge</text>')

for cls, pts in traces:
    poly(cls, pts)
for name, pts in spi_wires:
    poly("spi", pts, " jump")

# ---- parts ----
def lead(c1, r1, c2, r2):
    a(f'<line x1="{X(c1)}" y1="{Y(r1)}" x2="{X(c2)}" y2="{Y(r2)}" class="lead"/>')
# F1 fuse
lead(3, 2, 5, 2); a(f'<rect x="{X(3.3)}" y="{Y(2)-6}" width="{X(4.7)-X(3.3)}" height="12" rx="6" class="fuse"/>')
a(f'<text x="{X(4)}" y="{Y(1.25)}" class="ref" text-anchor="middle">F1 T1A</text>')
# RV1 MOV
lead(3, 8, 6, 8); a(f'<rect x="{X(3.6)}" y="{Y(8)-8}" width="{X(5.4)-X(3.6)}" height="16" rx="8" class="mov"/>')
a(f'<text x="{X(4.5)}" y="{Y(7.1)}" class="ref" text-anchor="middle">RV1 MOV</text>')
# D1: anode on the 5 V rail (42,2), cathode / stripe toward the Pico (39,2)
lead(39, 2, 42, 2)
cx, cy = (X(39) + X(42)) / 2, Y(2)
a(f'<rect x="{cx-16}" y="{cy-6}" width="32" height="12" rx="3" class="diode"/><rect x="{cx-13}" y="{cy-6}" width="5" height="12" class="band"/>')
a(f'<text x="{X(40.5)}" y="{Y(1.2)}" class="ref" text-anchor="middle">D1 1N5817</text>')
# C1 at the HLK output: GND (14,30) / 5 V (14,32)
lead(14, 30, 14, 32)
a(f'<circle cx="{X(14)}" cy="{Y(31)}" r="12" class="ecap"/><text x="{X(14)}" y="{Y(31)+3}" class="tiny" text-anchor="middle">470µ</text>')
a(f'<text x="{X(15)}" y="{Y(31)+4}" class="ref">C1</text>')
# C3 / C4 across GND (row 3) and 3.3 V (row 4) next to the module
a(f'<circle cx="{X(44)}" cy="{(Y(3)+Y(4))/2}" r="9" class="ecap"/>')
a(f'<rect x="{X(46)-5}" y="{Y(3)-3}" width="10" height="{Y(4)-Y(3)+6}" rx="4" class="ccap"/>')
a(f'<text x="{X(45)}" y="{Y(1.2)}" class="ref" text-anchor="middle">C3 · C4</text>')
# R1 (GP15 -> J6 +)
lead(30, 22, 30, 25)
a(f'<rect x="{X(30)-6}" y="{Y(23.5)-17}" width="12" height="34" rx="4" class="res"/>')
a(f'<text x="{X(30.7)}" y="{Y(23.5)+4}" class="ref">R1 330Ω</text>')
# SW1 across RUN (row 11) and GND (row 13)
a(f'<rect x="{X(38.6)}" y="{Y(10.6)}" width="{X(41.4)-X(38.6)}" height="{Y(13.4)-Y(10.6)}" rx="3" class="sw"/>')
a(f'<circle cx="{X(40)}" cy="{Y(12)}" r="7" class="swb"/>')
a(f'<text x="{X(40)}" y="{Y(10.25)}" class="ref" text-anchor="middle">SW1 · J7 reset</text>')

# ---- connectors / sockets ----
def header(ref, label, pins, side, lx, ly, anchor="end"):
    cs = [p[0] for p in pins]; rs = [p[1] for p in pins]
    a(f'<rect x="{X(min(cs))-8}" y="{Y(min(rs))-8}" width="{X(max(cs))-X(min(cs))+16}" height="{Y(max(rs))-Y(min(rs))+16}" rx="3" class="hdr"/>')
    for c, r, name, cls in pins:
        a(f'<rect x="{X(c)-4.5}" y="{Y(r)-4.5}" width="9" height="9" class="pin {cls}"/>')
        if side == "left":
            a(f'<text x="{X(c)-11}" y="{Y(r)+3}" class="pl" text-anchor="end">{name}</text>')
        elif side == "right":
            a(f'<text x="{X(c)+11}" y="{Y(r)+3}" class="pl">{name}</text>')
        elif side == "below":
            a(f'<text x="{X(c)}" y="{Y(r)+18}" class="pl" text-anchor="middle">{name}</text>')
    if ref:
        a(f'<text x="{lx}" y="{ly}" class="ref" text-anchor="{anchor}">{ref}</text>')
    if label:
        a(f'<text x="{lx}" y="{ly+12}" class="tiny" text-anchor="{anchor}">{label}</text>')

header("J0", "MAINS IN", [(1, 2, "L", "ac"), (1, 4, "N", "ac")], "right", X(1.9), Y(5.6), "start")
header("J4", "RELAY 4-ch", [(26, 3, "GND", "gnd"), (26, 4, "IN1", "rly"), (26, 5, "IN2", "rly"),
       (26, 6, "IN3", "rly"), (26, 7, "IN4", "rly"), (26, 8, "VCC", "v5")], "left", X(21.4), Y(5.3))
header("J5", "FAN STEPPER", [(26, 9, "IN1", "fan"), (26, 10, "IN2", "fan"), (26, 11, "IN3", "fan"),
       (26, 12, "IN4", "fan"), (26, 13, "GND", "gnd"), (26, 14, "+5V", "v5")], "left", X(21.4), Y(11.3))
header("J6", "LED", [(28, 27, "−", "gnd"), (29, 27, "+", "led")], "below", X(27.3), Y(27.3))
header("", "", [(42, 11, "RUN", "run"), (42, 13, "GND", "gnd")], "right", 0, 0)

# LoRa sockets: two 1x8 female headers, rows 3-10
rowA = ["GND", "3.3V", "RST", "DIO0", "DIO1", "DIO2", "DIO3", "GND"]
rowB = ["GND", "DIO4", "DIO5", "SCK", "MISO", "MOSI", "NSS", "GND"]
clsA = {"GND": "gnd", "3.3V": "v33", "RST": "spi", "DIO0": "spi"}
clsB = {"SCK": "spi", "MISO": "spi", "MOSI": "spi", "NSS": "spi", "GND": "nc"}
for col, names, cmap in ((49, rowA, clsA), (57, rowB, clsB)):
    a(f'<rect x="{X(col)-8}" y="{Y(3)-8}" width="16" height="{Y(10)-Y(3)+16}" rx="3" class="hdr"/>')
    for i, n in enumerate(names):
        cls = cmap.get(n, "nc")
        a(f'<rect x="{X(col)-4.5}" y="{Y(3+i)-4.5}" width="9" height="9" class="pin {cls}"/>')
        if col == 49:
            a(f'<text x="{X(col)+9}" y="{Y(3+i)+3}" class="pl lo">{n}</text>')
        else:
            a(f'<text x="{X(col)-9}" y="{Y(3+i)+3}" class="pl lo" text-anchor="end">{n}</text>')
a(f'<text x="{X(53)}" y="{Y(12.1)}" class="ref" text-anchor="middle">U2 LoRa (2 × 8 female header)</text>')

# HLK pins
for c, r, n, cls in [(3, 14, "AC-N", "ac"), (9, 14, "AC-L", "ac"), (3, 30, "+Vo", "v5"), (9, 30, "−Vo", "gnd")]:
    a(f'<rect x="{X(c)-5}" y="{Y(r)-5}" width="10" height="10" class="pin {cls}"/>')
    a(f'<text x="{X(c)}" y="{Y(r) + (15 if r == 14 else -9)}" class="pl" text-anchor="middle">{n}</text>')

# Pico pads + labels (labels inside the body)
right = {40: "VBUS", 39: "VSYS", 38: "GND", 36: "3V3", 30: "RUN", 28: "GND", 27: "GP21", 26: "GP20",
         25: "GP19", 24: "GP18", 23: "GND", 22: "GP17", 21: "GP16"}
left = {1: "GP0", 3: "GND", 4: "GP2", 5: "GP3", 6: "GP4", 7: "GP5", 8: "GND", 9: "GP6", 10: "GP7",
        11: "GP8", 12: "GP9", 13: "GND", 18: "GND", 20: "GP15"}
used_r = {39, 38, 36, 30, 28, 27, 26, 25, 24, 22, 21}
used_l = {3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 18, 20}
for p in range(21, 41):
    c, r = pr(p)
    a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="4.6" class="{"pad used" if p in used_r else "pad"}"/>')
    if p in right:
        a(f'<text x="{X(c)-8}" y="{Y(r)+3}" class="pl in" text-anchor="end">{right[p]} {p}</text>')
for p in range(1, 21):
    c, r = pl(p)
    a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="4.6" class="{"pad used" if p in used_l else "pad"}"/>')
    if p in left:
        a(f'<text x="{X(c)+8}" y="{Y(r)+3}" class="pl in">{p} {left[p]}</text>')

# SPI wire labels (on the vertical run, just below the turn)
for name, pts in spi_wires:
    c, r = pts[0]
    a(f'<text x="{X(46.5)}" y="{Y(r)-3}" class="pl wl">{name}</text>')

for c in range(0, COLS, 5):
    a(f'<text x="{X(c)}" y="{H-8}" class="ruler" text-anchor="middle">{c}</text>')
for r in range(0, ROWS, 5):
    a(f'<text x="12" y="{Y(r)+3}" class="ruler" text-anchor="middle">{r}</text>')
a('</svg>')

# ---- sanity check: no two different bare nets share a hole ----
def holes(pts):
    hs = set()
    for (c1, r1), (c2, r2) in zip(pts, pts[1:]):
        if c1 == c2:
            for r in range(min(r1, r2), max(r1, r2) + 1): hs.add((c1, r))
        else:
            for c in range(min(c1, c2), max(c1, c2) + 1): hs.add((c, r1))
    return hs
NET = {"ac": None, "v5": "5V", "gnd": "GND", "vsys": "VSYS", "v33": "3V3", "run": "RUN", "led": None}
owner = {}
padnet = {}
for p in range(1, 41):
    nm = (left if p <= 20 else right).get(p, f"pico{p}")
    padnet[pl(p) if p <= 20 else pr(p)] = "GND" if nm == "GND" else ("3V3" if nm == "3V3" else nm)
for i, n in enumerate(rowA): padnet[(49, 3 + i)] = {"GND": "GND", "3.3V": "3V3"}.get(n, "L" + n)
for i, n in enumerate(rowB): padnet[(57, 3 + i)] = "GND" if n == "GND" else "L" + n
for i, (cls, pts) in enumerate(traces):
    net = NET.get(cls) or f"{cls}{i}"
    if cls == "spi": net = f"spi{i}"
    if cls == "ac": net = "L" if i < 3 else "N"
    pads = [padnet[h] for h in holes(pts) if h in padnet]
    sig = [x for x in pads if x not in ("GND", "3V3", "VSYS")]
    if cls not in ("gnd", "v33", "vsys", "v5") and len(set(sig)) > 2:
        raise SystemExit(f"trace {i} {cls} touches pads {pads}")
    if cls in ("gnd", "v33", "vsys") and any(x != NET[cls] for x in pads):
        raise SystemExit(f"trace {i} {cls} touches pads {pads}")
    if cls == "v5" and pads:
        raise SystemExit(f"5V trace touches pads {pads}")
    for h in holes(pts):
        if h in owner and owner[h] != net:
            # LED path and its R1 are separate nets joined by R1, so they never share a hole either
            raise SystemExit(f"short: {h} {owner[h]} vs {net}")
        owner[h] = net

html = open(sys.argv[1], encoding="utf-8").read().replace("<!--SVG-->", "\n".join(out))
open(sys.argv[2], "w", encoding="utf-8").write(html)
print("ok", W, H)
