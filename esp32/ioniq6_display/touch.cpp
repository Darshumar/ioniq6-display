#include "touch.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

#include <math.h>

#include "board.h"
#include "exio.h"
#include "i18n.h"

namespace {

constexpr int PIN_TP_INT = 4;
uint8_t ADDR = 0x5D;  // GT911, je nach Reset auch 0x14
volatile bool tapped = false, long_pressed = false, secret = false;
volatile int swiped = 0;  // -1 = nach rechts gewischt (naechste Seite), +1 = nach links
volatile int vswiped = 0;  // -1 = nach oben gewischt, +1 = nach unten
volatile int tap_x = -1, tap_y = -1;  // Stelle des letzten Tippens, -1 = BOOT-Taste
volatile int raw_x = -1, raw_y = -1;  // Rohwerte des Chips dazu, fuer die Fehlersuche
bool flip = false;
bool y_inv = false;  // y gespiegelt? Wird an der Menue-Schaltflaeche gelernt
volatile int tap_rx = -1, tap_ry = -1;  // Rohwerte des letzten Tippens, gemittelt
// Zuletzt gelesene Chip-Antwort, fuer den Touch-Test
volatile uint8_t live_d[8];
volatile int live_x = -1, live_y = -1;
volatile bool live_touch = false;

// Kalibrierung aus dem Touch-Test: Bildschirm-x = (u - u0) / us mit u = Roh-x
// (bei swap Roh-y), Bildschirm-y entsprechend aus v. Gilt fuer die Drehung,
// in der kalibriert wurde (cal_flip).
struct Cal {
  uint32_t magic;
  bool swap, flip;
  float u0, us, v0, vs;
};
constexpr uint32_t CAL_MAGIC = 0x70C4CA11;
Cal cal = {};
constexpr uint32_t LONG_MS = 3000;
constexpr int SWIPE_PX = 50;  // so weit muss der Finger seitlich wandern

// GT911: Register 0x814E = Status (Bit 7 neue Daten, Bits 0-3 Anzahl Finger),
// ab 0x8150 x/y des ersten Fingers. Liefert schon Bildschirmkoordinaten
// in Panel-Richtung (800x480 quer).
bool gt_read(uint16_t reg, uint8_t* d, int n) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(ADDR, (uint8_t)n) != n) return false;
  for (int i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

void gt_write(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  Wire.endTransmission();
}

int touch_read(int& x, int& y) {
  static int last = 0, lx = 0, ly = 0;
  uint8_t st, d[8] = {};
  xSemaphoreTake(exio::i2c(), portMAX_DELAY);
  bool ok = gt_read(0x814E, &st, 1);
  if (ok && (st & 0x80)) {
    int n = st & 0x0F;
    if (n > 0 && n <= 5 && gt_read(0x8150, d, 4)) {
      lx = d[0] | (d[1] << 8);
      ly = d[2] | (d[3] << 8);
    }
    last = n <= 5 ? n : 0;
    gt_write(0x814E, 0);  // Puffer freigeben
  }
  xSemaphoreGive(exio::i2c());
  if (!ok) return -1;
  // Ohne neue Daten gilt der letzte Stand (der Chip meldet nur Aenderungen)
  d[0] = st;
  for (int i = 0; i < 8; i++) live_d[i] = d[i];
  if (last == 0) {
    live_touch = false;
    return 0;
  }
  raw_x = lx;
  raw_y = ly;
  x = flip ? SCREEN_W - 1 - lx : lx;
  y = flip ? SCREEN_H - 1 - ly : ly;
  x = x < 0 ? 0 : (x >= SCREEN_W ? SCREEN_W - 1 : x);
  y = y < 0 ? 0 : (y >= SCREEN_H ? SCREEN_H - 1 : y);
  live_x = x;
  live_y = y;
  live_touch = true;
  return last;
}

void task(void*) {
  int x = 0, y = 0;
  // Ein Finger, der schon beim Start liegt (nach dem Halten fuer die
  // Einstellungen), zaehlt nicht als Tippen
  bool was_touch = touch_read(x, y) > 0, was_boot = PIN_BOOT >= 0 && digitalRead(PIN_BOOT) == LOW, long_sent = true;
  bool moved = false;
  int x0 = x, x1 = x, y0 = y, y1 = y;
  uint32_t since = 0, last_tap = 0;
  int quick = 0;  // schnell hintereinander getippt
  long sum_rx = 0, sum_ry = 0;
  int samples = 0;
  auto tap = [&](uint32_t now, int tx, int ty) {
    tap_x = tx;
    tap_y = ty;
    tapped = true;
    quick = now - last_tap < 600 ? quick + 1 : 1;
    last_tap = now;
    if (quick == 5) {  // fuenfmal schnell = Demo-Modus
      secret = true;
      quick = 0;
    }
  };
  for (;;) {
    uint32_t now = millis();
    int n = touch_read(x, y);
    bool touch = n < 0 ? was_touch : n > 0;  // Lesefehler: Zustand beibehalten
    bool boot = PIN_BOOT >= 0 && digitalRead(PIN_BOOT) == LOW;
    // BOOT-Taste: Druck = Tippen
    if (boot && !was_boot) {
      tap(now, -1, -1);
      since = now;
      long_sent = false;
    }
    // Display: erst beim Loslassen entscheiden, ob getippt oder gewischt wurde
    if (touch && !was_touch) {
      x0 = x1 = x;
      y0 = y1 = y;
      moved = false;
      since = now;
      long_sent = false;
      sum_rx = sum_ry = samples = 0;
    }
    if (touch) {
      if (n > 0) {
        sum_rx += raw_x;
        sum_ry += raw_y;
        samples++;
      }
      x1 = x;
      y1 = y;
      if (abs(x1 - x0) >= SWIPE_PX || abs(y1 - y0) >= SWIPE_PX) moved = true;
    }
    if (!touch && was_touch && !long_sent) {
      if (moved && abs(y1 - y0) > abs(x1 - x0)) vswiped = y1 > y0 ? 1 : -1;
      else if (moved) swiped = x1 > x0 ? -1 : 1;  // nach rechts = weiter (so gewuenscht)
      else {
        if (samples) {
          tap_rx = sum_rx / samples;
          tap_ry = sum_ry / samples;
        }
        tap(now, x0, y0);
      }
    }
    if ((touch || boot) && !long_sent && !moved && now - since >= LONG_MS) long_pressed = long_sent = true;
    was_touch = touch;
    was_boot = boot;
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

}  // namespace

namespace touch {

void begin(bool flipped) {
  flip = flipped;
  if (PIN_BOOT >= 0) pinMode(PIN_BOOT, INPUT_PULLUP);
  exio::begin();  // I2C-Bus (doppelter Aufruf schadet nicht)
  // GT911-Reset: INT waehrend des Resets low = Adresse 0x5D
  pinMode(PIN_TP_INT, OUTPUT);
  digitalWrite(PIN_TP_INT, LOW);
  exio::set(exio::TP_RST, false);
  delay(10);
  exio::set(exio::TP_RST, true);
  delay(60);
  pinMode(PIN_TP_INT, INPUT);
  xSemaphoreTake(exio::i2c(), portMAX_DELAY);
  Wire.beginTransmission(0x5D);
  if (Wire.endTransmission() != 0) ADDR = 0x14;
  xSemaphoreGive(exio::i2c());
  xTaskCreatePinnedToCore(task, "touch", 3072, nullptr, 1, nullptr, 0);
}

bool take_tap(int* x, int* y) {
  if (!tapped) return false;
  tapped = false;
  if (x) *x = tap_x;
  if (y) *y = tap_y;
  return true;
}

void set_y_inverted(bool inv) { y_inv = inv; }
bool y_inverted() { return y_inv; }
void last_raw(int& x, int& y) {
  x = raw_x;
  y = raw_y;
}

int take_vswipe() {
  int s = vswiped;
  vswiped = 0;
  return s;
}

int take_swipe() {
  int s = swiped;
  swiped = 0;
  return s;
}

bool take_secret() {
  if (!secret) return false;
  secret = false;
  return true;
}

bool take_long_press() {
  if (!long_pressed) return false;
  long_pressed = false;
  return true;
}

void last_tap_raw(int& x, int& y) {
  x = tap_rx;
  y = tap_ry;
}

bool live(Live& l) {
  for (int i = 0; i < 8; i++) l.d[i] = live_d[i];
  l.x = live_x;
  l.y = live_y;
  l.rx = raw_x;
  l.ry = raw_y;
  l.touch = live_touch;
  return l.touch;
}

// Punkte 0..3: links oben, rechts oben, rechts unten, links unten
bool calibrate(const int sx[4], const int sy[4], const int rx[4], const int ry[4]) {
  // Aendert sich beim Weg nach rechts eher Roh-y als Roh-x, sind die Achsen vertauscht
  int dxr = abs(rx[1] - rx[0]) + abs(rx[2] - rx[3]);
  int dyr = abs(ry[1] - ry[0]) + abs(ry[2] - ry[3]);
  Cal c;
  c.magic = CAL_MAGIC;
  c.swap = dyr > dxr;
  c.flip = flip;
  int u[4], v[4];
  for (int i = 0; i < 4; i++) {
    u[i] = c.swap ? ry[i] : rx[i];
    v[i] = c.swap ? rx[i] : ry[i];
  }
  float ul = (u[0] + u[3]) / 2.0f, ur = (u[1] + u[2]) / 2.0f;
  float xl = (sx[0] + sx[3]) / 2.0f, xr = (sx[1] + sx[2]) / 2.0f;
  float vt = (v[0] + v[1]) / 2.0f, vb = (v[2] + v[3]) / 2.0f;
  float yt = (sy[0] + sy[1]) / 2.0f, yb = (sy[2] + sy[3]) / 2.0f;
  c.us = (ur - ul) / (xr - xl);
  c.u0 = ul - xl * c.us;
  c.vs = (vb - vt) / (yb - yt);
  c.v0 = vt - yt * c.vs;
  // Plausibel? Massstab nicht winzig oder riesig, und die beiden Punkte
  // einer Seite liegen beieinander (sonst wurde daneben getippt)
  auto ok = [](float s) { return fabsf(s) > 0.2f && fabsf(s) < 20; };
  if (!ok(c.us) || !ok(c.vs)) return false;
  if (fabsf(u[0] - u[3]) > 0.3f * fabsf(ur - ul) || fabsf(u[1] - u[2]) > 0.3f * fabsf(ur - ul)) return false;
  if (fabsf(v[0] - v[1]) > 0.3f * fabsf(vb - vt) || fabsf(v[2] - v[3]) > 0.3f * fabsf(vb - vt)) return false;
  cal = c;
  return true;
}

bool calibrated() {
  return true;  // GT911 liefert Bildschirmkoordinaten
}

void describe(char* buf, int n) {
  if (!calibrated()) {
    snprintf(buf, n, "%s", T("nicht kalibriert", "not calibrated"));
    return;
  }
  const char* mir = T("gespiegelt", "mirrored");
  snprintf(buf, n, T("%sx %s, y %s, Massstab %.2f / %.2f", "%sx %s, y %s, scale %.2f / %.2f"),
           cal.swap ? T("Achsen getauscht, ", "axes swapped, ") : "", cal.us < 0 ? mir : "normal",
           cal.vs < 0 ? mir : "normal", fabsf(cal.us), fabsf(cal.vs));
}

bool load() {
  Preferences p;
  p.begin("touch", true);
  Cal c = {};
  bool ok = p.getBytes("cal", &c, sizeof c) == sizeof c && c.magic == CAL_MAGIC;
  p.end();
  if (ok) cal = c;
  return ok;
}

void save() {
  Preferences p;
  p.begin("touch", false);
  p.putBytes("cal", &cal, sizeof cal);
  p.end();
}

void forget() {
  cal = {};
  Preferences p;
  p.begin("touch", false);
  p.remove("cal");
  p.end();
}

}  // namespace touch
