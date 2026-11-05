// Ioniq 6 OBD-Anzeige: Waveshare ESP32-S3-Touch-LCD-4.3 + Vgate iCar 2 BLE.
// Gleiche Abfragen und Dekodierung wie obd_reader.py auf dem PC.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>

// Unterspannungswaechter mit Core 3: er loeste beim Start am PC-USB schon beim
// Einschalten aus (Neustart-Schleife, rtc_brownout_isr_handler), auch wenn er
// direkt danach abgeschaltet wurde. Deshalb wird er gar nicht erst eingerichtet:
// mit -Wl,--wrap=esp_brownout_init gebaut landet der Aufruf hier.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
extern "C" void __wrap_esp_brownout_init(void) {}

// Bounce-Buffer des RGB-Panels: die Interrupts (GDMA-EOF und VSYNC) kopieren
// aus dem Framebuffer im PSRAM. Waehrend Flash-Schreibzugriffen (NVS, OTA,
// Funk-Kalibrierung) ist der Cache aus, ein Zugriff dann = Absturz ("Cache
// disabled but cached memory region accessed"). Deshalb sperren wir diese
// Interrupts fuer die Dauer des Flash-Zugriffs; das Panel zeigt so lange die
// alten Zeilen weiter und faengt sich beim naechsten VSYNC wieder.
// Gebaut mit -Wl,--wrap=spi_flash_disable_interrupts_caches_and_other_cpu
// und -Wl,--wrap=spi_flash_enable_interrupts_caches_and_other_cpu.
#include <soc/gdma_struct.h>
#include <soc/lcd_cam_struct.h>
static DRAM_ATTR uint32_t s_gdma_ena[5];
static DRAM_ATTR uint32_t s_lcd_ena;
static DRAM_ATTR int s_flash_depth;
static DRAM_ATTR volatile uint32_t s_flash_ops;  // Zaehler fuer die Diagnose
extern "C" void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern "C" void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);
extern "C" IRAM_ATTR void __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void) {
  s_flash_ops++;
  if (s_flash_depth++ == 0) {
    s_lcd_ena = LCD_CAM.lc_dma_int_ena.val;
    LCD_CAM.lc_dma_int_ena.val = s_lcd_ena & ~1u;  // lcd_vsync_int_ena
    for (int ch = 0; ch < 5; ch++) {
      s_gdma_ena[ch] = GDMA.channel[ch].out.int_ena.val;
      if (GDMA.channel[ch].out.peri_sel.sel == 5)  // LCD_CAM
        GDMA.channel[ch].out.int_ena.val = 0;
    }
  }
  __real_spi_flash_disable_interrupts_caches_and_other_cpu();
}
extern "C" IRAM_ATTR void __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void) {
  __real_spi_flash_enable_interrupts_caches_and_other_cpu();
  if (--s_flash_depth == 0) {
    for (int ch = 0; ch < 5; ch++)
      if (GDMA.channel[ch].out.peri_sel.sel == 5)
        GDMA.channel[ch].out.int_ena.val = s_gdma_ena[ch];
    LCD_CAM.lc_dma_int_ena.val = s_lcd_ena;
  }
}
#endif

#include "board.h"
#include "decode.h"
#include "demo.h"
#include "elm_ble.h"
#include "ota.h"
#include "sdlog.h"
#include "settings.h"
#include "i18n.h"
#include "touch.h"
#include "trips.h"
#include "ui.h"
#include "version.h"

ElmBle elm;
Consumption cons;
ChargeSession charge;
Tires tires;
Cells cells;
Vehicle veh;  // weitere Steuergeraete (Tempo, Klima, Ladesteuerung, Ladestatistik)
TripLog trip_log;
Trip trip;  // laufende Fahrt, wird am Fahrtende eingetragen
uint32_t trip_start_ms = 0;
BmsData last;
bool have_data = false;
bool link_ok = false;
int page = 0;

// Seitenwechsel per Tippen; wird auch zwischen den Abfragen aufgerufen,
// damit die Anzeige nicht erst nach dem ganzen Zyklus reagiert.
// Ueberlebt den Neustart: 3 s Druecken startet neu in die Einstellungen,
// damit dort kein Bluetooth neben dem WLAN laeuft.
RTC_NOINIT_ATTR uint32_t settings_request;
// Nach Strom an oder RESET startet der ESP einmal selbst neu (Panel dann
// eingeschwungen, Marc 29.10.); der eigentliche Grund bleibt fuers Menue hier
RTC_NOINIT_ATTR uint32_t cold_start;
constexpr uint32_t COLD_MAGIC = 0xC01D0000;
esp_reset_reason_t boot_reason = ESP_RST_UNKNOWN;
constexpr uint32_t SETTINGS_MAGIC = 0x5E77125;

[[noreturn]] void run_demo();
[[noreturn]] void sleep_display();
[[noreturn]] void power_off();
void target(const char* tx);
void elm_idle();
extern std::string cur_tx;
bool at2_active = false, at2_disabled = false;  // ATAT2, siehe at2_check
bool menu_closed = false;  // Menue war offen: Statusbildschirm neu zeichnen

// Touch-Test: vier Kreuze antippen, daraus Richtung und Massstab des
// Touch-Chips bestimmen und merken, dann ein Pruefbild zum Malen.
// Laeuft beim ersten Start von selbst; BOOT-Taste bricht ab.
void touch_test() {
  static const int SX[4] = {40, 280, 280, 40}, SY[4] = {80, 80, 400, 400};
  const char* hint = "";
  for (;;) {
    int rx[4], ry[4];
    for (int i = 0; i < 4; i++) {
      ui::touch_target(i, SX[i], SY[i], hint);
      touch::take_tap();
      touch::take_swipe();
      for (;;) {
        int x, y;
        if (touch::take_tap(&x, &y)) {
          if (x < 0) {  // BOOT-Taste: abbrechen, alte Einstellung bleibt
            Preferences p;
            p.begin("touch", false);
            p.putBool("skip", true);
            p.end();
            touch::take_long_press();
            return;
          }
          touch::last_tap_raw(rx[i], ry[i]);
          Serial.printf("Touch-Test Punkt %d (%d/%d): Chip %d/%d\n", i + 1, SX[i], SY[i], rx[i], ry[i]);
          break;
        }
        touch::take_swipe();  // verwischt: nochmal
        delay(20);
      }
    }
    if (!touch::calibrate(SX, SY, rx, ry)) {
      hint = T("Passte nicht, bitte nochmal genau", "Did not fit, please try again precisely");
      continue;
    }
    touch::save();
    char desc[64];
    touch::describe(desc, sizeof desc);
    Serial.printf("Touch kalibriert: %s\n", desc);
    // Pruefbild, bis "Fertig" (unten) oder "Nochmal" (oben) getippt wird
    touch::Live l;
    touch::live(l);
    ui::touch_check(true, l.d, false, 0, 0, 0, 0, desc);
    touch::take_tap();
    bool again = false;
    for (uint32_t last = 0;;) {
      bool on = touch::live(l);
      if (on || millis() - last > 300) {
        ui::touch_check(false, l.d, on, l.x, l.y, l.rx, l.ry, desc);
        last = millis();
      }
      int x, y;
      if (touch::take_tap(&x, &y)) {
        if (x < 0 || y >= 480 - 90) break;
        if (y < 56) {
          again = true;
          break;
        }
      }
      touch::take_swipe();
      delay(30);
    }
    touch::take_long_press();
    touch::take_secret();
    if (!again) return;
    hint = "";
  }
}

// Grund des letzten Neustarts, kurz fuers Menue
const char* reset_name() {
  switch (boot_reason) {
    case ESP_RST_POWERON: return T("Strom an", "power on");
    case ESP_RST_BROWNOUT: return T("Unterspg.", "brownout");
    case ESP_RST_SW: return T("Neustart", "restart");
    case ESP_RST_PANIC: return T("Absturz", "crash");
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return T("Schlaf", "sleep");
    case ESP_RST_EXT: return "Reset";
    default: return "?";
  }
}

// Helligkeitsregler (Tag und Nacht): Tippen auf Minus/Plus/Balken zeigt die
// Helligkeit sofort, "Fertig" (oder 30 s ohne Beruehrung) merkt die Werte
void brightness_screen() {
  int day = ui::brightness(false), night = ui::brightness(true);
  ui::preview_brightness(day);
  ui::brightness_view(day, night);
  touch::take_tap();
  touch::take_swipe();
  for (uint32_t t0 = millis(); millis() - t0 < 30000;) {
    int x, y;
    if (touch::take_tap(&x, &y)) {
      t0 = millis();
      int r = x < 0 ? ui::BR_DONE : ui::brightness_hit(x, y, day, night);
      if (r == ui::BR_DONE) break;
      if (r >= 0) {
        ui::preview_brightness(r ? night : day);
        ui::brightness_view(day, night);
      }
    }
    if (touch::take_swipe()) break;
    delay(20);
  }
  ui::save_brightness(day, night);
  ui::auto_brightness(time(nullptr));
  settings::backup();
}

// Menue hinter der Schaltflaeche: Einstellungen, Fahrten, Demo
void open_menu(bool in_demo) {
  enum Act { Back, Bright, Language, Settings, Trips, Sleep, Touch, Demo, DemoEnd };
  std::vector<ui::ListItem> items;
  std::vector<Act> acts;
  auto add = [&](const char* t, std::string sub, Act a) {
    ui::ListItem it;
    it.title = t;
    it.sub = sub;
    items.push_back(it);
    acts.push_back(a);
  };
  add(T("Helligkeit", "Brightness"), std::string(T("Tag ", "Day ")) + std::to_string(ui::brightness(false)) +
                                         T(" %, Nacht ", " %, night ") + std::to_string(ui::brightness(true)) + " %",
      Bright);
  add("Sprache / Language", i18n::en() ? "English (tap: Deutsch)" : "Deutsch (Tippen: English)", Language);
#if LOG_UPLOAD
  add(T("Einstellungen", "Settings"), T("WLAN, Updates, Schlüssel", "Wi-Fi, updates, key"), Settings);
#else
  add(T("Einstellungen", "Settings"), T("WLAN, Updates", "Wi-Fi, updates"), Settings);
#endif
  add(T("Fahrten ansehen", "View trips"),
      std::to_string(trip_log.n) + T(" gespeichert; Tippen schließt", " saved; tap to close"), Trips);
  if (!in_demo) add(T("Schlafen", "Sleep"), T("Display aus; Berühren oder Auto an weckt", "display off; touch or car on wakes"), Sleep);
  if (!touch::calibrated() || touch::load())
    add("Touch-Test", T("Bildschirm neu ausrichten", "recalibrate the screen"), Touch);
  if (in_demo) add(T("Demo beenden", "End demo"), T("startet neu", "restarts"), DemoEnd);
  else add(T("Demo-Modus", "Demo mode"), T("simulierte Fahrt und Ladung, ohne Auto", "simulated drive and charge, no car"), Demo);
  add(T("Zurück", "Back"), "", Back);
  char footer[48];
  // Firmware und warum er zuletzt neu gestartet ist (Unterspannung = Einbruch beim Abziehen von USB)
  snprintf(footer, sizeof footer, "Firmware %d   Start: %s", FW_VERSION, reset_name());
  ui::choice_list(T("Menü", "Menu"), items, -1, footer);
  touch::take_tap();
  touch::take_swipe();
  Act act = Back;
  for (uint32_t t0 = millis(); millis() - t0 < 20000;) {
    if (touch::take_long_press()) act = Settings;
    int x, y;
    if (touch::take_tap(&x, &y)) {
      int i = x < 0 ? (int)acts.size() - 1 : ui::list_hit(y, -1, items.size());
      if (i >= 0) act = acts[i];
      if (i >= 0 || x < 0) break;
    }
    if (touch::take_swipe() || act != Back) break;
    delay(20);
  }
  menu_closed = true;
  switch (act) {
    case Bright:
      brightness_screen();
      open_menu(in_demo);
      break;
    case Language:
      i18n::set(i18n::en() ? i18n::DE : i18n::EN);
      settings::backup();
      open_menu(in_demo);
      break;
    case Settings:
      settings_request = SETTINGS_MAGIC;
      ESP.restart();
    case Trips: {
      ui::trips_view(trip_log);
      touch::take_tap();
      touch::take_swipe();
      // Tippen, Wischen oder nach 60 s zurueck
      for (uint32_t t0 = millis(); millis() - t0 < 60000;) {
        if (touch::take_tap() || touch::take_swipe()) break;
        delay(20);
      }
      open_menu(in_demo);
      break;
    }
    case Sleep:
      sleep_display();
    case Touch:
      touch_test();
      break;
    case Demo:
      run_demo();
    case DemoEnd:
      ESP.restart();
    case Back:
      break;
  }
}

// Tippen auf eine Seite oben = diese Seite; sonst nach rechts wischen oder
// tippen = naechste Seite, nach links = vorige
int next_page(int page, int sw, bool tap, int tx, int ty) {
  int hit = tap ? ui::tab_hit(tx, ty) : -1;
  if (hit >= 0) return hit;
  page = ui::tab_of(page);  // Unterseiten blaettern wie ihr Reiter
  return (page + (sw > 0 ? ui::PAGES - 1 : 1)) % ui::PAGES;
}

void handle_touch() {
  if (touch::take_long_press()) {
    settings_request = SETTINGS_MAGIC;
    ESP.restart();
  }
  // Versteckter Demo-Modus, nur solange noch keine Daten vom Auto kommen
  if (touch::take_secret() && !have_data) run_demo();
  int sw = touch::take_swipe();
  int vsw = touch::take_vswipe();
  int tx, ty;
  bool tap = touch::take_tap(&tx, &ty);
  // Statusbildschirm: die Schaltflaeche unten lernt die y-Richtung des Touch.
  // Kommt der Druck gespiegelt oben an, ist y vertauscht: merken und umstellen.
  if (tap && !have_data && tx >= 0 && ty < 90 && !touch::calibrated()) {
    touch::set_y_inverted(!touch::y_inverted());
    Preferences p;
    p.begin("touch", false);
    p.putBool("yinv", touch::y_inverted());
    p.end();
    ty = 479 - ty;
  }
  if (tap && ui::menu_hit(tx, ty, have_data)) {
    open_menu(false);
    if (have_data) ui::dashboard(last, cons, charge, tires, cells, veh, trip_log, link_ok, page);
    return;
  }
  if (!have_data && tap) {
    int b = ui::sleep_hit(tx, ty);
    if (b == 1) sleep_display();
    if (b == 2) power_off();
  }
  if (!have_data) {  // Statusbildschirm: keine Seiten zum Blaettern
    if (tap && tx >= 0) {
      int rx, ry;
      touch::last_raw(rx, ry);
      ui::touch_debug(tx, ty, rx, ry);
    }
    return;
  }
  // Versteckt (Marc 27.10.): hoch oder runter wischen = Unterseite und zurueck
  // (Antrieb <-> G-Kraft, Akku <-> Zellen)
  if (vsw && ui::vswipe_page(page) >= 0) {
    page = ui::vswipe_page(page);
    ui::dashboard(last, cons, charge, tires, cells, veh, trip_log, link_ok, page);
    return;
  }
  if (!sw && !tap) return;
  page = next_page(page, sw, tap, tx, ty);
  if (have_data) ui::dashboard(last, cons, charge, tires, cells, veh, trip_log, link_ok, page);
}

// Kuerzester Abstand zwischen zwei Durchlaeufen. Bis FW 88 2000 ms (alles in
// jedem Durchlauf), seit FW 89 nur noch eine Bremse: ein Durchlauf dauert ~0,4-0,6 s.
constexpr uint32_t INTERVAL_MS = 300;
std::string last_answer;  // letzte Antwort des Adapters, fuer die Fehleranzeige

// Protokoll der Startschritte fuer die Anzeige: "ATZ: ELM327 v2.2"
std::string init_log;

std::string compact(const std::string& a) {
  std::string out;
  for (char c : a) {
    if (c == '\r' || c == '\n') {
      if (!out.empty() && out.back() != ' ') out += " | ";
    } else {
      out += c;
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '|')) out.pop_back();
  return out;
}

void add_log(const std::string& line) {
  init_log += line + "\n";
  // nur die letzten 9 Zeilen behalten, mehr passt nicht aufs Display
  int lines = 0;
  for (char c : init_log) lines += c == '\n';
  while (lines-- > 9) init_log.erase(0, init_log.find('\n') + 1);
}

// Schickt einen Befehl, zeigt ihn samt Antwort auf dem Display und seriell
std::string step(const char* cmd, uint32_t timeout_ms) {
  ui::status(T("Starte Adapter ...", "Starting adapter ..."), elm.address().c_str(), (init_log + cmd + ": ...").c_str());
  std::string a = elm.send(cmd, timeout_ms);
  if (a.empty()) last_answer = std::string(cmd) + T(": keine Antwort vom Adapter (Bluetooth)", ": no answer from the adapter (Bluetooth)");
  add_log(std::string(cmd) + ": " + (a.empty() ? std::string(T("KEINE ANTWORT", "NO ANSWER")) : compact(a)));
  Serial.printf("> %s\n%s\n", cmd, a.empty() ? "<keine Antwort>" : a.c_str());
  ui::status(T("Starte Adapter ...", "Starting adapter ..."), elm.address().c_str(), init_log.c_str());
  return a;
}

// Darueber laeuft sicher der DC/DC-Wandler. Darunter kann das Auto trotzdem an
// sein: beim Fahren pendeln die 12 V zwischen 12,5 und 13,4 V (Android-Log
// 22.10.2026). Die Spannung ist deshalb nur ein Hinweis, entscheidend ist,
// ob das Auto antwortet.
constexpr float CAR_ON_V = 13.0f;
constexpr uint32_t CHECK_S = 60;   // so oft schaut der ESP nach der Fahrt nach dem Auto
// Nach der Fahrt (Marc 26.10.): 2 min hell, dann dunkel (Auto oder Beruehren
// weckt), nach 10 min ganz aus wie "Ausschalten" (nur RESET weckt)
constexpr uint32_t DARK_AFTER_S = 120, OFF_AFTER_S = 600;
constexpr uint32_t NO_READY_S = 300;  // so lange darf das Auto ohne Ready wach sein

// 12-V-Spannung am OBD-Stecker, die misst der Adapter selbst (weckt das Auto
// nicht). 0 = unbekannt
float obd_voltage() {
  std::string a = elm.send("ATRV", 2000);
  for (size_t i = 0; i < a.size(); i++)
    if (isdigit((unsigned char)a[i])) return atof(a.c_str() + i);
  return 0;
}

// Faehrt das Auto gleich oder laedt es? Nach dem Abstellen laedt das Auto nach
// ein paar Minuten selbst die 12-V-Batterie nach (14,4 V, Hauptrelais an, ca.
// 15-25 min, Logs 28.10. 17:19 und 29.10. 08:27). Dabei schweigt die ESC (730),
// die beim Fahren immer antwortet. Darum weckt nur eine antwortende ESC oder
// eine HV-Ladung, nicht die Spannung allein.
bool car_in_use() {
  for (auto cmd : {"ATL0", "ATS1", "ATH1", "ATSPA6"}) elm.send(cmd, 2000);
  if (elm.send("0100", 4000).find("41 00") == std::string::npos) return false;
  elm.send("ATSP6", 2000);
  std::vector<uint8_t> p;
  cur_tx.clear();  // nach der Pause lieber neu setzen
  target("730");
  if (parse_isotp(elm.send("22F010", 1500), "738", p)) return true;
  target("7E4");
  BmsData d;
  return parse_isotp(elm.send("220101", 3000), "7EC", p) && decode_220101(p, d) && d.power_kw < -1.0f;
}

// Nach der Fahrt: jede Minute kurz nachsehen, ob das Auto wieder an ist, dann
// neu starten, frisch fuer die naechste Fahrt. Nach DARK_AFTER_S dunkel
// (Beruehren weckt), nach OFF_AFTER_S ausschalten (Tiefschlaf, nur RESET).
[[noreturn]] void wait_for_car(std::string line2) {
  have_data = false;  // Statusbildschirm: Schlafen/Ausschalten antippbar, kein Seitenwechsel
  uint32_t start = millis();
  bool dark = false;
  for (uint32_t checks = 1;; checks++) {
    if (!dark) {
      ui::auto_brightness(time(nullptr));
      ui::status(T("Warte auf das Auto", "Waiting for the car"), line2.c_str(),
                 T("Startet von selbst, wenn das Auto an ist.", "Starts by itself when the car is on."));
      ui::menu_button();
    }
    menu_closed = false;
    for (uint32_t t0 = millis(); millis() - t0 < CHECK_S * 1000 && !menu_closed;) {
      if (dark) {
        if (touch::take_tap() || touch::take_swipe() || touch::take_long_press()) ESP.restart();
        delay(50);
        continue;
      }
      handle_touch();  // Menue (Fahrten ansehen usw.), langer Druck = Einstellungen
      delay(10);
      if (millis() - start >= DARK_AFTER_S * 1000) break;
    }
    if (millis() - start >= OFF_AFTER_S * 1000) power_off();
    if (!dark && millis() - start >= DARK_AFTER_S * 1000) {
      dark = true;
      ui::backlight(false);
      setCpuFrequencyMhz(80);  // Bluetooth braucht mindestens 80 MHz
      touch::take_tap();
      touch::take_swipe();
      touch::take_long_press();
    }
    if (menu_closed) continue;  // Statusbildschirm neu zeichnen
    if (checks % 15 == 0 && ota::sync_due()) line2 = ota::wifi_sync();
    bool on = elm.connect(5000);
    if (on) {
      elm.send("ATE0", 1000);
      float v = obd_voltage();
      on = (v == 0 || v >= CAR_ON_V) && car_in_use();  // Spannung unbekannt: nachfragen
    }
    if (on) ESP.restart();
    elm.disconnect();
  }
}

// Schlafen aus dem Menue: Display dunkel, Bluetooth getrennt, langsamer Takt.
// Der Touch wird weiter abgefragt (kein Tiefschlaf, sonst weckt nur die
// BOOT-Taste). Beruehren oder BOOT weckt, ebenso das Auto, wenn es nach dem
// Einschlafen aus war und wieder angeht (Pruefung jede Minute). Geweckt wird
// per Neustart.
// Ausschalten (Marc 24.10.): Tiefschlaf ohne Wecker, nur die RESET-Taste
// startet wieder. Wandler und Display-Elektronik ziehen weiter etwas Strom.
[[noreturn]] void power_off() {
  elm.disconnect();
  if (have_data) sdlog::finish();
  settings::backup();
  ui::status(T("Ausgeschaltet", "Powered off"), T("Zum Einschalten RESET drücken", "Press RESET to switch on"));
  delay(2500);
  ui::backlight(false);
  Serial.println("Ausschalten (Tiefschlaf, RESET weckt)");
  Serial.flush();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_deep_sleep_start();
}

[[noreturn]] void sleep_display() {
  elm.disconnect();
  if (have_data) sdlog::finish();
  ui::status(T("Schlafe ...", "Sleeping ..."), T("Berühren weckt", "Touch to wake"));
  delay(1000);
  ui::backlight(false);
  setCpuFrequencyMhz(80);  // Bluetooth braucht mindestens 80 MHz
  touch::take_tap();
  touch::take_swipe();
  touch::take_long_press();
  bool seen_off = false;
  for (uint32_t t0 = millis();;) {
    if (touch::take_tap() || touch::take_swipe() || touch::take_long_press()) ESP.restart();
    if (millis() - t0 >= CHECK_S * 1000) {
      t0 = millis();
      bool on = elm.connect(5000);
      if (on) {
        elm.send("ATE0", 1000);
        float v = obd_voltage();
        on = v >= CAR_ON_V && car_in_use();
      }
      elm.disconnect();
      if (!on) seen_off = true;
      else if (seen_off) ESP.restart();
    }
    delay(50);
  }
}

// Demo-Modus: simulierte Fahrt, auf der Lade-Seite eine Schnellladung.
// Kein Bluetooth, kein Log. Fuenfmal schnell tippen beendet
// ihn (Neustart), 3 s halten oeffnet wie sonst die Einstellungen.
[[noreturn]] void run_demo() {
  elm.disconnect();
  sdlog::stop();  // erfundene Werte gehoeren nicht ins Log
  ui::set_demo(true);
  ui::status(T("Demo-Modus", "Demo mode"), T("simulierte Werte, kein Auto verbunden", "simulated values, no car connected"));
  static Demo demo;  // ein paar kB, nicht auf den Stack
  touch::take_tap();
  page = 0;
  uint32_t next = 0;
  for (;;) {
    if (touch::take_long_press()) {
      settings_request = SETTINGS_MAGIC;
      ESP.restart();
    }
    if (touch::take_secret()) ESP.restart();
    bool redraw = false;
    int sw = touch::take_swipe();
    int vsw = touch::take_vswipe();
    int tx, ty;
    bool tap = touch::take_tap(&tx, &ty);
    if (vsw && ui::vswipe_page(page) >= 0) {  // Unterseiten wie mit dem Auto
      page = ui::vswipe_page(page);
      redraw = true;
    } else if (tap && ui::menu_hit(tx, ty, true)) {
      open_menu(true);
      redraw = true;
    } else if (tap || sw) {
      page = next_page(page, sw, tap, tx, ty);
      redraw = true;
    }
    if (millis() >= next) {
      next = millis() + 1000;
      demo.step(1.0f, page == ui::PAGE_CHARGE);
      redraw = true;
    }
    ui::auto_brightness(time(nullptr));
    if (redraw) ui::dashboard(demo.d, demo.cons, demo.charge, demo.tires, demo.cells, demo.veh, demo.trips, true, page);
    else ui::animate();
    delay(20);
  }
}

// Fahrtende: Log abschliessen, im Heim-WLAN hochladen, warten bis das Auto
// wieder an ist
[[noreturn]] void end_of_drive(const char* why) {
  Serial.printf("Fahrtende: %s\n", why);
  ui::status(T("Fahrt beendet", "Drive ended"), why, T("Log wird gespeichert ...", "Saving log ..."));
  sdlog::finish();
  if (trip_start_ms) {  // es kamen Daten, also war das Auto an
    trip.km = cons.trip_km();
    trip.kwh = cons.energy_kwh;
    trip.regen_kwh = cons.regen_kwh;
    trip.soc_to = (uint8_t)last.soc_display;
    trip.minutes = (millis() - trip_start_ms) / 60000;
  }
  // Verbrauch der Fahrt merken: Startwert fuer die Reichweite der naechsten
  float c = cons.recent_kwh_100km();
  if (c >= 5) {
    Preferences p;
    p.begin("ioniq", false);
    p.putFloat("cons100", c);
    p.end();
  }
  elm.disconnect();
  std::string up = ota::wifi_sync();  // holt im WLAN auch die Uhrzeit
  if (trip_start_ms) {
    // Uhr erst jetzt gestellt: Fahrtbeginn nachtragen (jetzt minus Fahrtdauer)
    time_t now = time(nullptr);
    if (!trip.start && now > 1700000000) trip.start = now - (millis() - trip_start_ms) / 1000;
    trips::record(trip_log, trip);
  }
  wait_for_car(up);
}

// Schnellstart wie im PC-Leser: automatische Suche, die mit Protokoll 6 beginnt
enum class Init { Ok, CarOff, NoAnswer };

Init init_elm() {
  init_log.clear();
  const char* init[] = {"ATZ", "ATE0", "ATL0", "ATS1", "ATH1"};
  for (auto cmd : init) {
    if (step(cmd, strcmp(cmd, "ATZ") == 0 ? 5000 : 2000).empty() && !elm.connected()) return Init::NoAnswer;
  }
  // Niedrige Spannung heisst nur "vielleicht aus": dann kuerzer warten und
  // ohne Antwort auf 0100 als "Auto ist aus" melden
  float v = obd_voltage();
  char vl[32];
  snprintf(vl, sizeof vl, "ATRV: %.1f V", v);
  add_log(vl);
  bool maybe_off = v > 0 && v < CAR_ON_V;
  step("ATSPA6", 2000);
  std::string a = step("0100", maybe_off ? 4000 : 8000);
  if (a.find("41 00") == std::string::npos) {
    step("ATSP0", 2000);
    a = step("0100", maybe_off ? 6000 : 15000);
    if (a.find("41 00") == std::string::npos) {  // Auto nicht an?
      last_answer = "0100: " + (a.empty() ? std::string(T("keine Antwort", "no answer")) : a);
      return maybe_off ? Init::CarOff : Init::NoAnswer;
    }
  }
  step("ATSP6", 2000);
  for (auto cmd : {"ATSH7E4", "ATFCSH7E4", "ATFCSD300000", "ATFCSM1"}) step(cmd, 2000);
  cur_tx = "7E4";
  if (!at2_disabled) at2_active = step("ATAT2", 2000).find("OK") != std::string::npos;
  return Init::Ok;
}

std::string to_hex(const std::vector<uint8_t>& p) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < p.size(); i++) {
    snprintf(b, sizeof b, i ? " %02X" : "%02X", p[i]);
    s += b;
  }
  return s;
}

// Wie battery_thermal in ioniq6_decode.py, fuer das Log
const char* thermal_name(const BmsData& d) {
  switch (battery_thermal(d)) {
    case Thermal::Off: return "aus";
    case Thermal::Pump: return "Pumpe läuft";
    case Thermal::Heating: return "Heizung an";
    case Thermal::Cooling: return "Kühlung an";
    default: return "";
  }
}

// Zeiten und Zaehler fuers Diagnose-Log (FW 89)
struct QueryStats {
  uint32_t n = 0, fail = 0, ms = 0, headers = 0;
} q_stats;

// Eingestelltes Steuergeraet: die Kopfzeile nur bei einem Wechsel senden
// (je Wechsel zwei Befehle, ATSH und ATFCSH). Leer = unbekannt.
std::string cur_tx;

void target(const char* tx) {
  if (cur_tx == tx) return;
  char cmd[16];
  snprintf(cmd, sizeof cmd, "ATSH%s", tx);
  elm.send(cmd, 2000);
  snprintf(cmd, sizeof cmd, "ATFCSH%s", tx);
  elm.send(cmd, 2000);
  cur_tx = tx;
  q_stats.headers++;
}

// Adaptive Timing 2 (kuerzere Wartezeit des Adapters nach der Antwort, FW 89).
// Antwortet das BMS damit zweimal in Folge nicht, zurueck auf den Standard ATAT1
// und bis zum Neustart dabei bleiben.
int at2_fails = 0;
void at2_check(bool ok) {
  if (!at2_active) return;
  if (ok) {
    at2_fails = 0;
    return;
  }
  if (++at2_fails >= 2) {
    elm.send("ATAT1", 1000);
    at2_active = false;
    at2_disabled = true;
    Serial.println("ATAT2 aus, zurueck auf ATAT1");
  }
}

// Abfrage mit einer Wiederholung; liefert die zusammengesetzte Nutzlast
bool query(const char* cmd, const char* rx, std::vector<uint8_t>& out, int tries = 2,
           uint32_t timeout_ms = 8000) {
  for (int i = 0; i < tries; i++) {
    uint32_t t0 = millis();
    std::string a = elm.send(cmd, timeout_ms);
    q_stats.n++;
    q_stats.ms += millis() - t0;
    handle_touch();
    if (parse_isotp(a, rx, out)) return true;
    q_stats.fail++;
    last_answer = std::string(cmd) + ": " + (a.empty() ? T("keine Antwort", "no answer") : a);
    Serial.printf("> %s fehlgeschlagen\n%s\n", cmd, a.c_str());
  }
  return false;
}

void connect() {
  int ble_fails = 0, car_fails = 0;
  for (;;) {
    // Solange die BLE-Verbindung steht, nur den Adapter neu starten: ein
    // verbundener Adapter sendet keine Werbung und waere per Scan unauffindbar.
    if (!elm.connected()) {
      std::string name = ElmBle::saved_name();
      std::string line2 = name.empty() ? T("Adapter einstecken, Auto einschalten", "Plug in the adapter, switch the car on")
                                                : "Adapter: " + name;
      ui::status(T("Suche OBD-Adapter ...", "Searching OBD adapter ..."), line2.c_str());
      ui::menu_button();
      if (!elm.connect()) {
        if (++ble_fails >= 12) end_of_drive(T("OBD-Adapter nicht gefunden", "OBD adapter not found"));
        menu_closed = false;
        for (int i = 0; i < 200 && !menu_closed; i++) {
          handle_touch();  // Menue-Schaltflaeche, langer Druck = Einstellungen
          delay(10);
        }
        if (menu_closed) ble_fails = 0;
        continue;
      }
    }
    Init r = init_elm();
    if (r == Init::Ok) return;
    if (r == Init::CarOff) end_of_drive(T("Auto ist aus", "Car is off"));
    if (++car_fails >= 5) end_of_drive(T("Auto antwortet nicht", "Car does not answer"));
    ui::status(T("Auto antwortet nicht", "Car does not answer"),
               T("Ist das Auto an (READY)? Neuer Versuch ...", "Is the car on (READY)? Retrying ..."), init_log.c_str());
    delay(5000);  // Zeit fuer ein Foto
    delay(3000);
  }
}

void setup() {
  // Unterspannungswaechter aus: beim Umschalten von USB auf den Akku bricht die
  // Versorgung kurz ein, und der ESP wuerde sonst neu starten (mitten im
  // Fahrtende, bevor die Fahrt gespeichert ist)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  {
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_POWERON || r == ESP_RST_EXT) {
      cold_start = COLD_MAGIC | r;
      delay(300);
      ESP.restart();
    }
    boot_reason = r == ESP_RST_SW && (cold_start & 0xFFFF0000) == COLD_MAGIC ? (esp_reset_reason_t)(cold_start & 0xFFFF) : r;
    cold_start = 0;
  }
  Serial.begin(115200);
  // deutsche Zeit fuer Fahrtenbuch und Log-Namen (die Uhr laeuft im Schlaf weiter)
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  trips::load(trip_log);
  Serial.setTxTimeoutMs(0);  // ohne geoeffneten Monitor nicht beim Schreiben warten
  i18n::load();
  ui::auto_brightness(time(nullptr));  // nachts gleich gedimmt starten (die Uhr laeuft im Schlaf weiter)
  ui::begin(true);
  // Nach einem Kabel-Flashen ist das NVS leer: Einstellungen von der Kopie auf
  // der SD-Karte zurueckholen (Marc 24.10.), vor Touch-Test und WLAN
  sdlog::begin(false);
  if (settings::restore()) {
    ui::status(T("Einstellungen", "Settings"), T("von der SD-Karte zurückgeladen", "restored from the SD card"),
               T("Neustart ...", "Restarting ..."));
    delay(1500);
    ESP.restart();
  }
  ElmBle::idle = elm_idle;  // HV-Leitungen laufen auch waehrend der Abfragen weiter
  touch::begin(ui::flipped());
  {
    Preferences p;
    p.begin("touch", true);
    touch::set_y_inverted(p.getBool("yinv", false));
    bool skip = p.getBool("skip", false);
    p.end();
    // Erster Start ohne Kalibrierung: gleich den Touch-Test zeigen
    if (!touch::calibrated() && !touch::load() && !skip && settings_request != SETTINGS_MAGIC) touch_test();
  }
  if (settings_request == SETTINGS_MAGIC) {
    settings_request = 0;
    ota::settings_portal();
  }
  {
    Preferences p;
    p.begin("ioniq", true);
    cons.fallback = p.getFloat("cons100", -1);
    p.end();
  }
  // Nach einem Neustart wegen Unterspannung kein WLAN: das zieht kurz viel Strom
  // und wuerde ihn gleich wieder abwuergen
  if (esp_reset_reason() != ESP_RST_BROWNOUT) ota::check_at_boot();  // stellt im WLAN auch die Uhr
  std::string sd = sdlog::begin();
  Serial.println(sd.c_str());
  settings::backup();  // Kopie auf der Karte aktuell halten (Adapter, Touch, ...)
  sdlog::date_untimed();  // Uhr eben im WLAN gestellt: Log der letzten Fahrt nachdatieren
  // Noch nicht hochgeladene Logs gleich beim Start schicken (Marc 23.10.),
  // nicht erst am naechsten Fahrtende; ohne Heim-WLAN bleiben sie auf der Karte
  if (esp_reset_reason() != ESP_RST_BROWNOUT && ota::sync_due()) Serial.println(ota::wifi_sync().c_str());
  // Verbindet sich mit dem gemerkten Adapter. Ist keiner gemerkt, nimmt er den
  // ersten mit OBD-Namen (z. B. "IOS-Vlink") und merkt ihn sich.
  connect();
}

// Ready war in dieser Fahrt an; wie oft es seitdem in Folge aus war
// Ready war in dieser Fahrt an; wie oft es seitdem in Folge aus war
bool ready_seen = false;
int ready_off = 0;

// Abfrageplan (FW 89, Marc 04.11.: Werte schneller anzeigen). Bis FW 88 fragte
// jeder Durchlauf alles ab und dauerte 2,0-2,2 s (jeder 5. 3,3 s). Jetzt holt
// jeder Durchlauf nur das BMS 220101 (Leistung, Strom, SoC, Drehzahl, daraus
// auch das Tempo). Alles andere hat einen eigenen Takt und kommt reihum dran,
// der am laengsten ueberfaellige zuerst, bis SLOW_BUDGET_MS verbraucht sind.
// Wer dreimal nicht antwortet, wird nur noch jede Minute gefragt.
// FW 91: 120 statt 250 ms, meist also ein Nebenauftrag je Durchlauf (Marc 05.11.)
constexpr uint32_t SLOW_BUDGET_MS = 120;
constexpr uint32_t LOG_MS = 2000;  // CSV weiter alle 2 s, sonst werden die Dateien riesig

enum Job { J_220105, J_220106, J_CELLS, J_ODO, J_TIRES, J_7E2, J_READY, J_CLIMA, J_7E3,
           J_CHARGE, J_AUX, J_OBC, J_BODY, J_DYN, J_STATS, J_COUNT };
// Steuergeraet je Auftrag (fuer den Vorzug ohne Kopfwechsel)
const char* const JOB_TX[J_COUNT] = {"7E4", "7E4", "7E4", "7C6", "7A0", "7E2", "744", "7B3", "7E3",
                                     "744", "7E5", "7E5", "770", "730", "7E4"};
uint32_t job_last[J_COUNT] = {};
int job_miss[J_COUNT] = {};
bool was_charging = false;

bool is_charging(const BmsData& d) { return charge.active || (d.bms_status & 0x80); }

// Takt eines Auftrags in ms, 0 = jetzt nicht noetig
uint32_t job_period(int j, const BmsData& d) {
  bool charging = is_charging(d);
  bool moving = speed_kmh(d, veh) > 5;
  uint32_t ms = 0;
  switch (j) {
    case J_220105: ms = 3000; break;   // Anzeige-SoC, Restenergie, Module 6-16
    case J_220106: ms = 5000; break;   // Kuehlmittel, Pumpe
    case J_CELLS: ms = page == ui::PAGE_CELLS ? 1 : 3000; break;  // ein Block je Auftrag
    case J_ODO: ms = 5000; break;      // km-Stand fuer den Verbrauch
    case J_TIRES: ms = page == ui::PAGE_DRIVETRAIN ? 2000 : 10000; break;
    case J_7E2: ms = 2000; break;      // Fahrpedal; das Tempo kommt dazwischen aus der Drehzahl
    // Ready (Hauptrelais): bei Ready und danach oft, sonst selten (744 schweigt dann, kostet 1 s)
    case J_READY: ms = (veh.relay || ready_seen) ? 1500 : 6000; break;
    case J_CLIMA: ms = 10000; break;
    case J_7E3: ms = page == ui::PAGE_TEMPS ? 1500 : 10000; break;
    case J_CHARGE: ms = (charging || page == ui::PAGE_CHARGE) ? 2000 : 0; break;
    case J_AUX: ms = 10000; break;
    case J_OBC: ms = (charging && !(veh.has_charge ? veh.dc : charge.dc())) ? 2000 : 0; break;
    // Tueren: im Stand oft (Sprung zur Auto-Seite), beim Fahren selten
    case J_BODY: ms = page == ui::PAGE_CAR ? 1000 : moving ? 5000 : 1500; break;
    // ESC: zeigt Ready (FW 84) und liefert die G-Spitzen; auf der G-Seite fragt loop() dazwischen
    case J_DYN: ms = 2000; break;
    case J_STATS: ms = (!veh.has_stats || (was_charging && !charging)) ? 1 : 0; break;
  }
  if (ms && job_miss[j] >= 3 && j != J_READY && j != J_CELLS) ms = 60000;
  // Relais an: die ESC entscheidet, ob wirklich Ready ist (FW 84), also oefter nachsehen
  if (j == J_DYN && veh.relay && job_miss[j] >= 3) ms = 5000;
  return ms;
}

void job_note(int j, bool got) { job_miss[j] = got ? 0 : job_miss[j] + 1; }

void run_job(int j, BmsData& d, std::vector<std::string>& raw) {
  std::vector<uint8_t> p;
  bool got = false;
  switch (j) {
    case J_220105:
      target("7E4");
      got = query("220105", "7EC", p) && decode_220105(p, d);
      if (got) raw[1] = to_hex(p);
      break;
    case J_220106:
      target("7E4");
      got = query("220106", "7EC", p, 1, 2000) && decode_220106(p, d);
      if (got) raw[2] = to_hex(p);
      break;
    case J_CELLS: {
      // Zellspannungen: ein Block je Auftrag, Bloecke ohne Antwort ueberspringen
      static int cell_block = 0, cell_miss[6] = {};
      for (int k = 0; k < 6; k++) {
        int b = cell_block++ % 6;
        if (cell_miss[b] >= 3) continue;
        target("7E4");
        got = query(CELL_DIDS[b], "7EC", p, 1, 2000) && decode_cells(p, b, cells);
        if (got) raw[15 + b] = to_hex(p);
        cell_miss[b] = got ? 0 : cell_miss[b] + 1;
        break;
      }
      got = true;  // eigener Zaehler je Block
      break;
    }
    case J_ODO:
      target("7C6");
      got = query("22B002", "7CE", p, 1, 2000) && decode_22B002(p, d);
      break;
    case J_TIRES:
      target("7A0");
      got = query("22C00B", "7A8", p, 1, 2000) && decode_22C00B(p, tires);
      if (got) raw[3] = to_hex(p);
      break;
    case J_7E2:
      target("7E2");
      got = query("22E004", "7EA", p, 1, 1500) && decode_7E2_E004(p, veh);
      if (got) raw[4] = to_hex(p);
      if (!got) veh.has_speed = false;
      veh.ecu_7e2 = got;
      break;
    case J_READY:
      target("744");
      got = query("22E004", "74C", p, 1, 1000) && decode_744(p, veh);
      if (got) raw[7] = to_hex(p);
      else veh.relay = false;
      veh.has_relay = true;
      if (veh.relay) ready_off = 0;
      else if (ready_seen) ready_off++;
      got = true;  // Schweigen heisst hier "aus", nicht "kann es nicht"
      break;
    case J_CLIMA:
      target("7B3");
      got = query("220100", "7BB", p, 1, 1500) && decode_7B3_0100(p, veh);
      if (got) raw[5] = to_hex(p);
      break;
    case J_7E3:
      target("7E3");
      got = query("22E001", "7EB", p, 1, 1500) && decode_7E3_E001(p, veh);
      if (got) raw[9] = to_hex(p);
      break;
    case J_CHARGE:
      target("744");
      for (const char* did : {"22E001", "22E002", "22E003"}) {  // E004 bei J_READY
        bool one = query(did, "74C", p, 1, 1500) && decode_744(p, veh);
        if (one && did[5] == '2') raw[6] = to_hex(p);
        if (one && did[5] == '3') raw[10] = to_hex(p);
        got |= one;
      }
      break;
    case J_AUX:
      target("7E5");
      got = query("22E011", "7ED", p, 1, 1500) && decode_7E5(p, veh);
      if (got) raw[11] = to_hex(p);
      if (!got) veh.has_aux = false;
      break;
    case J_OBC:
      target("7E5");
      got = query("22E002", "7ED", p, 1, 1500) && decode_7E5(p, veh);
      if (got) raw[12] = to_hex(p);
      break;
    case J_BODY: {
      target("770");
      got = query("22BC03", "778", p, 1, 1500) && decode_770(p, veh);
      if (got) raw[21] = to_hex(p);
      if (!got) veh.has_body = veh.has_lights = false;
      // Geht eine Tuer oder Klappe auf, zur Auto-Seite springen und, wenn alles
      // wieder zu ist, zurueck. Wer solange selbst wegblaettert, bleibt dort.
      static int auto_from = -1;
      static bool dismissed = false;
      bool open = got && (veh.door[0] || veh.door[1] || veh.door[2] || veh.door[3] || veh.trunk || veh.hood);
      if (auto_from >= 0 && page != ui::PAGE_CAR) auto_from = -1, dismissed = true;
      if (open && page != ui::PAGE_CAR && auto_from < 0 && !dismissed) {
        auto_from = page;
        page = ui::PAGE_CAR;
      } else if (!open) {
        if (auto_from >= 0 && page == ui::PAGE_CAR) page = auto_from;
        auto_from = -1;
        dismissed = false;
      }
      if (got && page == ui::PAGE_CAR) {
        if (query("22BC06", "778", p, 1, 1500) && decode_770(p, veh)) raw[22] = to_hex(p);
        if (query("22BC10", "778", p, 1, 1500) && decode_770(p, veh)) raw[23] = to_hex(p);
        veh.has_switch = query("22BC05", "778", p, 1, 1500) && decode_770(p, veh);
        if (veh.has_switch) raw[26] = to_hex(p);
      }
      // Scheinwerfer (Standlicht/Abblend/Fern, FW 90): bei Licht an in jedem
      // Tuer-Takt, damit es auch im Log steht, sonst nur auf der Auto-Seite
      if (got && (veh.lights_on || page == ui::PAGE_CAR)) {
        veh.has_beam = query("22BC09", "778", p, 1, 1500) && decode_770(p, veh);
        if (veh.has_beam) raw[25] = to_hex(p);
      } else {
        veh.has_beam = false;
      }
      break;
    }
    case J_DYN:
      target("730");
      got = query("22F010", "738", p, 1, 1500) && decode_730_F010(p, veh);
      if (got) raw[24] = to_hex(p), veh.add_dyn_sample();
      else veh.has_dyn = false;
      break;
    case J_STATS:
      target("7E4");
      got = query("220111", "7EC", p, 1, 1500) && decode_220111(p, veh);
      if (got) raw[8] = to_hex(p);
      break;
  }
  job_note(j, got);
  job_last[j] = millis();
}

// Die faelligen Auftraege, der am laengsten ueberfaellige zuerst, bis das
// Zeitbudget verbraucht ist (mindestens einer). all = alles Faellige (erster Durchlauf).
void run_jobs(BmsData& d, std::vector<std::string>& raw, bool all) {
  uint32_t t0 = millis();
  bool done[J_COUNT] = {};
  for (;;) {
    int best = -1;
    float urg = 0;
    uint32_t now = millis();
    for (int j = 0; j < J_COUNT; j++) {
      uint32_t per = job_period(j, d);
      if (done[j] || !per) continue;
      float u = job_last[j] ? (float)(now - job_last[j]) / per : 1000.0f;
      if (u < 1.0f) continue;  // noch nicht faellig
      // Gleiches Steuergeraet wie gerade eingestellt: kein Kopfwechsel, kleiner Vorzug
      if (cur_tx == JOB_TX[j]) u *= 1.25f;
      if (best < 0 || u > urg) best = j, urg = u;
    }
    if (best < 0) break;
    run_job(best, d, raw);
    done[best] = true;
    handle_touch();
    if (!all && millis() - t0 >= SLOW_BUDGET_MS) break;
  }
  bool charging = is_charging(d);
  if (!charging) veh.has_charge = false, veh.has_obc = false;
  // Ready = Relais an und die ESC antwortet (oder ein Motor dreht). Relais
  // allein heisst auch 12-V-Nachladen nach dem Abstellen.
  if (veh.relay && (veh.has_dyn || d.rpm_front != 0 || d.rpm_rear != 0)) ready_seen = true;
  was_charging = charging;
}

// Messwerte fuers Diagnose-Log (je 5 s): Durchlaeufe, Abfragen, Zeichnen
struct Timing {
  uint32_t cycles = 0, cycle_ms = 0, cycle_max = 0, bms_ms = 0, draw_ms = 0;
} timing;

// Zeichnen, waehrend der Adapter antwortet (FW 91): der Durchlauf setzt
// draw_pending, gezeichnet wird in der Wartezeit der naechsten BMS-Abfrage.
// Alles laeuft in derselben Aufgabe, der Adapter sammelt die Antwort derweil
// im Bluetooth-Rueckruf (Kern 0).
bool draw_pending = false, draw_in_wait = false;
void draw_now() {
  draw_pending = false;
  uint32_t t0 = millis();
  ui::dashboard(last, cons, charge, tires, cells, veh, trip_log, link_ok, page);
  timing.draw_ms += millis() - t0;
}
void elm_idle() {
  if (draw_in_wait && draw_pending) draw_now();
  else ui::animate();
}

void loop() {
  static uint32_t next = 0;
  // Diagnose fuer Bildfehler bei Bluetooth-Verbindung (Marc 23.10.): alle 5 s
  // Flash-Zugriffe, BLE-Zustand und freien Speicher auf SD (diag_*.csv, wird
  // mit LOG_UPLOAD am Fahrtende mit hochgeladen) und an die serielle Schnittstelle.
  // Seit FW 89 dazu die Zeiten: Durchlaeufe, mittlere und laengste Dauer, Zeit
  // fuer 220101 (seit FW 91 mit dem Zeichnen darin), Abfragen/Fehler/Mittel,
  // Kopfwechsel, Zeichnen, Adaptive Timing.
  static uint32_t diag_ms = 0, diag_ops = 0;
  if (millis() - diag_ms >= 5000) {
    diag_ms = millis();
    char line[200], ts[12] = "";
    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm tm;
      localtime_r(&now, &tm);
      strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    }
    uint32_t c = timing.cycles ? timing.cycles : 1;
    snprintf(line, sizeof line, "%s;%lu;%u;%d;%u;%u;%u;%u;%u;%u;%u;%u;%u;%u;%u;%u;%d\n", ts,
             (unsigned long)(millis() / 1000), (unsigned)(s_flash_ops - diag_ops), elm.connected() ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)timing.cycles,
             (unsigned)(timing.cycle_ms / c), (unsigned)timing.cycle_max, (unsigned)(timing.bms_ms / c),
             (unsigned)q_stats.n, (unsigned)q_stats.fail, (unsigned)(q_stats.n ? q_stats.ms / q_stats.n : 0),
             (unsigned)q_stats.headers, (unsigned)(timing.draw_ms / c), at2_active ? 2 : 1);
    diag_ops = s_flash_ops;
    timing = Timing();
    q_stats = QueryStats();
    Serial.print("DIAG ");
    Serial.print(line);
    sdlog::diag(line);
  }
  if (!elm.connected()) {
    have_data = false;
    connect();
  }
  // G-Seite: zwischen den Zyklen etwa fuenfmal pro Sekunde die Beschleunigung
  // holen und neu zeichnen (dort bleibt der alte 2-s-Takt, damit Luecken entstehen)
  if (millis() < next) {
    if (draw_pending) draw_now();  // laengere Pause (G-Seite, ohne Antwort): gleich zeichnen
    handle_touch();
    ui::animate();
    static uint32_t last_g = 0;
    if (page == ui::PAGE_G && have_data && veh.has_dyn && next - millis() > 400 && millis() - last_g >= 200) {
      last_g = millis();
      target("730");
      std::vector<uint8_t> p;
      if (query("22F010", "738", p, 1, 500) && decode_730_F010(p, veh)) {
        veh.add_dyn_sample();
        ui::dashboard(last, cons, charge, tires, cells, veh, trip_log, link_ok, page);
      }
    }
    delay(10);
    return;
  }
  uint32_t cycle_start = millis();
  next = cycle_start + (page == ui::PAGE_G ? 2000 : INTERVAL_MS);

  // Werte, die in diesem Durchlauf nicht dran sind, bleiben vom letzten stehen
  BmsData d = have_data ? last : BmsData();
  std::vector<uint8_t> p;
  // Rohdaten fuers Log (Reihenfolge wie im Kopf der CSV): 0-12 Steuergeraete,
  // 13 ready, 14 car_state, 15-20 Zellbloecke 220102 ... 22010C, 21-23 770 22BC03/06/10,
  // 24 730 22F010, 25-26 770 22BC09/05. Sie sammeln sich bis zur naechsten Logzeile (alle LOG_MS).
  static std::vector<std::string> raw(27);
  static uint32_t log_ms = 0;
  target("7E4");
  uint32_t t_bms = millis();
  draw_in_wait = true;  // das Bild vom letzten Durchlauf zeichnen, waehrend das BMS antwortet
  bool ok = query("220101", "7EC", p) && decode_220101(p, d);
  draw_in_wait = false;
  if (draw_pending) draw_now();  // Antwort kam vor dem ersten Warteschritt
  timing.bms_ms += millis() - t_bms;
  if (ok) raw[0] = to_hex(p);
  at2_check(ok);
  if (ok) {
    bool first = !have_data;
    // Erster Durchlauf: alles einmal holen, damit jede Seite gleich gefuellt ist
    run_jobs(d, raw, first);
  }
  if (ok) {
    cons.update(millis() / 1000.0, d.power_kw, d.odometer_km, d.rpm_rear != 0 || d.rpm_front != 0);
    // Beim Einstecken des Ladekabels direkt auf die Lade-Seite wechseln
    if (charge.update(millis() / 1000.0, d)) page = ui::PAGE_CHARGE;
    last = d;
    if (!have_data && !trip_start_ms) {  // Fahrtbeginn
      trip_start_ms = millis();
      time_t now = time(nullptr);
      trip.start = now > 1700000000 ? (uint32_t)now : 0;
      trip.soc_from = (uint8_t)d.soc_display;
    }
    have_data = true;
    if (millis() - log_ms >= LOG_MS) {
      log_ms = millis();
      if (veh.has_relay) {
        raw[13] = veh.relay ? "1" : "0";
        raw[14] = std::to_string(car_state(veh, is_charging(d)));
      }
      sdlog::write(d, cons, tires, thermal_name(d), raw);
      for (auto& r : raw) r.clear();
    }
  }
  // Fahrtende: Ready (Hauptrelais) war an und ist dreimal in Folge aus. Das
  // kommt Sekunden nach dem Ausschalten, das BMS antwortet danach noch ~20 s
  // (Handy-Logger 24.10.). Auch am Ladeende faellt das Relais ab.
  if (ready_off >= 3) end_of_drive(T("Auto ist aus", "Car is off"));
  // Nur Zuendung oder 12-V-Nachladen, nie Ready (z. B. Tueren auf, Auto wach):
  // nach 5 min beenden, sonst haelt die Abfrage den Bus wach (Log 27.10. 07:30,
  // 27 min ohne Ready; 28.10. 17:19 und 29.10. 08:27 Relais an ohne ESC)
  if (!ready_seen && !charge.active && trip_start_ms && millis() - trip_start_ms > NO_READY_S * 1000)
    end_of_drive(T("Kein Ready, Auto steht", "No READY, car parked"));
  // Sonst (Ready nie gesehen, z. B. nur Zuendung): das BMS antwortet nicht
  // mehr. Die 12 V allein reichen nicht (pendeln beim Fahren unter CAR_ON_V),
  // sie verkuerzen nur das Warten.
  // Ohne Antwort weiter im alten 2-s-Takt, damit die Grenzen unten gleich lang bleiben
  if (!ok) next = cycle_start + 2000;
  static int fails = 0;
  fails = ok ? 0 : fails + 1;
  if (fails >= 5) {
    float v = obd_voltage();
    if (v > 0 && v < CAR_ON_V) end_of_drive(T("Auto ist aus", "Car is off"));
    if (fails >= 30) end_of_drive(T("Auto antwortet nicht mehr", "Car stopped answering"));
  }

  link_ok = ok;
  ui::auto_brightness(time(nullptr));
  // Gezeichnet wird erst waehrend der naechsten BMS-Abfrage (elm_idle), dann
  // wartet der Durchlauf nicht extra auf das Bild (FW 91, ~0,14 s je Durchlauf)
  if (have_data) draw_pending = true;
  else if (!ok) ui::status(T("Abfrage fehlgeschlagen", "Query failed"), T("wird wiederholt", "retrying"), last_answer.c_str());
  uint32_t dur = millis() - cycle_start;
  timing.cycles++;
  timing.cycle_ms += dur;
  if (dur > timing.cycle_max) timing.cycle_max = dur;
}
