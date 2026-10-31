"""Draufsicht des Ioniq 6 fuer die Auto-Seite: aus Marcs Blaupause (KI-Bild,
nur fuer den Eigengebrauch) die Strichzeichnung von oben ausschneiden und als
Linienstaerke je Pixel (0 = Hintergrund, 1..255 = Karosserie) nach car_top_img.h
schreiben, dazu die Flaechen von Kofferraumklappe und Motorhaube.
Aufruf: car_top_img.py <blaupause.png> <ausgabe.h> [vorschau.png]"""
import sys
from PIL import Image, ImageOps

W = 500  # Breite auf dem Display
src = Image.open(sys.argv[1]).convert("L")
top = src.crop((30, 540, 735, 835))  # Feld "TOP VIEW" ohne Beschriftung
# Linien hell auf dunkel; blasse Hilfslinien (Mittellinie) fallen weg
ink = top.point(lambda g: 0 if g > 225 else min(255, (225 - g) * 255 // 150))
bbox = ink.point(lambda v: 255 if v > 40 else 0).getbbox()
ink = ink.crop(bbox)
h = round(ink.height * W / ink.width)
ink = ink.resize((W, h), Image.LANCZOS)
# Karosserie = alles, was die Umrisslinie einschliesst: von aussen fluten
from PIL import ImageDraw
wall = ink.point(lambda v: 255 if v > 60 else 0).convert("RGB")
wall = wall.crop((-2, -2, W + 2, h + 2))
ImageDraw.floodfill(wall, (0, 0), (255, 0, 0))
outside = wall.crop((2, 2, W + 2, h + 2)).getchannel("G").point(lambda v: 0 if v else 255)  # rot = aussen
red = wall.crop((2, 2, W + 2, h + 2))
px, ipx = red.load(), ink.load()
out = Image.new("L", (W, h))
op = out.load()
for y in range(h):
    for x in range(W):
        r, g, b = px[x, y]
        is_out = (r, g, b) == (255, 0, 0)
        # 0 = Hintergrund, 1..255 = Karosserie mit Linienstaerke (Rand zaehlt mit)
        op[x, y] = 0 if is_out and ipx[x, y] < 40 else max(1, ipx[x, y])
ink = out
data = ink.tobytes()

# Klappen (Kofferraum, Motorhaube): Flaeche innerhalb ihrer Fugen, je Zeile
# als x-Bereich, 2 Pixel ueber die Fuge hinaus
from PIL import ImageFilter
def flap(seeds):
    """seeds: (x, y, Schwelle) - Flaechen bis zu Linien ueber der Schwelle, vereint"""
    m = Image.new("L", (W, h))
    mp = m.load()
    for sx, sy, th in seeds:
        wall = ink.point(lambda v: 255 if v > th else 0).convert("RGB")
        ImageDraw.floodfill(wall, (sx, sy), (255, 0, 0))
        wp = wall.load()
        for y in range(h):
            for x in range(W):
                if wp[x, y] == (255, 0, 0):
                    mp[x, y] = 255
    m = m.filter(ImageFilter.MaxFilter(5)).load()
    rows = []
    for y in range(h):
        xs = [x for x in range(W) if m[x, y]]
        rows.append((min(xs), max(xs)) if xs else (0, -1))
    return rows
# Tueren: Blech (Schwelle 60) und Seitenscheibe (Schwelle 150) zusammen
FLAPS = {"TRUNK": [(455, 100, 60)], "HOOD": [(60, 100, 60)],
         "DOOR_FL": [(205, 192, 60), (230, 176, 150)], "DOOR_FR": [(205, 14, 60), (230, 30, 150)],
         "DOOR_RL": [(325, 192, 60), (330, 176, 150)], "DOOR_RR": [(325, 14, 60), (330, 30, 150)]}
with open(sys.argv[2], "w") as f:
    f.write("// Ioniq 6 von oben (vorn links), Strichzeichnung aus Marcs Blaupause (KI-Bild,\n"
            "// nur fuer den Eigengebrauch). Je Pixel 0 = Hintergrund, sonst Karosserie mit Linienstaerke 1..255.\n"
            "// Erzeugt mit esp32/tools/car_top_img.py.\n#pragma once\n#include <stdint.h>\n")
    f.write(f"constexpr int CAR_W = {W}, CAR_H = {h};\nconst uint8_t CAR_TOP[] = {{\n")
    for i in range(0, len(data), 32):
        f.write("  " + ",".join(str(b) for b in data[i:i + 32]) + ",\n")
    f.write("};\n")
    for name, seed in FLAPS.items():
        rows = flap(seed)
        f.write(f"// {name}: je Zeile erstes und letztes Pixel der Klappe (x1 < x0 = keine)\n")
        f.write(f"const int16_t CAR_{name}[CAR_H][2] = {{\n")
        for i in range(0, h, 8):
            f.write("  " + "".join(f"{{{a},{b}}}," for a, b in rows[i:i + 8]) + "\n")
        f.write("};\n")
if len(sys.argv) > 3:
    ink.save(sys.argv[3])
print(W, h)
