#!/usr/bin/env python3
# Rechnet das Fahrgestell-Bild der Android-App in ein RGB565-Feld fuer den ESP
# um (esp32/ioniq6_display/chassis_img.h). Aufruf aus dem Repo-Wurzelordner.
from PIL import Image

H = 290
ch = Image.open('android/app/src/main/res/drawable-nodpi/chassis.webp').convert('RGBA')
W = round(ch.width * H / ch.height)
ch = ch.resize((W, H), Image.LANCZOS)
bg = Image.new('RGBA', (W, H), (11, 20, 26, 255))  # Hintergrund der Seiten
bg.alpha_composite(ch)
px = bg.convert('RGB').load()
vals = []
for y in range(H):
    for x in range(W):
        r, g, b = px[x, y]
        vals.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
out = ['// Fahrgestell des Ioniq 6 von oben (vorn links), aus dem Hyundai-Pressefoto der',
       '// Android-App (android/.../chassis.webp), auf den Hintergrund gerechnet, RGB565.',
       '// Nur fuer den Eigengebrauch. Erzeugt mit esp32/tools/chassis_img.py.',
       '#pragma once', '#include <stdint.h>', f'constexpr int CHASSIS_W = {W}, CHASSIS_H = {H};',
       'const uint16_t CHASSIS[] = {']
for i in range(0, len(vals), 16):
    out.append('  ' + ','.join(f'0x{v:04X}' for v in vals[i:i + 16]) + ',')
out.append('};')
open('esp32/ioniq6_display/chassis_img.h', 'w').write('\n'.join(out) + '\n')
