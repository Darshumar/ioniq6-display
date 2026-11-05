#include "sdlog.h"

#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>

#include "board.h"
#include "i18n.h"

#include <algorithm>

extern "C" uint64_t esp_rtc_get_time_us(void);  // RTC-Zeit seit Kaltstart, laeuft im Tiefschlaf weiter

namespace sdlog {

namespace {

// TF-Slot des Waveshare 4.3" (SPI). CS haengt am CH422G und bleibt dauerhaft
// aktiv; der SD-Bibliothek wird ein freier Pin als Platzhalter gegeben.
constexpr int PIN_CS = 6, PIN_MOSI = 11, PIN_SCK = 12, PIN_MISO = 13;
SPIClass sd_spi(HSPI);
File file;
bool mounted = false, ok = false;
bool want = false;  // Log gewuenscht: Datei wird mit der ersten Zeile angelegt
int rows = 0;
std::string current;  // Name der laufenden Datei
std::string diag_name;  // laufendes Diagnose-Log (/logs/diag_*.csv), leer = noch keins
// Zuletzt ohne Uhrzeit angelegtes Log (start_*.csv) und wann, nach der RTC-Uhr:
// beides uebersteht den Tiefschlaf, so klappt das Nachdatieren auch beim
// Upload im Schlaf oder beim naechsten Start im Heim-WLAN
RTC_DATA_ATTR char untimed[32];
RTC_DATA_ATTR uint64_t untimed_rtc_us;
#if LOG_UPLOAD
constexpr const char* UPLOADED = "/logs/uploaded.txt";
#endif
// Akku-Log der laufenden Schlafphase (ueberlebt den Tiefschlaf), leer = keins

const char* HEADER =
    "time;uptime_s;soc_bms_pct;soc_display_pct;soh_pct;remaining_kwh;battery_voltage_v;"
    "battery_current_a;battery_power_kw;battery_temp_min_c;battery_temp_max_c;"
    "module_01_c;module_02_c;module_03_c;module_04_c;module_05_c;module_06_c;module_07_c;module_08_c;"
    "module_09_c;module_10_c;module_11_c;module_12_c;module_13_c;module_14_c;module_15_c;module_16_c;"
    "cell_v_min;cell_v_min_no;cell_v_max;cell_v_max_no;aux_battery_v;cum_charged_kwh;"
    "charge_limit_kw;charge_request_a;discharge_limit_kw;bms_status_raw;motor1_rpm;motor2_rpm;"
    "coolant_inlet_c;coolant_pump_raw;battery_thermal;odometer_km;trip_km;trip_kwh;trip_kwh_100km;"
    "last_km_kwh_100km;tire_fl_bar;tire_fl_c;tire_fr_bar;tire_fr_c;tire_rl_bar;tire_rl_c;tire_rr_bar;"
    "tire_rr_c;raw_220101;raw_220105;raw_220106;raw_22C00B;raw_7E2_22E004;raw_7B3_220100;raw_744_22E002;"
    "raw_744_22E004;raw_220111;raw_7E3_22E001;raw_744_22E003;raw_7E5_22E011;raw_7E5_22E002;"
    "ready;car_state;raw_220102;raw_220103;raw_220104;raw_22010A;raw_22010B;raw_22010C;raw_770_22BC03;raw_770_22BC06;raw_770_22BC10;raw_730_22F010;raw_770_22BC09;raw_770_22BC05\n";

// Wenn die Karte fast voll ist, die aeltesten Logs loeschen
void make_room() {
  while (SD.totalBytes() - SD.usedBytes() < 50ull * 1024 * 1024) {
    File dir = SD.open("/logs");
    std::string oldest;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      std::string n = f.name();
      if (n.size() < 4 || n.compare(n.size() - 4, 4, ".csv")) continue;
      if (oldest.empty() || n < oldest) oldest = n;
    }
    if (oldest.empty()) return;
    SD.remove(("/logs/" + oldest).c_str());
  }
}

}  // namespace

std::string begin(bool new_log) {
  if (!mounted) {
    sd_spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    // Manche Karten antworten beim ersten Versuch nach dem Einschalten nicht
    // (Marc 23.10.: erster Start "keine SD-Karte", zweiter ok). Deshalb bis zu
    // dreimal, beim Wiederholen langsamer.
    bool found = false;
    for (int i = 0; i < 3 && !found; i++) {
      if (i) { SD.end(); delay(200); }
      found = SD.begin(PIN_CS, sd_spi, i ? 4000000 : 20000000);
    }
    if (!found) return T("keine SD-Karte gefunden", "no SD card found");
    mounted = true;
  }
  SD.mkdir("/logs");
  if (!new_log) return T("SD-Karte eingebunden", "SD card mounted");
  // Datei erst mit den ersten Daten anlegen: sonst bleibt bei jedem Start
  // ohne Auto (Demo, Touch-Test, zu Hause) eine leere Datei liegen
  want = true;
  char msg[48];
  snprintf(msg, sizeof msg, T("SD-Karte: %.1f GB frei", "SD card: %.1f GB free"), (SD.totalBytes() - SD.usedBytes()) / 1e9);
  return msg;
}

namespace {

bool open_file() {
  make_room();
  // Dateiname mit Datum, wenn die Uhr gestellt ist (nach dem Update-Check im
  // WLAN), sonst mit fortlaufender Nummer
  char name[48];
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(name, sizeof name, "/logs/%Y%m%d_%H%M%S.csv", &tm);
  } else {
    Preferences p;
    p.begin("ioniq", false);
    uint32_t n = p.getUInt("logno", 0) + 1;
    p.putUInt("logno", n);
    p.end();
    snprintf(name, sizeof name, "/logs/start_%05u.csv", (unsigned)n);
  }
  file = SD.open(name, FILE_WRITE);
  if (!file) return false;
  current = name + 6;
  if (now > 1700000000) {
    untimed[0] = 0;
  } else {
    snprintf(untimed, sizeof untimed, "%s", current.c_str());
    untimed_rtc_us = esp_rtc_get_time_us();
  }
  file.print(HEADER);
  file.flush();
  ok = true;
  return true;
}

}  // namespace

void stop() { want = false; }

bool active() { return ok || want; }

void write(const BmsData& d, const Consumption& c, const Tires& t, const char* thermal,
           const std::vector<std::string>& raw) {
  if (!ok && want && !open_file()) want = false;
  if (!ok) return;
  char tm_buf[24] = "";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(tm_buf, sizeof tm_buf, "%Y-%m-%dT%H:%M:%S", &tm);
  }
  String s;
  s.reserve(900);
  char b[64];
  auto add = [&](const char* v) { s += v; s += ';'; };
  auto addf = [&](const char* fmt, double v) { snprintf(b, sizeof b, fmt, v); add(b); };
  auto addi = [&](long v) { snprintf(b, sizeof b, "%ld", v); add(b); };
  add(tm_buf);
  addf("%.1f", millis() / 1000.0);
  addf("%.1f", d.soc_bms);
  addf("%.1f", d.soc_display);
  addf("%.1f", d.soh);
  addf("%.1f", remaining_kwh(d));
  addf("%.1f", d.voltage);
  addf("%.1f", d.current);
  addf("%.2f", d.power_kw);
  addi(d.temp_min);
  addi(d.temp_max);
  for (int i = 0; i < 16; i++) addi(d.modules[i]);
  addf("%.2f", d.cell_v_min);
  addi(d.cell_v_min_no);
  addf("%.2f", d.cell_v_max);
  addi(d.cell_v_max_no);
  addf("%.1f", d.aux_v);
  addf("%.1f", d.cum_charged_kwh);
  addf("%.2f", d.charge_limit_kw);
  addf("%.1f", charge_request_a(d));
  addf("%.2f", d.discharge_limit_kw);
  addi(d.bms_status);
  addi(d.rpm_rear);
  addi(d.rpm_front);
  if (d.has_coolant) {
    addi(d.coolant_inlet);
    addi(d.coolant_pump);
    add(thermal);
  } else {
    add(""), add(""), add("");
  }
  if (d.odometer_km >= 0) addi(d.odometer_km);
  else add("");
  addi(c.trip_km());
  addf("%.2f", c.tick_energy);
  if (c.trip_kwh_100km() >= 0) addf("%.1f", c.trip_kwh_100km());
  else add("");
  if (c.last_km >= 0) addf("%.1f", c.last_km);
  else add("");
  for (int i = 0; i < 4; i++) {
    if (t.valid && t.bar[i] > 0) {
      addf("%.2f", t.bar[i]);
      addi(t.temp[i]);
    } else {
      add(""), add("");
    }
  }
  for (size_t i = 0; i < raw.size(); i++) {
    s += raw[i].c_str();
    s += i + 1 < raw.size() ? ';' : '\n';
  }
  if (file.print(s) != s.length()) {
    ok = false;  // Karte gezogen oder voll
    file.close();
    return;
  }
  // Strom ist im Auto ploetzlich weg: regelmaessig auf die Karte schreiben
  if (++rows % 5 == 0) file.flush();
}

void diag(const char* line) {
  if (!mounted) return;
  if (diag_name.empty()) {
    char name[48];
    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm tm;
      localtime_r(&now, &tm);
      strftime(name, sizeof name, "/logs/diag_%Y%m%d_%H%M%S.csv", &tm);
    } else {
      snprintf(name, sizeof name, "/logs/diag_start_%05u.csv", (unsigned)(esp_random() % 100000));
    }
    File f = SD.open(name, FILE_WRITE);
    if (!f) return;
    f.print("time;uptime_s;flash_ops;ble;heap_int_free;heap_int_min;psram_free;cycles;cycle_ms;cycle_max_ms;bms_ms;queries;query_fails;query_ms;header_switches;draw_ms;atat\n");
    f.close();
    diag_name = name;
  }
  File f = SD.open(diag_name.c_str(), FILE_APPEND);
  if (!f) return;
  f.print(line);
  f.close();
}

void finish() {
  diag_name.clear();  // naechste Fahrt bekommt ein eigenes Diagnose-Log, dieses kann hoch
  want = false;
  if (!ok) return;
  file.close();
  ok = false;
  current.clear();
}

#if LOG_UPLOAD
std::vector<std::string> pending() {
  std::vector<std::string> out;
  if (!mounted) return out;
  std::string done;
  File u = SD.open(UPLOADED, FILE_READ);
  if (u) {
    while (u.available()) done += (char)u.read();
    u.close();
  }
  std::vector<std::string> empty;
  File dir = SD.open("/logs");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    std::string n = f.name();
    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".csv") || n == current) continue;
    if ("/logs/" + n == diag_name) continue;  // wird noch geschrieben
    if (n.compare(0, 5, "akku_") && n.compare(0, 5, "diag_") && f.size() <= strlen(HEADER)) {  // leer (nur Kopfzeile, aeltere Firmware): weg damit
      empty.push_back("/logs/" + n);
      continue;
    }
    if (done.find("\n" + n + "\n") != std::string::npos || done.rfind(n + "\n", 0) == 0) continue;
    out.push_back(n);
  }
  dir.close();
  for (auto& e : empty) SD.remove(e.c_str());
  std::sort(out.begin(), out.end());
  return out;
}
#endif

void date_untimed() {
  time_t now = time(nullptr);
  if (!untimed[0] || current == untimed || now < 1700000000 || !mounted) return;
  // Start der Fahrt = jetzt minus die seitdem vergangene Zeit (RTC laeuft im Schlaf weiter)
  time_t start = now - (time_t)((esp_rtc_get_time_us() - untimed_rtc_us) / 1000000);
  struct tm tm;
  localtime_r(&start, &tm);
  char name[48];
  strftime(name, sizeof name, "/logs/%Y%m%d_%H%M%S.csv", &tm);
  std::string from = std::string("/logs/") + untimed;
  if (SD.exists(from.c_str()) && !SD.exists(name) && SD.rename(from.c_str(), name))
    Serial.printf("Log %s -> %s\n", from.c_str(), name);
  untimed[0] = 0;
}

#if LOG_UPLOAD
void mark_uploaded(const std::string& name) {
  File u = SD.open(UPLOADED, FILE_APPEND);
  if (!u) return;
  u.print((name + "\n").c_str());
  u.close();
}
#endif

std::vector<std::pair<std::string, size_t>> list() {
  std::vector<std::pair<std::string, size_t>> out;
  if (!mounted) return out;
  File dir = SD.open("/logs");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    std::string n = f.name();
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".csv") == 0) out.push_back({n, f.size()});
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool open_read(const std::string& name, void* file_out) {
  if (!mounted || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return false;
  if (ok) file.flush();
  File f = SD.open(("/logs/" + name).c_str(), FILE_READ);
  if (!f) return false;
  *static_cast<File*>(file_out) = f;
  return true;
}

}  // namespace sdlog
