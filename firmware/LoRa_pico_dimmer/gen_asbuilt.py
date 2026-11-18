"""Draws the user's actual board (traced from a photo on 2026-11-14) into perfboard.html.
Run after gen_perfboard.py:  python gen_asbuilt.py perfboard.html
Positions are approximate (traced from a photo taken at an angle); Pico and LoRa pins are exact by name.
Frame: same as the reference layout - Hi-Link on the left, 59 x 35 holes, component side."""
import sys

COLS, ROWS = 59, 35
P, M = 17, 34
W, H = M * 2 + (COLS - 1) * P, M * 2 + (ROWS - 1) * P
def X(c): return M + c * P
def Y(r): return M + r * P
o = []
a = o.append

# Pico fitted mirrored: left column = pins 20..1 top to bottom, right column = pins 21..40, USB at the bottom.
PC_L, PC_R, PC_TOP = 31, 38, 14
def pin(p):
    return (PC_L, PC_TOP + 20 - p) if p <= 20 else (PC_R, PC_TOP + p - 21)

# Ra-02 adapter: left column (photo top row) and right column, rows 15..22 top to bottom
LA, LB = 16, 27
LORA_A = ["GND", "GND", "3.3V", "RST", "DIO0", "DIO1", "DIO2", "DIO3"]
LORA_B = ["GND", "NSS", "MOSI", "MISO", "SCK", "DIO5", "DIO4", "GND"]
def lora(name, col):
    names = LORA_A if col == LA else LORA_B
    return (col, 15 + names.index(name))

def pt(c, r): return f"{X(c):.1f},{Y(r):.1f}"
def wire(cls, pts, todo=False, curve=False):
    if curve:   # one smooth loop through the given control points (like the real flying wires)
        (c0, r0), (c1, r1), (c2, r2), (c3, r3) = pts
        d = f"M{pt(c0, r0)} C{pt(c1, r1)} {pt(c2, r2)} {pt(c3, r3)}"
    else:
        d = "M" + " L".join(pt(c, r) for c, r in pts)
    a(f'<path d="{d}" class="tr {cls}{" todo" if todo else ""}"/>')
def label(c, r, text, cls="ref", anchor="start"):
    a(f'<text x="{X(c):.1f}" y="{Y(r):.1f}" class="{cls}" text-anchor="{anchor}">{text}</text>')
def hdr(pins, cls="hdr"):
    cs = [p[0] for p in pins]; rs = [p[1] for p in pins]
    a(f'<rect x="{X(min(cs))-8}" y="{Y(min(rs))-8}" width="{X(max(cs))-X(min(cs))+16}" height="{Y(max(rs))-Y(min(rs))+16}" rx="3" class="{cls}"/>')
    for c, r, n in pins:
        a(f'<rect x="{X(c)-4.5}" y="{Y(r)-4.5}" width="9" height="9" class="pin {n}"/>')

a(f'<svg viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" aria-label="Your board as built, traced from the photo: Hi-Link left, Ra-02 next to it, Pico mirrored with USB at the bottom edge">')
a(f'<rect x="4" y="4" width="{W-8}" height="{H-8}" rx="10" class="board"/>')
# mains area as built: the Hi-Link's AC end plus the varistor / fuse corner above it
a(f'<path d="M{pt(-0.6,-0.6)} L{pt(23.4,-0.6)} L{pt(23.4,10.4)} L{pt(13.4,10.4)} L{pt(13.4,13.4)} L{pt(-0.6,13.4)} Z" class="mains"/>')
label(0.3, 1.5, "⚡ 230 V AREA", "mains-t")
for c in range(COLS):
    for r in range(ROWS):
        a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="2.4" class="hole"/>')
for c, r in [(0, 0), (58, 0), (0, 34), (58, 34)]:
    a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="8" class="mnt"/>')

# Hi-Link, AC end at the top, outputs at the bottom
a(f'<rect x="{X(0.9)}" y="{Y(10.6)}" width="{X(12.1)-X(0.9)}" height="{Y(30.4)-Y(10.6)}" rx="5" class="hlk"/>')
label(6.5, 20.4, "Hi-Link", "hlk-t", "middle"); label(6.5, 21.3, "HLK-10M05", "tiny", "middle")
for c, r, n, cls in [(3, 12, "AC-N", "ac"), (9, 12, "AC-L", "ac"), (3, 29, "+Vo", "v5"), (9, 29, "−Vo", "gnd")]:
    a(f'<rect x="{X(c)-5}" y="{Y(r)-5}" width="10" height="10" class="pin {cls}"/>')
    label(c, r + (1.1 if r == 12 else -0.6), n, "pl", "middle")

# varistor / fuse corner (parts plugged into small black sockets)
hdr([(14, 9, "ac"), (14, 8, "ac")]); hdr([(20, 9, "ac"), (21, 9, "ac")]); hdr([(20, 6, "ac"), (21, 6, "ac")])
hdr([(12, 1, "nc"), (13, 1, "nc"), (14, 1, "nc"), (15, 1, "nc"), (16, 1, "nc")])
label(17.3, 1.35, "5-pin socket ?", "pl q")
a(f'<line x1="{X(14)}" y1="{Y(8)}" x2="{X(16)}" y2="{Y(6)}" class="lead"/><line x1="{X(20)}" y1="{Y(9)}" x2="{X(17.5)}" y2="{Y(6.5)}" class="lead"/>')
a(f'<ellipse cx="{X(16.8)}" cy="{Y(5.6)}" rx="30" ry="24" class="mov"/>')
label(16.8, 5.8, "RV1 561K", "tiny ink", "middle")
a(f'<circle cx="{X(18.6)}" cy="{Y(3.6)}" r="20" class="fusebody"/>')
label(18.6, 3.75, "F1 ?", "tiny ink", "middle")
a(f'<ellipse cx="{X(5.2)}" cy="{Y(9.3)}" rx="16" ry="10" class="mov"/>')
label(7, 8.2, "blue part ?", "pl q")

# C1 in a 3-pin socket, D1 in a 2-pin socket with long legs, white JST
hdr([(23, 30, "gnd"), (23, 31, "nc"), (23, 32, "v5")])
a(f'<circle cx="{X(22)}" cy="{Y(27.2)}" r="22" class="ecap"/>'); label(22, 27.4, "470µ", "tiny", "middle"); label(20.2, 25.2, "C1", "ref", "middle")
hdr([(43, 29, "vsys"), (43, 30, "v5")])
a(f'<path d="M{pt(43,29)} C{pt(49,26)} {pt(51,27)} {pt(51.5,27.6)} M{pt(43,30)} C{pt(48,32)} {pt(51,31)} {pt(51.5,29.6)}" class="lead" fill="none"/>')
a(f'<rect x="{X(51.5)-6}" y="{Y(27.6)}" width="12" height="{Y(29.6)-Y(27.6)}" rx="3" class="diode"/><rect x="{X(51.5)-6}" y="{Y(27.6)+1}" width="12" height="5" class="band"/>')
label(52.5, 28.8, "D1 1N5817")
a(f'<rect x="{X(45.4)}" y="{Y(19.4)}" width="{X(46.8)-X(45.4)}" height="{Y(21.6)-Y(19.4)}" rx="2" class="jst"/>')
a(f'<rect x="{X(46)-4}" y="{Y(20)-4}" width="8" height="8" class="pin nc"/><rect x="{X(46)-4}" y="{Y(21)-4}" width="8" height="8" class="pin nc"/>')
label(47.3, 20.8, "white JST 2-pin ?", "pl q")

# Ra-02 adapter
a(f'<rect x="{X(15.3)}" y="{Y(14.3)}" width="{X(27.7)-X(15.3)}" height="{Y(22.7)-Y(14.3)}" rx="4" class="lora"/>')
a(f'<rect x="{X(17.6)}" y="{Y(15.6)}" width="{X(25.4)-X(17.6)}" height="{Y(21.4)-Y(15.6)}" rx="3" class="can"/>')
label(21.5, 18.4, "Ra-02", "hlk-t", "middle"); label(21.5, 19.3, "on its blue adapter", "tiny", "middle")
for col, names in ((LA, LORA_A), (LB, LORA_B)):
    for i, n in enumerate(names):
        cls = {"GND": "gnd", "3.3V": "v33", "RST": "spi", "DIO0": "spi", "NSS": "spi", "MOSI": "spi",
               "MISO": "spi", "SCK": "spi"}.get(n, "nc")
        a(f'<rect x="{X(col)-4.5}" y="{Y(15+i)-4.5}" width="9" height="9" class="pin {cls}"/>')
        if col == LA: label(col + 0.55, 15 + i + 0.2, n, "pl lo")
        else: label(col - 0.55, 15 + i + 0.2, n, "pl lo", "end")
# antenna lead: u.FL near the top-left of the adapter, black wire off the top edge
a(f'<path d="M{pt(17.9,15.4)} C{pt(14,10)} {pt(9,4)} {pt(6,-0.8)}" class="ant"/>')
label(7.4, 3.2, "antenna wire", "pl q")

# Pico (on the underside, drawn through the board)
a(f'<rect x="{X(30.35)}" y="{Y(13.4)}" width="{X(38.65)-X(30.35)}" height="{Y(33.6)-Y(13.4)}" rx="5" class="pico"/>')
a(f'<rect x="{X(33.3)}" y="{Y(33.3)}" width="{X(35.7)-X(33.3)}" height="{Y(34.5)-Y(33.3)}" rx="2" class="usb"/>')
cx, cy = X(34.5), Y(22.5)
a(f'<text x="{cx}" y="{cy}" class="pico-t" text-anchor="middle" transform="rotate(-90 {cx} {cy})" dominant-baseline="middle">Pico (mirrored, under the board)</text>')
used = {20, 18, 3, 21, 22, 24, 25, 26, 27, 28, 30, 31, 32, 34, 36, 38, 39}
names = {20: "GP15", 18: "GND", 3: "GND", 21: "GP16", 22: "GP17", 23: "GND", 24: "GP18",
         25: "GP19", 26: "GP20", 27: "GP21", 28: "GND", 29: "GP22", 30: "RUN", 31: "GP26", 32: "GP27", 33: "AGND", 34: "GP28", 35: "VREF", 36: "3V3", 37: "EN", 38: "GND", 39: "VSYS", 40: "VBUS",
         13: "GND", 8: "GND", 1: "GP0"}
for p in range(1, 41):
    c, r = pin(p)
    a(f'<circle cx="{X(c)}" cy="{Y(r)}" r="4.6" class="{"pad used" if p in used else "pad"}"/>')
    if p in names:
        if p <= 20: label(c + 0.5, r + 0.18, f"{p} {names[p]}", "pl in")
        else: label(c - 0.5, r + 0.18, f"{names[p]} {p}", "pl in", "end")

# ---- wires already on the board (traced from the photo) ----
wire("v5", [(3, 29), (3, 33.3), (23, 33.3), (23, 32)])                 # +Vo -> C1 socket (grey wire)
wire("v5", [(23, 33.3), (43, 33.3), (43, 30)])                         # -> D1 socket (grey wire)
wire("vsys", [(43, 29), (42.2, 29), (42.2, 32.5), (38.6, 32.5), pin(39)])   # D1 -> VSYS
wire("gnd", [(9, 29), (13, 29), (13, 30), (23, 30)])                   # -Vo -> C1 -
wire("gnd", [(23, 30), (29, 30), (29, 31), pin(3)])                    # -> Pico pin 3
wire("gnd", [(27, 22), (29, 24), (29, 30)])                           # LoRa GND (blue wire)
for nm, p, lift in (("MISO", 21, 4.5), ("NSS", 22, 5.5), ("SCK", 24, 6.5), ("MOSI", 25, 7.5)):
    c0, r0 = lora(nm, LB); c3, r3 = pin(p)
    wire("spi", [(c0, r0), (c0 + 4, r0 - lift - 6), (c3 + 6, r3 - lift - 6), (c3, r3)], curve=True)
for nm, p, lift in (("RST", 27, 9.5), ("DIO0", 26, 8.5)):
    c0, r0 = lora(nm, LA); c3, r3 = pin(p)
    wire("spi", [(c0, r0), (c0 - 2, r0 - lift - 4), (c3 + 9, r3 - lift - 4), (c3, r3)], curve=True)
c0, r0 = lora("3.3V", LA); c3, r3 = pin(36)
wire("v33", [(c0, r0), (c0 - 1, r0 - 9), (c3 + 12, r3 - 20), (c3, r3)], curve=True)

# ---- still to wire (dashed) ----
J4 = [(24, "Z-C", "dim"), (25, "PWM1 light", "dim"), (26, "", "nc"), (27, "PWM2 fan", "fan"), (28, "", "nc"),
      (29, "VCC", "v33"), (30, "", "nc"), (31, "GND", "gnd")]
hdr([(40, r, cls) for r, n, cls in J4])
for r, n, cls in J4:
    if n: label(40.7, r + 0.2, n, "pl")
label(40.7, 26.25, "J4 → dimmer", "ref")
for r, n, cls in J4:
    if n: wire(cls, [(38, r), (40, r)], todo=True)
hdr([(40, 21, "gnd"), (40, 23, "run")])
label(40.7, 21.2, "GND", "pl"); label(40.7, 23.2, "RUN", "pl")
label(40.7, 22.25, "J7 → reset", "ref")
wire("gnd", [pin(28), (40, 21)], todo=True)
wire("run", [pin(30), (40, 23)], todo=True)
# status LED: GP15 is the top-left pin now
a(f'<line x1="{X(31)}" y1="{Y(11)}" x2="{X(34)}" y2="{Y(11)}" class="lead"/><rect x="{X(31.6)}" y="{Y(11)-6}" width="{X(33.4)-X(31.6)}" height="12" rx="4" class="res"/>')
label(32.5, 10.2, "R1 330Ω", "ref", "middle")
hdr([(35, 11, "led"), (36, 11, "gnd")])
label(35.5, 12.4, "J6 LED + −", "pl", "middle")
wire("led", [pin(20), (31, 11)], todo=True)
wire("led", [(34, 11), (35, 11)], todo=True)
wire("gnd", [pin(18), (29.8, 16), (29.8, 9.6), (36, 9.6), (36, 11)], todo=True)

for c in range(0, COLS, 5):
    a(f'<text x="{X(c)}" y="{H-8}" class="ruler" text-anchor="middle">{c}</text>')
for r in range(0, ROWS, 5):
    a(f'<text x="12" y="{Y(r)+3}" class="ruler" text-anchor="middle">{r}</text>')
a('</svg>')

path = sys.argv[1]
html = open(path, encoding="utf-8").read()
assert "<!--ASLAYOUT-->" in html, "run gen_perfboard.py first (template needs <!--ASLAYOUT-->)"
open(path, "w", encoding="utf-8").write(html.replace("<!--ASLAYOUT-->", "\n".join(o)))
print("ok")
