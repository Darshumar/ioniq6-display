// Dekodierung der BMS-Antworten des Ioniq 6 (Port von ioniq6_decode.py).
// Bewusst ohne Arduino-Abhaengigkeiten, damit es auch auf dem PC testbar ist.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

struct BmsData {
  // 220101
  float soc_bms = 0, voltage = 0, current = 0, power_kw = 0, aux_v = 0;
  int temp_min = 0, temp_max = 0;
  float cell_v_min = 0, cell_v_max = 0;
  int cell_v_min_no = 0, cell_v_max_no = 0;
  int rpm_rear = 0, rpm_front = 0;  // Motor 1 = hinten, Motor 2 = vorne
  double cum_charged_kwh = 0;       // Zaehler der insgesamt geladenen Energie
  double cum_discharged_kwh = 0;    // ... und der insgesamt entnommenen
  float operating_h = 0;            // Betriebsstunden des BMS
  // Vom BMS erlaubte Ladeleistung (unbestaetigt); beim DC-Laden fordert das
  // Auto damit den Strom charge_limit_kw / voltage an
  float charge_limit_kw = 0, discharge_limit_kw = 0;
  int bms_status = 0;
  int isolation_kohm = -1;  // Isolationswiderstand des HV-Akkus, -1 = unbekannt
  int inlet_c = 0;          // Akku-Einlasstemperatur (Kuehlmittel am Akkueingang)
  // 220105
  float soh = 0, soc_display = 0;
  float energy_kwh = 0;  // nutzbare Restenergie laut BMS (220105), 0 = unbekannt
  int8_t modules[16] = {0};
  // 220106 (optional)
  bool has_coolant = false;
  int coolant_inlet = 0, coolant_pump = 0;
  // 22B002 Kombiinstrument (optional)
  long odometer_km = -1;
};

// Werte weiterer Steuergeraete (Zuordnung aus OBDb Hyundai-IONIQ-6, CC BY-SA 4.0).
// Sie werden seltener abgefragt als das BMS und bleiben deshalb stehen.
struct Vehicle {
  // 7E2 22E004 Fahrzeugrechner
  bool has_speed = false;
  float speed_kmh = 0, pedal_pct = 0;
  // 7B3 220100 Klima
  bool has_climate = false;
  float outside_c = 0, inside_c = 0;
  // 744 22E001-E004 Ladesteuerung, nur beim Laden abgefragt
  bool has_charge = false, dc = false, ac = false, plugged = false;
  float evse_v = 0, evse_a = 0, evse_max_kw = 0;
  float target_v = 0, target_a = 0;  // was das BMS bei der Saeule anfordert
  int dc_inlet_c[2] = {0, 0}, ac_inlet_c = 0;
  int plug_min = -1;  // Minuten seit dem Einstecken
  // 744 22E004 d[4] Bit 2: Hauptrelais = Ready (Handy-Logger 24.10. am Auto bestaetigt;
  // faellt beim Ausschalten und am Ladeende auf 0, danach schweigt 744)
  bool has_relay = false, relay = false;
  bool ecu_7e2 = false;  // Fahrzeugrechner antwortet (nach dem Abstellen noch kurz, bei nur Zuendung nicht)
  // 7E3 22E001 Inverter des Heckmotors
  bool has_motor = false;
  float motor_c = 0, inverter_c = 0, heatsink_c = 0;
  int motor_rpm = 0;
  // 7E4 220111 Ladestatistik
  // ICCU (7E5): 12-V-Wandler und 12-V-Batterie (22E011), Bordlader beim AC-Laden (22E002)
  bool has_aux = false;
  float aux_soc = 0, aux_a = 0, aux_c = 0, aux_v = 0;  // 12-V-Batterie: Ladestand %, Strom (+ = laedt), Temperatur
  float ldc_in_v = 0, ldc_out_v = 0, ldc_out_a = 0;
  int ldc_c = 0;
  bool has_obc = false;
  float obc_ac_v = 0, obc_ac_hz = 0, obc_ac_a = 0, obc_dc_v = 0, obc_dc_a = 0;
  int obc_c[2] = {0, 0};
  bool has_stats = false;
  uint32_t ac_count = 0, dc_count = 0, ac_kwh = 0, dc_kwh = 0;
  // 770 Karosserie (IGPM): 22BC03 Tueren/Kofferraum/Abblendlicht, 22BC06 Bremslicht,
  // 22BC10 Lichtcode. Tuerbits aus OBDb Kia-EV9, am Ioniq 6 noch unbestaetigt;
  // Abblendlicht/Bremslicht/Lichtcode aus OBDb Hyundai-IONIQ-6.
  bool has_body = false;
  bool door[4] = {false, false, false, false};  // vorne links, vorne rechts, hinten links, hinten rechts
  bool trunk = false, hood = false;             // Motorhaube (Frunk): Bit noch unbekannt
  bool has_lights = false, low_beam = false, brake_light = false;
  int light_code = -1;  // 22BC10, Bedeutung noch unbekannt
  uint8_t body_raw[4] = {0, 0, 0, 0};  // 22BC03 Nutzbytes 4..7 zum Bestaetigen am Auto
  // 730 22F010 Fahrdynamik (ESC), Skala und Achsen aus Marcs Fahrt 27.10.
  // Beschleunigung in g; lon > 0 = beschleunigen, lat > 0 = Linkskurve
  // (wie positiver Lenkwinkel; Richtung am Auto noch zu pruefen)
  bool has_dyn = false;
  float lat_g = 0, lon_g = 0, steer_deg = 0;
  float wheel_kmh[4] = {0, 0, 0, 0};  // vorne links, vorne rechts, hinten links, hinten rechts
  // G-Kraft-Anzeige: Spitzen der Fahrt und die letzten Punkte als Spur
  float peak_acc = 0, peak_brake = 0, peak_left = 0, peak_right = 0;
  static constexpr int TRAIL = 24;
  float trail_lat[TRAIL] = {0}, trail_lon[TRAIL] = {0};
  int trail_n = 0;
  void add_dyn_sample();  // nach decode_730_F010: Spitzen und Spur fortschreiben
};

// Zustand wie im Handy-Logger (CSV-Spalte car_state): 0 aus, 1 Zuendung, 2 Ready, 3 Laden.
// Relais an -> Ready (beim Laden 3); Relais aus und 7E2 antwortet -> aus (gerade
// abgestellt); Relais aus, 7E2 stumm, BMS antwortet -> nur Zuendung.
inline int car_state(const Vehicle& v, bool charging) {
  if (v.relay) return charging ? 3 : 2;
  return v.ecu_7e2 ? 0 : 1;
}
bool decode_7E2_E004(const std::vector<uint8_t>& p, Vehicle& v);
bool decode_7B3_0100(const std::vector<uint8_t>& p, Vehicle& v);
bool decode_744(const std::vector<uint8_t>& p, Vehicle& v);  // E001-E004
bool decode_220111(const std::vector<uint8_t>& p, Vehicle& v);
bool decode_7E5(const std::vector<uint8_t>& p, Vehicle& v);  // 22E011 und 22E002
bool decode_7E3_E001(const std::vector<uint8_t>& p, Vehicle& v);
bool decode_770(const std::vector<uint8_t>& p, Vehicle& v);  // 22BC03, 22BC06, 22BC10
bool decode_730_F010(const std::vector<uint8_t>& p, Vehicle& v);
float speed_kmh(const BmsData& d, const Vehicle& v);  // vom Auto, sonst aus der Drehzahl

// Setzt eine ELM327-Antwort (ATH1, ATS1) aus ISO-TP-Frames zusammen.
// Liefert false, wenn nichts oder nur ein Teil angekommen ist.
bool parse_isotp(const std::string& text, const char* rx_header, std::vector<uint8_t>& out);

bool decode_220101(const std::vector<uint8_t>& p, BmsData& d);
bool decode_220105(const std::vector<uint8_t>& p, BmsData& d);
bool decode_220106(const std::vector<uint8_t>& p, BmsData& d);
bool decode_22B002(const std::vector<uint8_t>& p, BmsData& d);

// Alle 192 Zellspannungen: 220102, 220103, 220104, 22010A, 22010B, 22010C
// liefern je 32 Zellen (Byte / 50 V) ab Nutzbyte 4.
constexpr int CELLS = 192;
struct Cells {
  float v[CELLS] = {0};
  bool have[6] = {false};  // welche der sechs Bloecke schon da sind
  bool complete() const;
};
extern const char* const CELL_DIDS[6];
bool decode_cells(const std::vector<uint8_t>& p, int block, Cells& c);

// Reifen aus dem Karosserie-Steuergeraet (7A0/7A8, 22C00B), Belegung aus der
// Ioniq-5-PID-Liste, fuer den Ioniq 6 noch unbestaetigt.
// Reihenfolge: vorne links, vorne rechts, hinten links, hinten rechts.
struct Tires {
  bool valid = false;
  float bar[4] = {0};  // 0 = Sensor hat noch nichts gemeldet
  int temp[4] = {0};
};
bool decode_22C00B(const std::vector<uint8_t>& p, Tires& t);

// Nutzbare Netto-Kapazitaet neu (Ioniq 6 Long Range AWD, Baujahr 2023)
constexpr float CAPACITY_NEW_KWH = 77.4f;
float remaining_kwh(const BmsData& d);  // BMS-Wert, sonst Nennkapazitaet x SoH x SoC
// Geschwindigkeit aus der Drehzahl des Hinterachsmotors (kein Steuergeraet
// meldet km/h per OBD): Uebersetzung 10,65, Reifenumfang ca. 2,23 m
float speed_kmh(const BmsData& d);
// Vom Auto angeforderter Ladestrom in A, 0 = keine Angabe
float charge_request_a(const BmsData& d);

// "aus", "Pumpe laeuft", "Heizung an", "Kuehlung an" (siehe battery_thermal in Python)
enum class Thermal { Off, Pump, Heating, Cooling, Unknown };
Thermal battery_thermal(const BmsData& d);

// Verbrauch: integriert die Akkuleistung und bezieht sie auf den km-Stand.
struct Consumption {
  double energy_kwh = 0, tick_energy = 0;
  double regen_kwh = 0;  // beim Fahren zurueckgewonnen (Leistung negativ)
  double last_time_s = -1;
  long start_odo = -1, tick_odo = -1, odo = -1;
  float last_km = -1;  // kWh/100 km auf dem letzten vollen km, -1 = noch keiner
  // Verlauf: Verbrauch je km der letzten HIST km, aeltester zuerst
  static constexpr int HIST = 20;
  float hist[HIST] = {0};
  int hist_n = 0;
  // Stuetzpunkte (km-Stand, Energie) fuer den Schnitt der letzten RECENT_KM
  static constexpr int RECENT_KM = 25;
  long ring_odo[RECENT_KM + 1] = {0};
  double ring_e[RECENT_KM + 1] = {0};
  int ring_n = 0;
  float fallback = -1;  // Schnitt der letzten Fahrt, solange noch keine 3 km gefahren
  void update(double now_s, float power_kw, long odo, bool driving = true);
  long trip_km() const { return start_odo < 0 ? 0 : odo - start_odo; }
  float trip_kwh_100km() const;  // -1 = noch kein voller km
  float recent_kwh_100km() const;  // Schnitt der letzten bis zu 25 km, -1 = unbekannt
  float range_km(const BmsData& d) const;  // Restenergie / Schnitt, -1 = unbekannt
};

// Ladevorgang: beginnt, sobald im Stand Strom in den Akku fliesst, und endet,
// wenn das mehrere Abfragen lang nicht mehr so ist. Die Werte bleiben danach
// als "letzte Ladung" stehen.
struct ChargeSession {
  bool active = false, seen = false;
  double start_s = 0, last_s = 0, start_cum = 0, cum = 0;
  float start_soc = 0, soc = 0, kw = 0, peak_kw = 0;
  int idle = 0;
  // Ladekurve: Leistung ueber Ladestand, ein Punkt je 0,5 % Ladestand
  static constexpr int CURVE_MAX = 220;
  float curve_soc[CURVE_MAX], curve_kw[CURVE_MAX];
  int curve_n = 0;
  bool update(double now_s, const BmsData& d);  // true = Ladung hat gerade begonnen
  double kwh() const { return cum - start_cum; }
  double minutes() const { return (last_s - start_s) / 60.0; }
  bool dc() const { return peak_kw >= 12.0f; }  // AC laedt der Ioniq 6 mit hoechstens 11 kW
  // Minuten bis zum Ziel-SoC bei der aktuellen Leistung, -1 = nicht absehbar
  float minutes_to(float target_soc, const BmsData& d) const;
};

// Warnungen fuer die rote Kopfzeile: Texte in out, Rueckgabe = Anzahl
constexpr int MAX_WARN = 6;
int warnings(const BmsData& d, const Tires& t, char out[MAX_WARN][40]);

// Fahrtenbuch: die letzten Fahrten und Summen je Monat, im ESP gespeichert
struct Trip {
  uint32_t start = 0;  // Unix-Zeit des Fahrtbeginns, 0 = Uhr unbekannt
  float km = 0, kwh = 0, regen_kwh = 0;
  uint8_t soc_from = 0, soc_to = 0;
  uint16_t minutes = 0;
  float kwh_100km() const { return km > 0 ? kwh / km * 100 : -1; }
};
struct MonthStat {
  uint32_t ym = 0;  // Jahr * 100 + Monat, z. B. 202610
  uint16_t trips = 0;
  float km = 0, kwh = 0;
};
struct TripLog {
  static constexpr int TRIPS = 20, MONTHS = 12;
  Trip trips[TRIPS];  // aeltester zuerst
  MonthStat months[MONTHS];
  int n = 0, nm = 0;
  void add(const Trip& t, uint32_t ym);  // ym = 0: Monat unbekannt, nur in die Liste
};

// Sonnenhoehe in Grad (NOAA-Naeherung) fuer den Nachtmodus; yday 0..365,
// utc_h = Stunde in UTC mit Nachkommastellen, lat/lon in Grad
float sun_elevation(int yday, float utc_h, float lat, float lon);
