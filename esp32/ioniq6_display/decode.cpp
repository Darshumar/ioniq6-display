#include "decode.h"
#include "i18n.h"

#include <algorithm>
#include <cmath>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int u16(const uint8_t* d, int i) { return (d[i] << 8) | d[i + 1]; }
uint32_t u32(const uint8_t* d, int i) {
  return ((uint32_t)d[i] << 24) | ((uint32_t)d[i + 1] << 16) | (d[i + 2] << 8) | d[i + 3];
}
int s16(const uint8_t* d, int i) {
  int v = u16(d, i);
  return v & 0x8000 ? v - 0x10000 : v;
}
int s8(uint8_t v) { return v & 0x80 ? v - 0x100 : v; }

bool check(const std::vector<uint8_t>& p, uint8_t b1, uint8_t b2, size_t min_len) {
  return p.size() >= min_len && p[0] == 0x62 && p[1] == b1 && p[2] == b2;
}

}  // namespace

bool parse_isotp(const std::string& text, const char* rx_header, std::vector<uint8_t>& out) {
  out.clear();
  int expected = -1;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find_first_of("\r\n", pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;

    // Zeile in Hex-Token zerlegen: "7EC 10 3E 62 01 01 ..."
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && line[i] == ' ') i++;
      size_t j = i;
      while (j < line.size() && line[j] != ' ') j++;
      if (j > i) parts.push_back(line.substr(i, j - i));
      i = j;
    }
    if (parts.size() < 2) continue;
    if (rx_header && strcasecmp(parts[0].c_str(), rx_header) != 0) continue;
    std::vector<int> raw;
    bool ok = true;
    for (size_t k = 1; k < parts.size(); k++) {
      char* e;
      long v = strtol(parts[k].c_str(), &e, 16);
      if (*e || parts[k].size() > 2) { ok = false; break; }
      raw.push_back(v);
    }
    if (!ok || raw.empty()) continue;
    int pci = raw[0] >> 4;
    if (pci == 0) {  // Single Frame
      int len = raw[0] & 0x0F;
      for (int k = 1; k <= len && k < (int)raw.size(); k++) out.push_back(raw[k]);
      return true;
    }
    if (pci == 1 && raw.size() >= 2) {  // First Frame
      expected = ((raw[0] & 0x0F) << 8) | raw[1];
      out.assign(raw.begin() + 2, raw.end());
    } else if (pci == 2 && expected >= 0) {  // Consecutive Frame
      out.insert(out.end(), raw.begin() + 1, raw.end());
    }
  }
  if (expected < 0 || (int)out.size() < expected) {
    out.clear();
    return false;  // nichts oder unvollstaendig (BLE-Frames verloren)
  }
  out.resize(expected);
  return true;
}

bool decode_220101(const std::vector<uint8_t>& p, BmsData& b) {
  if (!check(p, 0x01, 0x01, 60)) return false;
  const uint8_t* d = p.data() + 3;
  b.soc_bms = d[4] / 2.0f;
  b.charge_limit_kw = u16(d, 5) / 100.0f;
  b.discharge_limit_kw = u16(d, 7) / 100.0f;
  b.bms_status = d[9];
  b.current = s16(d, 10) / 10.0f;  // + = Entladen, - = Laden
  b.voltage = u16(d, 12) / 10.0f;
  b.power_kw = b.voltage * b.current / 1000.0f;
  b.temp_max = s8(d[14]);
  b.temp_min = s8(d[15]);
  for (int i = 0; i < 5; i++) b.modules[i] = s8(d[16 + i]);
  b.cell_v_max = d[23] / 50.0f;
  b.cell_v_max_no = d[24];
  b.cell_v_min = d[25] / 50.0f;
  b.cell_v_min_no = d[26];
  b.aux_v = d[29] / 10.0f;
  b.cum_charged_kwh = u32(d, 38) / 10.0;
  b.cum_discharged_kwh = u32(d, 42) / 10.0;
  b.operating_h = u32(d, 46) / 3600.0f;
  b.inlet_c = d[22] - 40;
  b.rpm_rear = s16(d, 53);
  if (p.size() >= 3 + 59) b.isolation_kohm = u16(d, 57);
  b.rpm_front = s16(d, 55);
  return true;
}

bool decode_220105(const std::vector<uint8_t>& p, BmsData& b) {
  if (!check(p, 0x01, 0x05, 46)) return false;
  const uint8_t* d = p.data() + 3;
  b.soh = u16(d, 25) / 10.0f;
  b.soc_display = d[31] / 2.0f;
  // Restenergie in 2-Wh-Schritten (aus OBDb Hyundai-IONIQ-6, passt zum SoC der Fahrt vom 19.10.)
  b.energy_kwh = u16(d, 28) * 2 / 1000.0f;
  for (int i = 0; i < 7; i++) b.modules[5 + i] = s8(d[9 + i]);    // Module 6-12
  for (int i = 0; i < 4; i++) b.modules[12 + i] = s8(d[39 + i]);  // Module 13-16
  return true;
}

bool decode_220106(const std::vector<uint8_t>& p, BmsData& b) {
  b.has_coolant = false;
  if (!check(p, 0x01, 0x06, 14)) return false;
  const uint8_t* d = p.data() + 3;
  b.coolant_inlet = s8(d[4]);
  b.coolant_pump = d[9];  // Akku-Kuehlmittelpumpe (Rohwert, 0 = aus)
  b.has_coolant = true;
  return true;
}

bool decode_22B002(const std::vector<uint8_t>& p, BmsData& b) {
  if (!check(p, 0xB0, 0x02, 12)) return false;
  const uint8_t* d = p.data() + 3;
  b.odometer_km = ((long)d[6] << 16) | (d[7] << 8) | d[8];
  return true;
}

const char* const CELL_DIDS[6] = {"220102", "220103", "220104", "22010A", "22010B", "22010C"};

bool Cells::complete() const {
  for (bool h : have)
    if (!h) return false;
  return true;
}

bool decode_cells(const std::vector<uint8_t>& p, int block, Cells& c) {
  static const uint8_t ids[6] = {0x02, 0x03, 0x04, 0x0A, 0x0B, 0x0C};
  if (block < 0 || block > 5 || !check(p, 0x01, ids[block], 3 + 4 + 32)) return false;
  const uint8_t* d = p.data() + 3;
  for (int i = 0; i < 32; i++) c.v[block * 32 + i] = d[4 + i] / 50.0f;
  c.have[block] = true;
  return true;
}

bool decode_22C00B(const std::vector<uint8_t>& p, Tires& t) {
  if (!check(p, 0xC0, 0x0B, 24)) return false;
  const uint8_t* d = p.data() + 3;
  for (int i = 0; i < 4; i++) {
    int raw = d[4 + 5 * i];
    t.bar[i] = raw / 5.0f * 0.0689476f;  // Rohwert in 0,2 psi
    t.temp[i] = raw ? d[5 + 5 * i] - 50 : 0;
  }
  t.valid = true;
  return true;
}

float remaining_kwh(const BmsData& d) {
  if (d.energy_kwh > 0) return d.energy_kwh;  // vom BMS gemeldet
  return CAPACITY_NEW_KWH * d.soh / 100.0f * d.soc_display / 100.0f;
}

float charge_request_a(const BmsData& d) {
  return d.voltage > 0 ? d.charge_limit_kw * 1000.0f / d.voltage : 0;
}

Thermal battery_thermal(const BmsData& d) {
  if (!d.has_coolant) return Thermal::Unknown;
  if (!d.coolant_pump) return Thermal::Off;
  // Im Stand zeigt die Heizleistung (~4,5 kW) das Heizen eindeutig; die
  // Temperatur taugt dort nicht (warmes Restwasser beim Anlaufen der Pumpe).
  // Beim Laden ist die Leistung negativ: dann wie beim Fahren ueber die
  // Einlasstemperatur (DC-Laden 31.10./02.11.: Einlass bis 41 C, Akku 14-26 C).
  bool parked = d.rpm_rear == 0 && d.rpm_front == 0;
  bool charging = d.power_kw <= -1.0f;
  bool heating = parked && !charging ? d.power_kw >= 3.0f : d.coolant_inlet >= d.temp_max + 3;
  if (heating) return Thermal::Heating;
  if (d.coolant_inlet <= d.temp_min - 3) return Thermal::Cooling;
  return Thermal::Pump;
}

void Consumption::update(double now_s, float power_kw, long new_odo, bool driving) {
  // Laden im Stand ist kein Verbrauch: sonst ergaebe der naechste km einen
  // riesigen negativen Wert
  if (!driving && power_kw < -1.0f) {
    last_time_s = now_s;
    return;
  }
  if (last_time_s >= 0) {
    double e = power_kw * (now_s - last_time_s) / 3600.0;
    energy_kwh += e;
    if (driving && e < 0) regen_kwh -= e;
  }
  last_time_s = now_s;
  if (new_odo < 0) return;
  odo = new_odo;
  if (start_odo < 0) {
    start_odo = tick_odo = odo;
    ring_odo[0] = odo;
    ring_e[0] = energy_kwh;
    ring_n = 1;
  }
  if (odo > tick_odo) {
    last_km = (energy_kwh - tick_energy) / (odo - tick_odo) * 100.0;
    tick_odo = odo;
    tick_energy = energy_kwh;
    if (hist_n == HIST) {
      for (int i = 1; i < HIST; i++) hist[i - 1] = hist[i];
      hist_n--;
    }
    hist[hist_n++] = last_km;
    if (ring_n == RECENT_KM + 1) {
      for (int i = 1; i <= RECENT_KM; i++) {
        ring_odo[i - 1] = ring_odo[i];
        ring_e[i - 1] = ring_e[i];
      }
      ring_n--;
    }
    ring_odo[ring_n] = odo;
    ring_e[ring_n] = energy_kwh;
    ring_n++;
  }
}

float Consumption::recent_kwh_100km() const {
  if (ring_n >= 2) {
    long km = ring_odo[ring_n - 1] - ring_odo[0];
    if (km >= 3) return (ring_e[ring_n - 1] - ring_e[0]) / km * 100.0;
  }
  return fallback;
}

float Consumption::range_km(const BmsData& d) const {
  float c = recent_kwh_100km();
  if (c < 5) return -1;  // unplausibel wenig (z. B. lange bergab): keine Angabe
  return remaining_kwh(d) / c * 100.0f;
}

float Consumption::trip_kwh_100km() const {
  long km = trip_km();
  return km > 0 ? tick_energy / km * 100.0 : -1;
}

bool ChargeSession::update(double now_s, const BmsData& d) {
  bool charging = d.power_kw < -1.0f && d.rpm_rear == 0 && d.rpm_front == 0;
  bool started = false;
  if (charging && !active) {
    active = seen = started = true;
    start_s = now_s;
    start_cum = d.cum_charged_kwh;
    start_soc = d.soc_display;
    peak_kw = 0;
    curve_n = 0;
  }
  if (!active) return false;
  if (charging) {
    idle = 0;
    last_s = now_s;
    cum = d.cum_charged_kwh;
    soc = d.soc_display;
    kw = -d.power_kw;
    if (kw > peak_kw) peak_kw = kw;
    if (curve_n == 0 || (d.soc_display - curve_soc[curve_n - 1] >= 0.5f && curve_n < CURVE_MAX)) {
      curve_soc[curve_n] = d.soc_display;
      curve_kw[curve_n] = kw;
      curve_n++;
    }
  } else if (++idle >= 3) {
    active = false;
    kw = 0;
  }
  return started;
}

float ChargeSession::minutes_to(float target_soc, const BmsData& d) const {
  if (!active || kw < 1.0f || d.soc_display >= target_soc) return -1;
  float kwh = CAPACITY_NEW_KWH * d.soh / 100.0f * (target_soc - d.soc_display) / 100.0f;
  return kwh / kw * 60.0f;
}

float speed_kmh(const BmsData& d) {
  constexpr float REV_PER_KM = 10.65f * 1000.0f / 2.25f;  // Motorumdrehungen je km (2,25 m Abrollumfang, an 7E2 abgeglichen)
  return fabsf((float)d.rpm_rear) * 60.0f / REV_PER_KM;
}

int warnings(const BmsData& d, const Tires& t, char out[MAX_WARN][40]) {
  int n = 0;
  if (d.isolation_kohm >= 0 && d.isolation_kohm < 100)
    snprintf(out[n++], 40, T("Isolation nur %d kOhm", "Insulation only %d kOhm"), d.isolation_kohm);
  static const char* pos_de[4] = {"VL", "VR", "HL", "HR"};
  static const char* pos_en[4] = {"FL", "FR", "RL", "RR"};
  const char** pos = i18n::en() ? pos_en : pos_de;
  if (t.valid) {
    for (int i = 0; i < 4 && n < MAX_WARN; i++) {
      if (t.bar[i] > 0.5f && t.bar[i] < 2.2f) snprintf(out[n++], 40, T("Reifendruck %s %.1f bar", "Tyre pressure %s %.1f bar"), pos[i], t.bar[i]);
    }
  }
  if (n < MAX_WARN && d.cell_v_max > 0 && d.cell_v_max - d.cell_v_min >= 0.10f)
    snprintf(out[n++], 40, T("Zelldifferenz %.0f mV", "Cell difference %.0f mV"), (d.cell_v_max - d.cell_v_min) * 1000);
  if (n < MAX_WARN && d.temp_max >= 50) snprintf(out[n++], 40, T("Akku heiß: %d °C", "Battery hot: %d °C"), d.temp_max);
  if (n < MAX_WARN && d.aux_v > 5 && d.aux_v < 12.0f) snprintf(out[n++], 40, T("12-V-Batterie %.1f V", "12 V battery %.1f V"), d.aux_v);
  if (n < MAX_WARN && d.soc_display > 0 && d.soc_display < 10)
    snprintf(out[n++], 40, T("Akku fast leer: %.0f %%", "Battery almost empty: %.0f %%"), d.soc_display);
  return n;
}

namespace {
int u16le(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }
}  // namespace

bool decode_7E2_E004(const std::vector<uint8_t>& p, Vehicle& v) {
  if (!check(p, 0xE0, 0x04, 3 + 12)) return false;
  const uint8_t* d = p.data() + 3;
  v.pedal_pct = u16le(d, 8) / 512.0f;
  int s = u16le(d, 10);
  if (s & 0x8000) s -= 0x10000;
  v.speed_kmh = s * 0.015625f;  // rueckwaerts negativ
  v.has_speed = true;
  return true;
}

bool decode_7B3_0100(const std::vector<uint8_t>& p, Vehicle& v) {
  if (!check(p, 0x01, 0x00, 3 + 7)) return false;
  const uint8_t* d = p.data() + 3;
  v.inside_c = d[5] / 2.0f - 40;
  v.outside_c = d[6] / 2.0f - 40;
  v.has_climate = true;
  return true;
}

bool decode_744(const std::vector<uint8_t>& p, Vehicle& v) {
  if (p.size() < 3 || p[0] != 0x62 || p[1] != 0xE0) return false;
  const uint8_t* d = p.data() + 3;
  size_t n = p.size() - 3;
  switch (p[2]) {
    case 0x01:  // Lade-Zustand
      if (n < 18) return false;
      v.dc = d[17] & 0x08;
      v.ac = d[17] & 0x04;
      break;
    case 0x02:  // Ladesaeule
      if (n < 27) return false;
      v.evse_v = u16(d, 17) / 10.0f;
      v.evse_a = u16(d, 19) / 10.0f;
      v.evse_max_kw = u16(d, 25) / 100.0f;  // 10-W-Schritte
      break;
    case 0x03:  // angeforderte Spannung/Strom, Minuten seit dem Einstecken (AC bzw. DC)
      if (n < 28) return false;
      v.target_v = u16(d, 24) / 10.0f;
      v.target_a = u16(d, 26) / 10.0f;
      if (n >= 46) v.plug_min = v.dc ? u16(d, 44) : u16(d, 42);
      break;
    case 0x04:  // Stecker, Temperaturen der Ladebuchse
      if (n < 11) return false;
      v.plugged = d[4] & 0x02;
      v.relay = d[4] & 0x04;
      v.has_relay = true;
      v.dc_inlet_c[0] = d[8] - 50;
      v.dc_inlet_c[1] = d[9] - 50;
      v.ac_inlet_c = d[10] - 50;
      break;
    default:
      return false;
  }
  v.has_charge = true;
  return true;
}

// ICCU, Zuordnung aus OBDb Hyundai-IONIQ-6; im Stand am 17.10. geprueft
// (12-V-Batterie 14,7 V / 89 % / 19,5 C, Wandler 702 V -> 14,9 V)
bool decode_7E5(const std::vector<uint8_t>& p, Vehicle& v) {
  if (p.size() < 3 || p[0] != 0x62 || p[1] != 0xE0) return false;
  const uint8_t* d = p.data() + 3;
  size_t n = p.size() - 3;
  if (p[2] == 0x11) {
    if (n < 25) return false;
    v.ldc_c = d[9] - 100;
    v.ldc_out_v = u16(d, 10) / 1000.0f;
    v.ldc_out_a = u16(d, 12) / 1000.0f;  // Skalierung unsicher (im Stand 2,5 A)
    v.ldc_in_v = u16(d, 14) / 10.0f;
    v.aux_a = u16(d, 18) / 100.0f - 327;
    v.aux_soc = d[20];
    v.aux_v = u16(d, 21) / 1000.0f + 6;
    v.aux_c = u16(d, 23) / 2.0f - 40;
    v.has_aux = true;
    return true;
  }
  if (p[2] == 0x02) {
    if (n < 29) return false;
    v.obc_ac_v = u16(d, 4) / 10.0f;
    v.obc_ac_hz = d[10];
    v.obc_ac_a = u16(d, 11) / 100.0f;
    v.obc_dc_v = u16(d, 19) / 10.0f;
    v.obc_dc_a = u16(d, 21) / 100.0f;
    v.obc_c[0] = d[27] - 40;
    v.obc_c[1] = d[28] - 40;
    v.has_obc = true;
    return true;
  }
  return false;
}

bool decode_220111(const std::vector<uint8_t>& p, Vehicle& v) {
  if (!check(p, 0x01, 0x11, 3 + 20)) return false;
  const uint8_t* d = p.data() + 3;
  v.ac_count = u32(d, 4);
  v.dc_count = u32(d, 8);
  v.ac_kwh = u32(d, 12);
  v.dc_kwh = u32(d, 16);
  v.has_stats = true;
  return true;
}

// Seit FW 89 zuerst aus der Drehzahl: die kommt in jedem Durchlauf, 7E2 nur alle
// 2 s (Logs 02.-04.11.: Drehzahl-Tempo / 7E2-Tempo im Mittel 1,00, +-5 %)
float speed_kmh(const BmsData& d, const Vehicle& v) {
  if (d.rpm_rear != 0) return speed_kmh(d);
  return v.has_speed ? fabsf(v.speed_kmh) : 0;
}

void TripLog::add(const Trip& t, uint32_t ym) {
  if (n == TRIPS) {
    for (int i = 1; i < TRIPS; i++) trips[i - 1] = trips[i];
    n--;
  }
  trips[n++] = t;
  if (!ym) return;
  if (nm == 0 || months[nm - 1].ym != ym) {
    if (nm == MONTHS) {
      for (int i = 1; i < MONTHS; i++) months[i - 1] = months[i];
      nm--;
    }
    months[nm] = MonthStat();
    months[nm++].ym = ym;
  }
  MonthStat& m = months[nm - 1];
  m.trips++;
  m.km += t.km;
  m.kwh += t.kwh;
}

float sun_elevation(int yday, float utc_h, float lat, float lon) {
  const float RAD = 3.14159265f / 180;
  float g = 2 * 3.14159265f / 365 * (yday + (utc_h - 12) / 24);
  float eqtime = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g) - 0.014615f * cosf(2 * g) -
                            0.040849f * sinf(2 * g));
  float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g) - 0.006758f * cosf(2 * g) +
               0.000907f * sinf(2 * g) - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
  float tst = utc_h * 60 + eqtime + 4 * lon;  // wahre Ortszeit in Minuten
  float ha = (tst / 4 - 180) * RAD;
  float cz = sinf(lat * RAD) * sinf(decl) + cosf(lat * RAD) * cosf(decl) * cosf(ha);
  return 90 - acosf(cz) / RAD;
}

bool decode_7E3_E001(const std::vector<uint8_t>& p, Vehicle& v) {
  if (!check(p, 0xE0, 0x01, 3 + 58)) return false;
  const uint8_t* d = p.data() + 3;
  auto s16le = [&](int i) { int x = u16le(d, i); return x & 0x8000 ? x - 0x10000 : x; };
  v.motor_rpm = s16le(17);
  v.motor_c = s16le(25) / 100.0f;
  v.inverter_c = s16le(27) / 100.0f;
  v.heatsink_c = s16le(56) / 100.0f;
  v.has_motor = true;
  return true;
}

// Karosserie-Steuergeraet (IGPM, 770/778). OBDb-bix zaehlt ab Nutzbyte 0 und
// innerhalb des Bytes vom hoechsten Bit: bix 34 = d[4] Bit 5 (0x20).
bool decode_770(const std::vector<uint8_t>& p, Vehicle& v) {
  if (p.size() < 3 || p[0] != 0x62 || p[1] != 0xBC) return false;
  const uint8_t* d = p.data() + 3;
  size_t n = p.size() - 3;
  switch (p[2]) {
    case 0x03:  // Tueren (Kia EV9: bix 32 Kofferraum, 34 VL, 35 VR, 37 HR, 39 HL), Licht an bix 48
      if (n < 7) return false;
      v.trunk = d[4] & 0x80;
      v.door[0] = d[4] & 0x20;
      v.door[1] = d[4] & 0x10;
      v.door[3] = d[4] & 0x04;
      v.door[2] = d[4] & 0x01;
      v.lights_on = d[6] != 0;
      for (int i = 0; i < 4; i++) v.body_raw[i] = 4 + i < (int)n ? d[4 + i] : 0;
      v.has_body = v.has_lights = true;
      return true;
    case 0x05:  // Lichtschalter (Kona EV: bix 48), Bedeutung am Ioniq 6 noch offen
      for (int i = 0; i < 4; i++) v.switch_raw[i] = 4 + i < (int)n ? d[4 + i] : 0;
      v.has_switch = true;
      return true;
    case 0x09:  // Scheinwerfer (Kona EV, IONIQ 9): bix 55 Fernlicht, bix 63 Abblendlicht
      if (n < 8) return false;
      v.high_beam = d[6] & 0x01;
      v.low_beam = d[7] & 0x01;
      for (int i = 0; i < 4; i++) v.beam_raw[i] = d[4 + i];
      v.has_beam = true;
      return true;
    case 0x06:  // Bremslicht bix 32
      if (n < 5) return false;
      v.brake_light = d[4] != 0;
      return true;
    case 0x10:  // Lichtcode bix 32, 4 Bit
      if (n < 5) return false;
      v.light_code = d[4] >> 4;
      return true;
  }
  return false;
}

// Fahrdynamik vom ESC (730/738, 22F010), little-endian. Nach Marcs Fahrt am
// 27.10. (FW 78) passt OBDb nicht: d[14] ist laengs (folgt der Tempoaenderung,
// im Stand die Hangneigung), d[16] quer, d[18] Gierrate (folgt Tempo mal
// Lenkwinkel); je (roh - 32768) / 1000 m/s^2, OBDb rechnete 8x zu klein.
bool decode_730_F010(const std::vector<uint8_t>& p, Vehicle& v) {
  if (!check(p, 0xF0, 0x10, 3 + 19)) return false;
  const uint8_t* d = p.data() + 3;
  auto ms2 = [&](int i) { return (u16le(d, i) - 32768) / 1000.0f; };
  const int wheel[4] = {6, 4, 10, 8};  // OBDb: FR 32, FL 48, RR 64, RL 80
  for (int i = 0; i < 4; i++) v.wheel_kmh[i] = u16le(d, wheel[i]) / 32.0f;
  int st = u16le(d, 12);
  v.steer_deg = (st & 0x8000 ? st - 0x10000 : st) / 10.0f;
  v.lon_g = ms2(14) / 9.81f;  // + beschleunigen
  v.lat_g = ms2(16) / 9.81f;  // + mit positivem Lenkwinkel (Linkskurve)
  v.has_dyn = true;
  return true;
}

void Vehicle::add_dyn_sample() {
  peak_acc = std::max(peak_acc, lon_g);
  peak_brake = std::max(peak_brake, -lon_g);
  peak_left = std::max(peak_left, lat_g);
  peak_right = std::max(peak_right, -lat_g);
  if (trail_n == TRAIL) {
    for (int i = 1; i < TRAIL; i++) trail_lat[i - 1] = trail_lat[i], trail_lon[i - 1] = trail_lon[i];
    trail_n--;
  }
  trail_lat[trail_n] = lat_g;
  trail_lon[trail_n++] = lon_g;
}
