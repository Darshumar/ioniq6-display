#include "demo.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float SPEEDUP = 8;  // simulierte Sekunden je echter Sekunde
constexpr float REV_PER_KM = 10.65f * 1000.0f / 2.23f;
constexpr float USABLE_KWH = 74.0f;

// Geschwindigkeitsprofil Landstrasse/Autobahn in km/h
float profile(double t) {
  float v = 78 + 38 * sinf(t / 95.0f) + 8 * sinf(t / 31.0f);
  // alle 4 Minuten eine Ortsdurchfahrt: abbremsen (rekuperieren) und wieder los
  float ph = fmodf((float)t, 240.0f);
  if (ph < 40) v *= 1 - 0.6f * sinf(3.14159f * ph / 40);
  return v < 0 ? 0 : (v > 135 ? 135 : v);
}

// typische E-GMP-Ladekurve an einer 350-kW-Saeule
float charge_kw(float soc) {
  if (soc < 12) return 120 + (soc - 5) * 8;
  if (soc < 52) return 176 - (soc - 12) * 0.15f;
  if (soc < 80) return 170 - (soc - 52) * 3.6f;
  return 0;
}
}  // namespace

Demo::Demo() {
  d.soh = 96.2f;
  d.aux_v = 14.4f;
  d.has_coolant = true;
  d.cum_charged_kwh = 9120.4;
  d.cum_discharged_kwh = 8703.9;
  d.operating_h = 1184.5f;
  tires.valid = true;
  // Vorlauf: gut 25 km fahren, damit Reichweite und Verbrauchsverlauf gefuellt sind
  // Fahrtenbuch: zwei Wochen Pendeln und ein Ausflug, Monate davor nur Summen
  for (int m = 0; m < 5; m++) {
    static const float km[5] = {1120, 940, 1380, 1210, 0};
    if (!km[m]) continue;
    MonthStat& s = trips.months[trips.nm++];
    s.ym = 202606 + m;
    s.trips = (uint16_t)(km[m] / 28);
    s.km = km[m];
    s.kwh = km[m] * (0.142f + 0.006f * m);
  }
  uint32_t t0 = 1790812800;  // Anfang Oktober 2026
  for (int i = 0; i < 14; i++) {
    Trip t;
    t.start = t0 + i * 86400 + (i % 2 ? 55800 : 19800) + i * 420;
    t.km = i == 9 ? 186.4f : 23.5f + (i * 7) % 9;
    t.kwh = t.km * (0.138f + 0.02f * ((i * 5) % 4) / 3);
    t.regen_kwh = t.kwh * 0.12f;
    t.soc_from = (uint8_t)(82 - (i % 5) * 9);
    t.soc_to = (uint8_t)(t.soc_from - t.kwh / 0.74f);
    t.minutes = (uint16_t)(t.km * 1.3f);
    trips.add(t, 202610);
  }
  v = profile(0);
  for (int i = 0; i < 1300; i++) sim(1.0f, false);
}

void Demo::step(float dt, bool charging) { sim(dt * SPEEDUP, charging); }

void Demo::sim(float dt, bool charging) {
  vt += dt;
  float kw, accel = 0;
  if (charging) {
    if (!was_charging && soc > 60) soc = 14;  // fuer die Vorfuehrung fast leer ankommen
    v = 0;
    kw = -charge_kw(soc);
    d.charge_limit_kw = -kw > 0 ? -kw + 4 : 0;
  } else {
    float nv = profile(vt);
    float a = (nv - v) / 3.6f / dt;  // m/s^2
    accel = a;
    v = nv;
    float ms = v / 3.6f;
    float wheel_kw = (2050 * a + 240 + 0.33f * ms * ms) * ms / 1000;
    kw = (wheel_kw > 0 ? wheel_kw / 0.9f : wheel_kw * 0.7f) + 0.6f;
    // Grenzen des Akkus: voll weniger Rekuperation, leer weniger Leistung
    d.charge_limit_kw = soc > 90 ? 40 : 180 - soc;
    d.discharge_limit_kw = soc < 15 ? 120 : 260;
  }
  was_charging = charging;
  soc -= kw * dt / 3600 / USABLE_KWH * 100;
  if (soc < 8) soc = 8;
  if (soc > 100) soc = 100;
  odo += v * dt / 3600;
  if (kw < 0) d.cum_charged_kwh -= kw * dt / 3600;
  else d.cum_discharged_kwh += kw * dt / 3600;

  d.soc_display = soc;
  d.energy_kwh = soc / 100 * 66.0f;
  d.isolation_kohm = 1750;
  veh.has_speed = true;
  veh.speed_kmh = v;
  veh.pedal_pct = kw > 0 ? std::min(100.0f, kw / 2) : 0;
  veh.has_climate = true;
  veh.outside_c = 12.5f;
  veh.inside_c = 21.0f;
  veh.has_aux = true;  // 12-V-Batterie wie im Scan vom 17.10.
  veh.aux_soc = 89;
  veh.aux_v = 14.7f;
  veh.aux_a = 0.5f;
  veh.aux_c = 19.5f;
  veh.has_motor = true;
  veh.motor_rpm = d.rpm_rear;
  veh.motor_c = 38 + kw * 0.15f;
  veh.inverter_c = 31 + kw * 0.08f;
  veh.heatsink_c = 29 + kw * 0.05f;
  veh.has_stats = true;
  veh.ac_count = 212;
  veh.dc_count = 64;
  veh.ac_kwh = 6480;
  veh.dc_kwh = 3870;
  veh.has_charge = charging;
  veh.dc = charging;
  veh.plugged = charging;
  veh.evse_v = charging ? d.voltage + 5 : 0;
  veh.evse_a = charging ? -d.current : 0;
  veh.evse_max_kw = 350;
  veh.dc_inlet_c[0] = charging ? 38 : 20;
  veh.dc_inlet_c[1] = charging ? 41 : 20;
  d.soc_bms = soc * 0.97f + 2;
  d.voltage = 640 + soc * 1.15f - kw * 0.25f;
  d.power_kw = kw;
  d.current = kw * 1000 / d.voltage;
  d.rpm_rear = (int)(v * REV_PER_KM / 60);
  d.rpm_front = kw > 70 ? d.rpm_rear : 0;  // Frontmotor nur bei viel Leistung
  d.inlet_c = d.temp_min - 1;
  d.odometer_km = (long)odo;
  // Akku: beim Schnellladen waermer, die Kuehlung laeuft
  int base = charging ? 32 : 27;
  static const int8_t spread[16] = {0, 1, 1, 2, 2, 3, 1, 1, 0, 0, 1, 2, -1, 0, 1, 1};
  d.temp_min = d.temp_max = base;
  for (int i = 0; i < 16; i++) {
    d.modules[i] = base + spread[i];
    if (d.modules[i] < d.temp_min) d.temp_min = d.modules[i];
    if (d.modules[i] > d.temp_max) d.temp_max = d.modules[i];
  }
  d.coolant_pump = charging ? 125 : 0;
  d.coolant_inlet = charging ? 24 : 22;
  // Zellen: Spannung folgt dem Ladestand, Zelle 107 ist etwas schwaecher
  float lo = 9, hi = 0;
  for (int i = 0; i < CELLS; i++) {
    float c = 3.55f + soc * 0.0062f - kw * 0.0004f + 0.004f * ((i * 37) % 7) / 6.0f;
    if (i == 106) c -= 0.011f;
    if (i == 11) c += 0.006f;
    cells.v[i] = c;
    if (c < lo) lo = c, d.cell_v_min_no = i + 1;
    if (c > hi) hi = c, d.cell_v_max_no = i + 1;
  }
  for (bool& h : cells.have) h = true;
  d.cell_v_min = lo;
  d.cell_v_max = hi;
  // Reifen werden beim Fahren langsam warm, der Druck steigt mit
  if (!charging && tire_warm < 1) tire_warm += dt / 1800;
  static const float bar0[4] = {2.48f, 2.50f, 2.45f, 2.47f};
  for (int i = 0; i < 4; i++) {
    tires.temp[i] = 18 + (int)(tire_warm * 9) + (i < 2 ? 1 : 0);
    tires.bar[i] = bar0[i] + tire_warm * 0.12f;
  }

  // Auto-Seite: alles zu, Licht an, Bremslicht beim Verzoegern
  veh.has_body = veh.has_lights = true;
  veh.low_beam = true;
  veh.brake_light = !charging && accel < -0.5f;
  // G-Kraft: laengs aus dem Tempoprofil, dazu sanfte Kurven
  veh.has_dyn = true;
  float bend = sinf(vt / 9) * std::min(v / 50, 1.0f);
  veh.lon_g = std::max(-0.4f, std::min(0.4f, accel / 9.81f));
  veh.lat_g = 0.28f * bend;
  veh.steer_deg = bend * 60 / (1 + v / 40);  // + = links wie lat_g
  for (float& w : veh.wheel_kmh) w = v;
  if (!charging) veh.add_dyn_sample();

  cons.update(vt, kw, d.odometer_km, !charging);
  charge.update(vt, d);
}
