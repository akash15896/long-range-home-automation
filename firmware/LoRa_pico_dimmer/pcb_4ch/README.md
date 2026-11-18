# LoRa Pico dimmer node: 4-channel carrier PCB, rev A (2026-11-15)

A carrier board for the Pico dimmer node. It holds the Pico, the Ra-02 radio, the Hi-Link supply and a
7-pin header (J3) that goes to a separate **4-channel AC dimmer module** (RobotDyn style). No triacs
are on this board. The only mains on it is the Hi-Link input (J1 -> F1 -> RV1 -> HLK), kept in the
left-hand zone.

- `gen_pcb.py`: generates everything (`python gen_pcb.py`). It runs the clearance and connectivity
  checks and prints `DRC: no problems` when they pass.
- `gerbers/`: Gerber RS-274X + Excellon files.
- `LoRa_pico_dimmer_4ch_rev_A_gerbers.zip`: upload this file to the PCB maker.
- `preview.png`: top view (red = top copper, blue = bottom GND pour, white = silkscreen).

## Ordering (JLCPCB / PCBWay)
2 layers, 108 x 84 mm, 1.6 mm FR-4, 1 oz copper, HASL, any colour. Upload the zip. Nothing else needs
setting. The files are mounting holes M3 x 4 (NPTH) + 99 plated holes (min drill 0.3 mm vias).

## Pin map (firmware GPIO numbers)
| Function | Pico pin | GPIO | Goes to |
|---|---|---|---|
| Dimmer Z-C | 31 | GP26 | J3 ZC |
| Channel 1 (light) | 32 | GP27 | J3 CH1 |
| Channel 2 (fan) | 34 | GP28 | J3 CH2 |
| **Channel 3 (new)** | 29 | **GP22** | J3 CH3 |
| **Channel 4 (new)** | 17 | **GP13** | J3 CH4 |
| Dimmer VCC | 36 | 3V3 | J3 3V3 (3.3 V only, RP2040 is not 5 V tolerant) |
| LoRa MISO / NSS / SCK / MOSI | 21 / 22 / 24 / 25 | GP16 / 17 / 18 / 19 | Ra-02 13 / 15 / 12 / 14 |
| LoRa DIO0 / RST | 26 / 27 | GP20 / GP21 | Ra-02 5 / 4 |
| Status LED | 20 | GP15 | R1 330R -> J6 + (LED - to J6 -) |
| Spare button | 19 | GP14 | J7 (GP14 / GND) |
| Reset | 30 | RUN | SW1 on board + J5 (panel push switch, NO contact) |
| Power in | 39 | VSYS | Hi-Link +5 V through D1 1N5817 (USB can stay plugged in) |

The existing firmware pins (Z-C GP26, CH1 GP27, CH2 GP28, LoRa, LED) are unchanged. CH3 and CH4 need
a firmware update before they do anything.

**J3 order (top to bottom):** 3V3, GND, ZC, CH1, CH2, CH3, CH4. The 4-channel module's own header may be
in a different order, so wire it by name (dupont leads). The module's D1-D4 / PWM1-4 inputs go to CH1-CH4.

## Bill of materials
| Ref | Part | Footprint |
|---|---|---|
| U1 | Raspberry Pi Pico (RP2040) | 2 x 20 female headers, 2.54 mm, rows 17.78 mm apart |
| U2 | Ai-Thinker Ra-02 (SX1278 433 MHz) + IPEX 433 MHz antenna | SMD, 16 pads, 2.0 mm pitch (solder the bare module, no adapter) |
| PS1 | Hi-Link HLK-10M05 (5 V 2 A) | THT, per datasheet V1.5 |
| J1 | 2-pin screw terminal, 5.08 mm | THT |
| F1 | Fuse 1 A slow-blow, radial TR5 (or a pigtail fuse) | 5.08 mm |
| RV1 | Varistor 10D561K | 7.5 mm |
| D1 | 1N5817 Schottky | DO-41, 10.16 mm |
| C1 | 470 uF 10 V electrolytic, 8 mm can | 3.5 mm pitch |
| C3 | 10 uF ceramic | 1206 |
| C4 | 100 nF ceramic | 1206 |
| R1 | 330 ohm | 1206 |
| SW1 | 6 x 6 mm tactile switch | THT |
| J3 | 1 x 7 male header, 2.54 mm | THT |
| J5, J6, J7 | 1 x 2 male header, 2.54 mm | THT |
| - | 4-channel AC dimmer module (RobotDyn style, 3.3 V logic) | off board |

## Before you order: check these
1. **Hi-Link pin spacing.** The footprint comes from the HLK-10M05 datasheet (V1.5), which allows ±8 % on pin
   pitch. Measure your module: the two AC pins are 7.8 mm apart, the two DC pins 22.8 mm apart, and the AC
   end to the DC end is 42.5 mm. If yours differs, change the `PS1` lines in `gen_pcb.py` and run it again.
2. **Ra-02 is soldered directly**, not on the 2.54 mm adapter used on the perfboard. Pin 1 is bottom-right
   (marked with a dot and "1"). Nothing else connects to the antenna, so plug an IPEX 433 MHz antenna lead
   into the module.
3. **Mounting holes:** use nylon screws or standoffs on the two left-hand holes because they are near the mains zone.

## Design rules used (checked by gen_pcb.py)
- 0.2 mm copper clearance minimum (0.3 mm signal tracks, 0.5 mm 3V3, 0.8-1.0 mm 5 V/VSYS)
- mains copper ≥ 8 mm from any low-voltage copper (no LV copper or pour left of x = 26.5 mm)
- L to N ≥ 2.5 mm; mains tracks 1.5 mm wide, top layer only
- bottom layer = GND pour (0.4 mm clearance, thermal spokes on GND through-hole pads)
- no tracks under the Ra-02 on the top layer

The board was not checked in KiCad. The Gerbers were parsed with gerbonara as an independent check
(all layers and both drill files are read and identified correctly).
