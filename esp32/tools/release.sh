#!/bin/sh
# Legt eine gebaute Firmware ab: das komplette Abbild fuers Flashen per Kabel
# (bei 0x0) und die App allein fuers Online-Update.
# Aufruf: tools/release.sh <build-Ordner von arduino-cli> <esptool.py> <boot_app0.bin> [Zielordner]
# Zielordner: Standard esp32/firmware/ws43, muss zu FW_DIR in board.h passen.
set -e
B=$1; ESPTOOL=$2; BOOT_APP0=$3
OUT=${4:-$(dirname "$0")/../firmware/ws43}
mkdir -p "$OUT"
V=$(sed -n 's/^#define FW_VERSION \([0-9]*\).*/\1/p' "$(dirname "$0")/../ioniq6_display/version.h")
# Schutz vor einer Endlosschleife beim Online-Update: die App muss genau die
# Version enthalten, die version.txt ankuendigt (version.h vor dem Bauen hochzaehlen)
if ! grep -aq "IONIQ6_FW_VERSION=$V;" "$B/ioniq6_display.ino.bin"; then
  echo "FEHLER: $B/ioniq6_display.ino.bin ist nicht Version $V - nach dem Hochzaehlen neu bauen" >&2
  exit 1
fi
python3 "$ESPTOOL" --chip esp32s3 merge_bin -o "$OUT/ioniq6_display_merged.bin" \
  --flash_mode keep --flash_freq keep --flash_size keep \
  0x0 "$B/ioniq6_display.ino.bootloader.bin" 0x8000 "$B/ioniq6_display.ino.partitions.bin" \
  0xe000 "$BOOT_APP0" 0x10000 "$B/ioniq6_display.ino.bin"
cp "$B/ioniq6_display.ino.bin" "$OUT/ioniq6_display_app.bin"
echo "$V" > "$OUT/version.txt"
echo "Version $V abgelegt"
