#!/bin/sh
# Rendert die sieben Seiten (plus Unterseiten) der ESP-Anzeige mit dem echten ui.cpp am PC nach
# docs/esp32_*.png. Aufruf: render.sh <Ordner der Arduino-Bibliotheken>
# (darin GFX_Library_for_Arduino und U8g2). Braucht g++, gcc, Python mit Pillow.
set -e
LIB=$1
HERE=$(cd "$(dirname "$0")" && pwd)
UI=$HERE/../../ioniq6_display
DOCS=$HERE/../../../docs
G=$LIB/GFX_Library_for_Arduino/src
U=$LIB/U8g2/src/clib
OUT=$(mktemp -d)
# ui.cpp ohne die Display-Hardware: der Puffer geht an eine Bilddatei
python3 - "$UI/ui.cpp" "$OUT/ui_sim.cpp" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'Arduino_ESP32RGBPanel\* rgb = new Arduino_ESP32RGBPanel\(.*?\);\n', '', s, flags=re.S)
s = re.sub(r'Arduino_GFX\* panel = new Arduino_RGB_Display\(.*?\);\n', 'Arduino_G* panel = make_png_out();\n', s, flags=re.S)
s = s.replace('namespace {\n', 'Arduino_G* make_png_out();\nnamespace {\n', 1)
open(sys.argv[2], 'w').write(s)
PY
gcc -c -O1 -I"$U" "$U/u8g2_fonts.c" -o "$OUT/u8g2_fonts.o"
g++ -std=c++17 -O1 -w -I"$HERE/shim" -I"$G" -I"$U" -I"$UI" "$HERE/sim_main.cpp" "$OUT/ui_sim.cpp" "$UI/decode.cpp" "$UI/demo.cpp" -DBOARD_WS43 \
  "$G/Arduino_GFX.cpp" "$G/Arduino_G.cpp" "$G/canvas/Arduino_Canvas.cpp" "$G/Arduino_DataBus.cpp" "$OUT/u8g2_fonts.o" -o "$OUT/sim"
cd "$OUT" && ./sim
mkdir -p "$OUT/en" && (cd "$OUT/en" && SIM_EN=1 ../sim)
python3 - "$OUT" "$DOCS" <<'PY'
import os, sys
from PIL import Image
out, docs = sys.argv[1:]
def grid(names, path, cols=2):
    ims = [Image.open(f"{out}/{n}.ppm") for n in names]
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + 20) + 20, rows * (h + 20) + 20), (40, 40, 40))
    for i, im in enumerate(ims):
        sheet.paste(im, (20 + (i % cols) * (w + 20), 20 + (i // cols) * (h + 20)))
    sheet.save(path)
names = ["fahren", "akku", "laden", "antrieb", "fahrten", "temperaturen", "auto"]
pages = [f"page{i + 1}_{n}" for i, n in enumerate(names)]
for n, f in zip(names, pages):
    Image.open(f"{out}/{f}.ppm").save(f"{docs}/esp32_{n}.png")
grid(pages, f"{docs}/esp32_seiten.png")
grid(["suche", "menue", "fahrten_menue"], f"{docs}/esp32_menue.png")
grid(["touch1", "touch2"], f"{docs}/esp32_touchtest.png")
grid(["demo0", "demo2"], f"{docs}/esp32_demo.png")
Image.open(f"{out}/g.ppm").save(f"{docs}/esp32_g.png")  # Unterseite von Antrieb
Image.open(f"{out}/zellen.ppm").save(f"{docs}/esp32_zellen.png")  # Unterseite von Akku
grid(["zellen", "g"], f"{docs}/esp32_unterseiten.png")  # versteckte Unterseiten
grid(["page7_auto", "auto_offen"], f"{docs}/esp32_auto.png")  # Tueren und Licht
grid(["page3_laden", "laden_ac"], f"{docs}/esp32_laden_dc_ac.png")  # Lade-Seite DC und AC  # Demo-Modus (Fahren und Laden)
Image.open(f"{out}/warnung.ppm").save(f"{docs}/esp32_warnung.png")
grid(["helligkeit", "gedimmt_40"], f"{docs}/esp32_helligkeit.png")
# Englisch (Sprachwahl im Menue)
grid([f"en/{p}" for p in pages], f"{docs}/esp32_seiten_en.png")
grid(["en/suche", "en/menue", "en/fahrten_menue"], f"{docs}/esp32_menue_en.png")
# Alle Demo-Seiten einzeln nur auf Wunsch (DEMO_OUT=<Ordner>)
d = os.environ.get("DEMO_OUT")
if d:
    for p in range(9):  # 7 = G-Kraft, 8 = Zellen
        Image.open(f"{out}/demo{p}.ppm").save(f"{d}/demo{p}.png")
    Image.open(f"{out}/warnung.ppm").save(f"{d}/warnung.png")
PY
echo "Bilder in $DOCS"
