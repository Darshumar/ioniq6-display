// Touch des Waveshare 4.3" (GT911, I2C an SDA 8 / SCL 9).
// Wischen nach rechts/links wechselt die Seite vor/zurueck, Tippen (oder BOOT)
// schaltet weiter, 3 s gedrueckt halten oeffnet die Einstellungen (WLAN fuer Updates),
// fuenfmal schnell tippen schaltet den Demo-Modus ein oder aus.
#pragma once
#include <stdint.h>

namespace touch {

void begin(bool flipped);  // startet einen eigenen Task, der Touch und Taste abfragt
bool take_tap(int* x = nullptr, int* y = nullptr);  // true, wenn getippt wurde; x/y = Stelle, -1 = BOOT-Taste
bool take_long_press();  // true, wenn seitdem 3 s lang gedrueckt wurde
int take_swipe();        // -1 = nach rechts gewischt (naechste Seite), +1 = nach links, 0 = nicht
int take_vswipe();       // -1 = nach oben gewischt, +1 = nach unten, 0 = nicht
bool take_secret();
// Ausrichtung der y-Achse des Touch-Chips (gemerkt in Preferences touch/yinv)
void set_y_inverted(bool inv);
bool y_inverted();
void last_raw(int& x, int& y);  // Rohwerte der letzten Beruehrung
// Rohwerte des letzten Tippens, gemittelt ueber die ganze Beruehrung
void last_tap_raw(int& x, int& y);
// Was der Chip gerade meldet (fuer den Touch-Test); true = Finger liegt auf
struct Live {
  uint8_t d[8];   // Antwort des Chips
  int x, y;       // umgerechnete Stelle
  int rx, ry;     // Rohwerte
  bool touch;
};
bool live(Live& l);
// Kalibrierung aus dem Touch-Test: Bildschirmpunkte sx/sy und die dort
// gemessenen Rohwerte rx/ry (4 Ecken). false = Werte passen nicht zusammen.
bool calibrate(const int sx[4], const int sy[4], const int rx[4], const int ry[4]);
bool calibrated();
void describe(char* buf, int n);  // Kalibrierung als Text
bool load();   // aus Preferences touch/cal, true = vorhanden
void save();
void forget();

}  // namespace touch
