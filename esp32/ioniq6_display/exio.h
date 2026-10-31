// CH422G-Portexpander des Waveshare 4.3" (I2C): Touch-Reset, Hintergrundlicht,
// Display-Reset, CS der SD-Karte, USB/CAN-Umschaltung.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace exio {

enum Pin { TP_RST = 1, DISP = 2, LCD_RST = 3, SD_CS = 4, USB_SEL = 5 };

void begin();  // startet auch den I2C-Bus
void set(Pin pin, bool high);
// Der I2C-Bus wird vom Touch-Task und vom Hauptprogramm benutzt
SemaphoreHandle_t i2c();

}  // namespace exio
