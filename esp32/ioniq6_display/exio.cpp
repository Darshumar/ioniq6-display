#include "exio.h"

#include <Arduino.h>
#include <Wire.h>

#include "board.h"

namespace exio {

namespace {
SemaphoreHandle_t mux = nullptr;
// Startzustand: Touch und Display aus dem Reset, Licht an, SD-Karte
// dauerhaft ausgewaehlt (einziges Geraet am Bus), USB statt CAN
uint8_t out = (1 << TP_RST) | (1 << DISP) | (1 << LCD_RST);

void write_reg(uint8_t addr, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(v);
  Wire.endTransmission();
}
}  // namespace

SemaphoreHandle_t i2c() {
  if (!mux) mux = xSemaphoreCreateMutex();
  return mux;
}

void begin() {
  static bool done = false;
  if (done) return;
  done = true;
  i2c();
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  xSemaphoreTake(mux, portMAX_DELAY);
  write_reg(0x24, 0x01);  // IO0-7 als Ausgaenge
  // Sauberer Display-Reset wie in den Waveshare-Beispielen: nach kaltem
  // Einschalten blieb die Schrift sonst ausgefranst, bis zum Neustart (Marc 29.10.)
  write_reg(0x38, out & ~(1 << LCD_RST));
  delay(20);
  write_reg(0x38, out);
  xSemaphoreGive(mux);
  delay(120);
}

void set(Pin pin, bool high) {
  uint8_t v = high ? out | (1 << pin) : out & ~(1 << pin);
  xSemaphoreTake(i2c(), portMAX_DELAY);
  out = v;
  write_reg(0x38, out);
  xSemaphoreGive(i2c());
}

}  // namespace exio
