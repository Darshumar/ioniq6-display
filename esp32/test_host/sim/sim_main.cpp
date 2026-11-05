#include "i18n.h"
#include <cstdlib>
// Rendert die sieben Seiten der ESP-Anzeige mit Beispielwerten in PPM-Dateien.
#include <cstdio>
#include <vector>
#include "Arduino_GFX_Library.h"
#include "board.h"
#include "decode.h"
#include "exio.h"
#include "demo.h"
#include "sdlog.h"
#include "ui.h"

static std::vector<uint16_t> frame;
static int fw, fh;
class PngOut : public Arduino_G {
 public:
  PngOut() : Arduino_G(SCREEN_W, SCREEN_H) {}
  bool begin(int32_t) override { return true; }
  void drawBitmap(int16_t, int16_t, uint8_t*, int16_t, int16_t, uint16_t, uint16_t) override {}
  void drawIndexedBitmap(int16_t, int16_t, uint8_t*, uint16_t*, int16_t, int16_t, int16_t) override {}
  void draw3bitRGBBitmap(int16_t, int16_t, uint8_t*, int16_t, int16_t) override {}
  // Die Firmware schreibt nur geaenderte Zeilen (ui.cpp show)
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* b, int16_t w, int16_t h) override {
    fw = SCREEN_W; fh = SCREEN_H;
    frame.resize(fw * fh);
    for (int r = 0; r < h; r++)
      for (int i = 0; i < w; i++) frame[(y + r) * fw + x + i] = b[r * w + i];
  }
  void draw24bitRGBBitmap(int16_t, int16_t, uint8_t*, int16_t, int16_t) override {}
};
Arduino_G* make_png_out() { return new PngOut(); }
namespace sdlog { bool active() { return true; } }
namespace exio { void begin() {} void set(Pin, bool) {} }

static void save(const char* name) {
  FILE* f = fopen(name, "wb");
  fprintf(f, "P6\n%d %d\n255\n", fw, fh);
  for (uint16_t c : frame) {
    uint8_t rgb[3] = {(uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC), (uint8_t)((c << 3) & 0xF8)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

int main() {
  if (getenv("SIM_EN")) i18n::lang = i18n::EN;  // englische Bilder (render.sh, zweiter Lauf)
  ui::begin();
  const TripLog demo_log = Demo().trips;  // Fahrtenbuch fuer die Bilder
  // Werte wie auf Marcs Fotos vom 17.10.2026
  BmsData d;
  d.soc_bms = 74.5; d.soc_display = 75.5; d.soh = 93.3; d.voltage = 756.2; d.current = 0.3;
  d.power_kw = 0.2; d.aux_v = 14.3; d.temp_min = 25; d.temp_max = 30;
  d.cell_v_min = 3.92; d.cell_v_max = 3.94; d.cell_v_min_no = 107; d.cell_v_max_no = 12;
  int8_t mods[16] = {26, 27, 27, 28, 29, 30, 28, 27, 26, 26, 27, 28, 25, 26, 27, 27};
  memcpy(d.modules, mods, sizeof mods);
  d.has_coolant = true; d.coolant_inlet = 21; d.coolant_pump = 0;
  d.rpm_rear = 2310; d.rpm_front = 0; d.odometer_km = 52746;
  d.power_kw = 18.4; d.current = 24.3; d.cum_charged_kwh = 14720.4; d.charge_limit_kw = 104; d.discharge_limit_kw = 252;
  Consumption c;
  c.start_odo = 52722; c.odo = 52746; c.tick_odo = 52746; c.tick_energy = 4.73; c.last_km = 16.8;
  c.energy_kwh = 4.73; c.regen_kwh = 0.86;
  float hist[20] = {14.2, 15.8, 18.9, 22.4, 17.1, 12.3, 9.8, 13.5, 16.2, 21.7,
                    25.3, 19.4, 16.8, 15.1, 14.9, 11.2, 4.1, 13.8, 17.6, 16.8};
  memcpy(c.hist, hist, sizeof hist); c.hist_n = 20;
  c.ring_n = 25;
  for (int i = 0; i < 25; i++) { c.ring_odo[i] = 52722 + i; c.ring_e[i] = i * 0.168; }
  d.cum_discharged_kwh = 8341.7; d.operating_h = 1184.5;
  ChargeSession cs; Tires t;
  Cells cells;
  for (int i = 0; i < CELLS; i++) {
    cells.v[i] = 3.925f + 0.004f * ((i * 37) % 7) / 6.0f + 0.006f * (i % 16 == 3);
  }
  cells.v[106] = 3.912f; cells.v[11] = 3.941f;
  for (bool& h : cells.have) h = true;
  t.valid = true;
  float bar[4] = {2.52, 2.55, 2.48, 2.50}; int tt[4] = {24, 25, 22, 23};
  for (int i = 0; i < 4; i++) { t.bar[i] = bar[i]; t.temp[i] = tt[i]; }

  Vehicle veh;
  veh.has_speed = true; veh.speed_kmh = 29;
  veh.has_climate = true; veh.outside_c = 12.5f; veh.inside_c = 21;
  veh.has_stats = true; veh.ac_count = 212; veh.dc_count = 64; veh.ac_kwh = 6480; veh.dc_kwh = 3870;
  veh.has_motor = true; veh.motor_rpm = 1820; veh.motor_c = 46.3f; veh.inverter_c = 38.9f; veh.heatsink_c = 35.2f;
  veh.has_charge = true; veh.dc = true; veh.plugged = true; veh.evse_v = 742; veh.evse_a = 238.6f;
  veh.evse_max_kw = 350; veh.dc_inlet_c[0] = 38; veh.dc_inlet_c[1] = 41;
  d.isolation_kohm = 1750;
  veh.has_aux = true; veh.aux_soc = 89; veh.aux_v = 14.7f; veh.aux_a = 0.5f; veh.aux_c = 19.5f;
  ui::dashboard(d, c, cs, t, cells, veh, demo_log, true, 0); save("page1_fahren.ppm");
  BmsData b = d; b.power_kw = 0.2; b.current = 0.3; b.rpm_rear = 0;
  ui::dashboard(b, c, cs, t, cells, veh, demo_log, true, 1); save("page2_akku.ppm");
  // laufende DC-Ladung
  BmsData ch = d; ch.rpm_rear = 0; ch.soc_display = 46.5; ch.soc_bms = 45.5; ch.voltage = 712.4;
  ch.current = -238.6; ch.power_kw = ch.voltage * ch.current / 1000; ch.charge_limit_kw = 175.0;
  ch.coolant_pump = 125; ch.coolant_inlet = 22; ch.temp_min = 31; ch.temp_max = 34; ch.cell_v_max = 3.86;
  ch.bms_status = 0xC4;
  cs.active = cs.seen = true; cs.start_s = 0; cs.last_s = 14 * 60; cs.start_soc = 18.0; cs.soc = 46.5;
  cs.start_cum = 0; cs.cum = 21.3; cs.kw = -ch.power_kw; cs.peak_kw = 176;
  // Kurve von 18 % bis 46,5 %: Anstieg, dann ueber 170 kW
  for (float soc = 18.0f; soc <= 46.5f; soc += 0.5f) {
    float kw = soc < 20 ? 80 + (soc - 18) * 45 : 176 - (soc - 20) * 0.08f;
    cs.curve_soc[cs.curve_n] = soc; cs.curve_kw[cs.curve_n++] = kw;
  }
  ui::dashboard(ch, c, cs, t, cells, veh, demo_log, true, 2); save("page3_laden.ppm");
  {  // Akku-Seite beim DC-Laden, Auto heizt den kalten Akku vor (31.10.)
    BmsData h = ch; h.temp_min = 14; h.temp_max = 17; h.coolant_inlet = 30; h.cell_v_min = 3.84f;
    for (int i = 0; i < 16; i++) h.modules[i] = 14 + i % 4;
    ui::dashboard(h, c, cs, t, cells, veh, demo_log, true, 1); save("akku_laden_heizen.ppm");
  }
  {  // AC-Ladung an der Wallbox: Netz und Bordlader statt Saeule
    Vehicle ac = veh; ac.dc = false; ac.ac = true; ac.ac_inlet_c = 31;
    ac.has_obc = true; ac.obc_ac_v = 231; ac.obc_ac_a = 15.9f; ac.obc_ac_hz = 50; ac.obc_c[0] = 41; ac.obc_c[1] = 43;
    BmsData a = ch; a.current = -14.6f; a.power_kw = a.voltage * a.current / 1000;
    ChargeSession acs = cs; acs.kw = -a.power_kw; acs.peak_kw = 10.6f; acs.cum = 6.2; acs.last_s = 40 * 60;
    acs.curve_n = 0;
    for (float soc = 18.0f; soc <= 46.5f; soc += 0.5f) { acs.curve_soc[acs.curve_n] = soc; acs.curve_kw[acs.curve_n++] = 10.4f + (soc > 20); }
    ui::dashboard(a, c, acs, t, cells, ac, demo_log, true, 2); save("laden_ac.ppm");
  }
  ui::dashboard(b, c, cs, t, cells, veh, demo_log, true, 3); save("page4_antrieb.ppm");
  ui::dashboard(b, c, cs, t, cells, veh, demo_log, true, ui::PAGE_CELLS); save("zellen.ppm");
  ui::dashboard(b, c, cs, t, cells, veh, demo_log, true, ui::PAGE_TRIPS); save("page5_fahrten.ppm");
  b.inlet_c = 17;
  ui::dashboard(b, c, cs, t, cells, veh, demo_log, true, ui::PAGE_TEMPS); save("page6_temperaturen.ppm");
  {  // Auto von oben: Licht an, alles zu; dann Fahrertuer, hinten rechts und Kofferraum offen, Bremse
    Vehicle car = veh;
    car.has_body = car.has_lights = true; car.light_code = 0;
    car.has_beam = car.has_switch = true; car.drl = true; car.beam_raw[3] = 0x30;  // Werte vom 05.11.
    ui::dashboard(b, c, cs, t, cells, car, demo_log, true, ui::PAGE_CAR); save("auto_tagfahrlicht.ppm");
    car.lights_on = true; car.light_code = 15; car.body_raw[2] = 0x04;
    car.drl = false; car.beam_raw[2] = 0xC0; car.beam_raw[3] = 0;
    ui::dashboard(b, c, cs, t, cells, car, demo_log, true, ui::PAGE_CAR); save("page7_auto.ppm");
    car.door[0] = car.door[3] = car.trunk = true; car.brake_light = true;
    car.body_raw[0] = 0xA4;
    ui::dashboard(b, c, cs, t, cells, car, demo_log, true, ui::PAGE_CAR); save("auto_offen.ppm");
  }
  {  // G-Kraft: Linkskurve beim Anbremsen, Spur davor
    Vehicle g = veh;
    g.has_dyn = true; g.steer_deg = 38; g.wheel_kmh[0] = 52.1f; g.wheel_kmh[1] = 49.8f; g.wheel_kmh[2] = 51.6f; g.wheel_kmh[3] = 49.5f;
    for (int i = 0; i < Vehicle::TRAIL; i++) {
      float t = i / (float)(Vehicle::TRAIL - 1);
      g.lat_g = 0.27f * t; g.lon_g = 0.05f - 0.2f * t * t;
      g.add_dyn_sample();
    }
    g.peak_acc = 0.31f; g.peak_brake = 0.18f; g.peak_left = 0.27f; g.peak_right = 0.22f;
    ui::dashboard(b, c, cs, t, cells, g, demo_log, true, ui::PAGE_G); save("g.ppm");
  }
  // Warnung: Reifen hinten links verliert Luft
  Tires low = t; low.bar[2] = 1.9f;
  ui::dashboard(d, c, cs, low, cells, veh, demo_log, true, 0); save("warnung.ppm");
  // Helligkeitsregler und eine abgedunkelte Seite
  ui::brightness_view(80, 30); save("helligkeit.ppm");
  ui::preview_brightness(40);
  ui::dashboard(d, c, cs, t, cells, veh, demo_log, true, 0); save("gedimmt_40.ppm");
  ui::preview_brightness(100);
  // Menue und Startbildschirm mit Schaltflaeche
  {
    const char* m[][2] = {{"Helligkeit", "Tag 80 %, Nacht 30 %"},
                          {"Einstellungen", "WLAN, Updates"},
                          {"Fahrten ansehen", "8 gespeichert; Tippen schließt"},
                          {"Schlafen", "Display aus; Berühren oder Auto an weckt"},
                          {"Touch-Test", "Bildschirm neu ausrichten"},
                          {"Demo-Modus", "simulierte Fahrt und Ladung, ohne Auto"},
                          {"Zurück", ""}};
    std::vector<ui::ListItem> it(7);
    for (int i = 0; i < 7; i++) { it[i].title = m[i][0]; it[i].sub = m[i][1]; }
    ui::choice_list("Menü", it, -1, "Firmware 86   Start: Strom an");
    save("menue.ppm");
    ui::trips_view(demo_log);
    save("fahrten_menue.ppm");
    ui::status("Suche OBD-Adapter ...", "Adapter: IOS-Vlink");
    ui::menu_button();
    save("suche.ppm");
    ui::update_view("Update auf Version 79", 62, "Lade herunter: 1254 von 2023 kB", "Nicht ausschalten");
    save("update1.ppm");
    ui::update_view("Update auf Version 79", -1, "Wird installiert. Das Display ist dabei ca. 15 s dunkel.", "Nicht ausschalten");
    save("update2.ppm");
    ui::touch_target(1, 760, 40, "");
    save("touch1.ppm");
    const uint8_t d[8] = {0x00, 0x01, 0x40, 0x5A, 0x01, 0x0E, 0x00, 0x00};
    const char* cal = "x gespiegelt, y normal, Massstab 1.00 / 1.00";
    ui::touch_check(true, d, false, 0, 0, 0, 0, cal);
    for (int i = 0; i < 40; i++) ui::touch_check(false, d, true, 60 + i * 5, 200 + (int)(40 * __builtin_sin(i / 6.0)), 259 - i * 5, 270, cal);
    save("touch2.ppm");
  }
  // Demo-Modus: 60 s fahren, dann 60 s auf der Lade-Seite
  ui::set_demo(true);
  Demo demo;
  for (int i = 0; i < 60; i++) demo.step(1, false);
  for (int p : {0, 1, 3, 4, 5, 6, ui::PAGE_G, ui::PAGE_CELLS}) {
    ui::dashboard(demo.d, demo.cons, demo.charge, demo.tires, demo.cells, demo.veh, demo.trips, true, p);
    char n[32]; snprintf(n, sizeof n, "demo%d.ppm", p); save(n);
  }
  for (int i = 0; i < 60; i++) demo.step(1, true);
  ui::dashboard(demo.d, demo.cons, demo.charge, demo.tires, demo.cells, demo.veh, demo.trips, true, 2); save("demo2.ppm");
  return 0;
}
