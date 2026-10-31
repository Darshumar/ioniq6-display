#include "ota.h"

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Preferences.h>

#include <algorithm>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>

#include "board.h"
#include <time.h>

#include "root_cas.h"
#include "sdlog.h"
#include "settings.h"
#include "touch.h"
#include "ui.h"
#include "i18n.h"
#include "version.h"
#include "elm_ble.h"

#define FW_STR2(x) #x
#define FW_STR(x) FW_STR2(x)
// Kennung im Abbild: tools/release.sh prueft damit, dass die gebaute Datei
// wirklich die Version aus version.h enthaelt
extern "C" const char fw_version_tag[] __attribute__((used)) = "IONIQ6_FW_VERSION=" FW_STR(FW_VERSION) ";";

namespace {

volatile bool time_synced = false;  // vom SNTP-Rueckruf gesetzt

constexpr const char* AP_SSID = "Ioniq6-Display";
constexpr const char* AP_PASS = "ioniq6obd";
// Contents-API statt raw.githubusercontent.com: funktioniert mit einem
// Schluessel auch fuer private Repos. Repos sind einstellbar, damit andere die
// Anzeige mit eigenem Repo nutzen koennen (Marc 24.10.).
constexpr const char* REPO_DEFAULT = UPDATE_REPO_DEFAULT;
String contents(const String& repo) { return "https://api.github.com/repos/" + repo + "/contents/"; }

String ssid, pass, token;
String repo;       // Firmware fuer Updates (Ordner FW_DIR, Branch main)
#if LOG_UPLOAD
String logrepo;    // Ziel der Logs, leer = wie repo
String logbranch;  // Branch fuer die Logs
String logdir;     // Ordner im Log-Repo, ohne / am Anfang und Ende

constexpr const char* LOGDIR_DEFAULT = "logs/esp32";

// "/logs//esp32/" -> "logs/esp32"; leer oder mit ".." -> Standard
String clean_dir(String d) {
  d.trim();
  d.replace("\\", "/");
  while (d.indexOf("//") >= 0) d.replace("//", "/");
  while (d.startsWith("/")) d.remove(0, 1);
  while (d.endsWith("/")) d.remove(d.length() - 1);
  if (d.isEmpty() || d.indexOf("..") >= 0) return LOGDIR_DEFAULT;
  return d;
}
#endif

// "https://github.com/a/b.git" oder " a/b " -> "a/b"; Ungueltiges -> fallback
String clean_repo(String r, const String& fallback) {
  r.trim();
  if (r.startsWith("https://")) r.remove(0, 8);
  if (r.startsWith("github.com/")) r.remove(0, 11);
  if (r.endsWith(".git")) r.remove(r.length() - 4);
  while (r.endsWith("/")) r.remove(r.length() - 1);
  int slash = r.indexOf('/');
  if (slash < 1 || slash == (int)r.length() - 1 || r.indexOf('/', slash + 1) >= 0) return fallback;
  for (char c : r)
    if (!isalnum((unsigned char)c) && c != '/' && c != '-' && c != '_' && c != '.') return fallback;
  return r;
}

#if LOG_UPLOAD
String clean_branch(String b) {
  b.trim();
  if (b.isEmpty() || b.indexOf("..") >= 0 || b.indexOf(' ') >= 0) return "main";
  return b;
}
#endif

void load() {
  Preferences p;
  p.begin("ota", true);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  token = p.getString("token", "");
  repo = clean_repo(p.getString("repo", REPO_DEFAULT), REPO_DEFAULT);
#if LOG_UPLOAD
  logrepo = clean_repo(p.getString("logrepo", ""), "");
  logbranch = clean_branch(p.getString("logbranch", "main"));
  logdir = clean_dir(p.getString("logdir", LOGDIR_DEFAULT));
#endif
  p.end();
}

void save(const String& key, const String& value) {
  Preferences p;
  p.begin("ota", false);
  p.putString(key.c_str(), value);
  p.end();
}

// Verbindet nur, wenn das WLAN in Reichweite ist; sonst nach ~2 s zurueck
bool wifi_connect(String& err) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (ssid.isEmpty()) {
    err = T("kein WLAN eingetragen", "no Wi-Fi configured");
    return false;
  }
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, false, false, 120, 0, ssid.c_str());
  WiFi.scanDelete();
  if (n <= 0) {
    err = String(T("WLAN \"", "Wi-Fi \"")) + ssid + T("\" nicht in Reichweite", "\" not in range");
    return false;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (uint32_t t = millis(); millis() - t < 10000;) {
    if (WiFi.status() == WL_CONNECTED) {
      // Uhrzeit fuer die Pruefung der Zertifikate, Log-Namen und Fahrtenbuch.
      // Erst weiter, wenn der Zeitserver wirklich geantwortet hat: die Uhr
      // laeuft aus dem letzten Schlaf schon und waere sonst sofort "gueltig",
      // ohne dass die Zeit aufgefrischt wurde. Hoechstens 20 s warten.
      // Mit Core 3 meldet sntp_get_sync_status() nicht zuverlaessig, deshalb
      // setzt der Rueckruf beim Stellen der Uhr ein eigenes Flag.
      time_synced = false;
      sntp_set_time_sync_notification_cb([](struct timeval*) { time_synced = true; });
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
      ui::status(T("WLAN verbunden", "Wi-Fi connected"), T("Hole Uhrzeit ...", "Getting the time ..."));
      for (uint32_t s = millis(); !time_synced && millis() - s < 20000;) delay(100);
      Serial.printf("Uhrzeit %s\n", time_synced ? "gestellt" : "nicht gestellt");
      return true;
    }
    delay(100);
  }
  err = String(T("keine Verbindung zu \"", "no connection to \"")) + ssid + T("\" (Passwort?)", "\" (password?)");
  return false;
}

// GET auf eine Datei im Repo; liefert den HTTP-Code
int get(HTTPClient& http, WiFiClientSecure& client, const char* file) {
  client.setCACert(GITHUB_ROOT_CAS);
  http.begin(client, contents(repo) + FW_DIR + file + "?ref=main");
  http.setUserAgent("ioniq6-display");
  http.addHeader("Accept", "application/vnd.github.raw");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  if (!token.isEmpty()) http.addHeader("Authorization", "Bearer " + token);
  http.setTimeout(15000);
  return http.GET();
}

String http_error(int code) {
  if (code == 401 || code == 404)
#if LOG_UPLOAD
    return String(T("GitHub: kein Zugriff (", "GitHub: no access (")) + code + T("), Schlüssel prüfen", "), check the key");
#else
    return String(T("GitHub: kein Zugriff (", "GitHub: no access (")) + code + T("), Repo prüfen", "), check the repo");
#endif
  if (code < 0) return String(T("GitHub nicht erreichbar (", "GitHub not reachable (")) + HTTPClient::errorToString(code) + ")";
  return String(T("GitHub antwortet ", "GitHub answers ")) + code;
}

// Neueste Version auf GitHub, -1 bei Fehler
int remote_version(String& err) {
  WiFiClientSecure client;
  HTTPClient http;
  int code = get(http, client, "version.txt");
  if (code != 200) {
    err = http_error(code);
    http.end();
    return -1;
  }
  int v = http.getString().toInt();
  http.end();
  if (v <= 0) err = T("version.txt unlesbar", "version.txt unreadable");
  return v > 0 ? v : -1;
}

// Erst die ganze Datei in den PSRAM laden (ruhiges Bild mit Fortschritt),
// dann bei ausgeschaltetem Licht in den Flash schreiben: jedes Flash-Schreiben
// haelt die Bildzeilen fuers Display an, das Bild verzerrte sonst (Marc 27.10.).
bool install(int version, String& err) {
  char title[40];
  snprintf(title, sizeof title, T("Update auf Version %d", "Update to version %d"), version);
  const char* hint = T("Nicht ausschalten", "Do not switch off");
  ui::update_view(title, 0, T("Lade herunter ...", "Downloading ..."), hint);
  WiFiClientSecure client;
  HTTPClient http;
  int code = get(http, client, "ioniq6_display_app.bin");
  if (code != 200) {
    err = http_error(code);
    http.end();
    return false;
  }
  int len = http.getSize();  // ohne Laengenangabe: bis zum Ende lesen, hoechstens App-Partition (3 MB)
  size_t cap = len > 0 ? len : 0x300000;
  uint8_t* img = (uint8_t*)ps_malloc(cap);
  if (!img) {
    err = T("Kein Speicher für das Update", "No memory for the update");
    http.end();
    return false;
  }
  // Nach "IONIQ6_FW_VERSION=<v>;" suchen: GitHub liefert kurz nach dem
  // Hochladen manchmal schon die neue version.txt, aber noch die alte Datei
  // (Marc 23.10. mit FW 62). Dann nicht installieren und spaeter nochmal.
  char marker[32];
  int mlen = snprintf(marker, sizeof marker, "IONIQ6_FW_VERSION=%d;", version);
  int mpos = 0;
  bool found = false;
  size_t got = 0;
  int last_pct = 0;
  WiFiClient* stream = http.getStreamPtr();
  uint32_t idle = millis();
  while (got < cap) {
    if (!http.connected() && !stream->available()) break;
    size_t n = stream->available() ? stream->readBytes(img + got, std::min((size_t)stream->available(), cap - got)) : 0;
    if (!n) {
      if (millis() - idle > 15000) break;
      delay(1);
      continue;
    }
    idle = millis();
    for (size_t i = got; i < got + n && !found; i++) {
      mpos = img[i] == (uint8_t)marker[mpos] ? mpos + 1 : (img[i] == (uint8_t)marker[0] ? 1 : 0);
      if (mpos == mlen) found = true;
    }
    got += n;
    int pct = len > 0 ? got * 100 / len : 0;
    if (pct / 5 != last_pct / 5) {  // Display nur alle 5 % neu zeichnen
      last_pct = pct;
      char line[64];
      snprintf(line, sizeof line, T("Lade herunter: %u von %u kB", "Downloading: %u of %u kB"), (unsigned)(got / 1024),
               (unsigned)(cap / 1024));
      ui::update_view(title, pct, line, hint);
    }
  }
  http.end();
  if (len <= 0) len = got;
  if (!got || got != (size_t)len) {
    free(img);
    err = T("Download abgebrochen", "Download aborted");
    return false;
  }
  if (!found) {
    free(img);
    err = T("GitHub liefert noch die alte Datei, nächster Versuch beim nächsten Start", "GitHub still serves the old file, next try at the next start");
    return false;
  }
  ui::update_view(title, -1, T("Wird installiert. Das Display ist dabei ca. 15 s dunkel.", "Installing. The display stays dark for about 15 s."), hint);
  delay(2000);
  ui::backlight(false);
  bool ok = Update.begin(len);
  for (size_t off = 0; ok && off < (size_t)len; off += 4096) {
    size_t n = std::min((size_t)4096, (size_t)len - off);
    ok = Update.write(img + off, n) == n;
  }
  ok = ok && Update.end(true);
  if (!ok) Update.abort();
  free(img);
  ui::backlight(true);
  if (!ok) err = String(T("Installation fehlgeschlagen: ", "Installation failed: ")) + Update.errorString();
  return ok;
}

// Prueft und installiert; liefert eine Meldung fuer die Anzeige. Ist dieselbe
// Version schon einmal installiert worden und laeuft danach trotzdem die alte,
// passt die Datei auf GitHub nicht zur version.txt: dann nicht bei jedem Start
// erneut laden (nur von Hand aus den Einstellungen, force).
String check_and_install(bool force = false) {
  String err;
  if (!wifi_connect(err)) return err;
  int v = remote_version(err);
  if (v < 0) return err;
  if (v <= FW_VERSION) return String(T("Firmware ist aktuell (Version ", "Firmware is up to date (version ")) + FW_VERSION + ")";
  Preferences p;
  p.begin("ota", false);
  int tried = p.getInt("tried", 0);
  if (!force && tried == v) {
    p.end();
    return "Update " + String(v) + T(" lief schon einmal ohne Erfolg, wird nicht wiederholt", " already failed once, not retried");
  }
  p.putInt("tried", v);
  p.end();
  if (!install(v, err)) {
    if (err.startsWith(T("GitHub liefert noch", "GitHub still serves"))) {  // war kein echter Fehlversuch
      p.begin("ota", false);
      p.putInt("tried", tried);
      p.end();
    }
    return err;
  }
  ui::status(T("Update fertig", "Update done"), T("Neustart ...", "Restarting ..."));
  delay(1000);
  ESP.restart();
  return "";
}

String adapter_label() {
  std::string a = ElmBle::saved_address();
  if (a.empty()) return T("keiner gemerkt", "none saved");
  std::string n = ElmBle::saved_name();
  return String((n.empty() ? std::string(T("(ohne Namen)", "(no name)")) : n).c_str()) + " (" + a.c_str() + ")";
}

bool flip() {
  Preferences p;
  p.begin("ui", true);
  bool f = p.getBool("flip", false);
  p.end();
  return f;
}

#if LOG_UPLOAD
// Liefert {"message":...,"content":"<Base64 der Datei>"} stueckweise, damit
// auch mehrere MB grosse Logs ohne Zwischenspeicher hochgehen
class JsonB64Stream : public Stream {
 public:
  JsonB64Stream(const String& head, fs::File& f, const String& tail) : head_(head), tail_(tail), f_(f) {}
  size_t total() const { return head_.length() + (f_.size() + 2) / 3 * 4 + tail_.length(); }
  int available() override { return pos_ < total() ? (int)min<size_t>(total() - pos_, 65536) : 0; }
  int read() override {
    int c = peek();
    if (c >= 0) pos_++;
    return c;
  }
  int peek() override {
    size_t hl = head_.length(), bl = (f_.size() + 2) / 3 * 4;
    if (pos_ < hl) return head_[pos_];
    if (pos_ < hl + bl) {
      size_t i = pos_ - hl;  // Stelle im Base64-Teil
      if (i % 4 == 0 && quad_pos_ != i) {
        fill();
        quad_pos_ = i;
      }
      return quad_[i % 4];
    }
    if (pos_ < hl + bl + tail_.length()) return tail_[pos_ - hl - bl];
    return -1;
  }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}

 private:
  void fill() {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t b[3] = {0, 0, 0};
    int n = f_.read(b, 3);
    quad_[0] = B64[b[0] >> 2];
    quad_[1] = B64[((b[0] & 3) << 4) | (b[1] >> 4)];
    quad_[2] = n > 1 ? B64[((b[1] & 15) << 2) | (b[2] >> 6)] : '=';
    quad_[3] = n > 2 ? B64[b[2] & 63] : '=';
  }
  String head_, tail_;
  fs::File& f_;
  size_t pos_ = 0, quad_pos_ = (size_t)-1;
  char quad_[4];
};
#endif

String html_escape(const String& s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

#if LOG_UPLOAD
// PUT einer Datei nach <logdir>/<name>; liefert den HTTP-Code
int put_log(const std::string& name, fs::File& f) {
  WiFiClientSecure client;
  client.setCACert(GITHUB_ROOT_CAS);
  HTTPClient http;
  http.begin(client, contents(logrepo.isEmpty() ? repo : logrepo) + logdir + "/" + name.c_str());
  http.setUserAgent("ioniq6-display");
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);
  http.setTimeout(30000);
  JsonB64Stream body("{\"message\":\"ESP32-Log " + String(name.c_str()) + "\",\"branch\":\"" + logbranch + "\",\"content\":\"",
                     f, "\"}");
  int code = http.sendRequest("PUT", &body, body.total());
  http.end();
  return code;
}
#endif

}  // namespace

namespace ota {

bool sync_due() {
  if (time(nullptr) < 1700000000) return true;  // Uhr fehlt: Log und Fahrt nachdatieren
#if LOG_UPLOAD
  return !sdlog::pending().empty();
#else
  return false;
#endif
}

#if !LOG_UPLOAD
std::string wifi_sync() {
  load();
  const char* on_card = T("Logs liegen auf der SD-Karte", "Logs are on the SD card");
  if (time(nullptr) >= 1700000000 || ssid.isEmpty()) return on_card;
  String err;
  bool ok = wifi_connect(err);
  if (ok) sdlog::date_untimed();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return ok ? std::string(T("Uhrzeit gestellt, ", "Time set, ")) + on_card : std::string(err.c_str()) + ", " + on_card;
}
#else
std::string wifi_sync() {
  load();
  auto files = sdlog::pending();
  // Ohne gestellte Uhr trotzdem ins WLAN: fuer die Uhrzeit (Log und Fahrt nachdatieren)
  bool need_time = time(nullptr) < 1700000000;
  if (!need_time && files.empty()) return T("keine neuen Logs", "no new logs");
  if (!need_time && token.isEmpty()) return T("kein GitHub-Schlüssel für den Upload", "no GitHub key for the upload");
  String err;
  if (!wifi_connect(err)) {
    WiFi.mode(WIFI_OFF);
    return std::string(err.c_str()) + T(", Logs bleiben auf der Karte", ", logs stay on the card");
  }
  sdlog::date_untimed();
  files = sdlog::pending();
  if (files.empty() || token.isEmpty()) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return files.empty() ? T("Uhrzeit gestellt, keine neuen Logs", "Time set, no new logs")
                         : T("Uhrzeit gestellt, kein GitHub-Schlüssel für den Upload", "Time set, no GitHub key for the upload");
  }
  int done = 0;
  std::string msg;
  for (auto& name : files) {
    char line[64];
    snprintf(line, sizeof line, T("%d von %d: %s", "%d of %d: %s"), done + 1, (int)files.size(), name.c_str());
    ui::status(T("Lade Logs auf GitHub", "Uploading logs to GitHub"), line);
    fs::File f;
    if (!sdlog::open_read(name, &f)) continue;
    int code = put_log(name, f);
    f.close();
    if (code == 201 || code == 422) {  // 422: liegt schon dort
      sdlog::mark_uploaded(name);
      done++;
    } else {
      msg = code == 403 || code == 404
                ? std::string(T("Upload abgelehnt: Schlüssel braucht Schreibrecht (Contents)",
                                "Upload refused: the key needs write access (Contents)"))
                : std::string(T("Upload fehlgeschlagen (", "Upload failed (")) + std::to_string(code) + ")";
      break;
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (msg.empty()) msg = std::to_string(done) + T(" Log(s) auf GitHub hochgeladen", " log(s) uploaded to GitHub");
  return msg;
}

#endif

bool configured() {
  load();
  return !ssid.isEmpty();
}

void check_at_boot() {
  Serial.println(fw_version_tag);  // haelt die Kennung auch im gebauten Abbild
  load();
  if (ssid.isEmpty()) return;
  ui::status(T("Suche Update ...", "Checking for update ..."), (String(T("WLAN \"", "Wi-Fi \"")) + ssid + "\"").c_str());
  String msg = check_and_install();
  Serial.printf("Update: %s\n", msg.c_str());
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);  // Funk wieder frei fuer Bluetooth
  ui::status(T("Suche Update ...", "Checking for update ..."), msg.c_str());
  delay(1500);
}

void settings_portal() {
  load();
  sdlog::begin(false);  // nur fuer die Log-Liste
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WebServer server(80);
  String result;

  auto page = [&]() {
    String h =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<title>Ioniq 6</title><style>body{font-family:sans-serif;margin:16px;max-width:30em}"
        "input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
        "button{padding:10px 16px;margin:4px 8px 4px 0}.r{background:#eef;padding:8px}</style>";
    h += String("<h2>") + T("Ioniq 6 Anzeige", "Ioniq 6 display") + "</h2><p>Firmware " + FW_VERSION + "</p>";
    if (!result.isEmpty()) h += "<p class=r>" + html_escape(result) + "</p>";
    h += String("<form method=post action=/save><h3>") + T("WLAN für Updates", "Wi-Fi for updates") + "</h3>" +
         T("Name", "Name") + "<input name=ssid value=\"" + html_escape(ssid) + "\">" + T("Passwort", "Password") +
         "<input name=pass type=password placeholder='" +
         (pass.isEmpty() ? "" : T("unverändert lassen = leer", "leave empty = unchanged")) + "'>" +
#if LOG_UPLOAD
         "<h3>" + T("GitHub-Schlüssel", "GitHub key") + "</h3>" +
         T("Fine-grained Token, Contents: Read and write für beide Repos unten (Lesen für Updates, Schreiben für "
           "den Log-Upload; bei öffentlichem Update-Repo ohne Upload nicht nötig)",
           "Fine-grained token, Contents: Read and write for both repos below (read for updates, write for the log "
           "upload; not needed for a public update repo without upload)") +
         "<input name=token type=password placeholder='" +
         (token.isEmpty() ? T("noch keiner hinterlegt", "none stored yet")
                          : T("hinterlegt, leer = unverändert", "stored, empty = unchanged")) +
         "'>" +
#endif
         "<h3>Updates</h3>" + T("Repo mit der Firmware (Besitzer/Name, Ordner ", "Repo with the firmware (owner/name, folder ") +
         FW_DIR + ", Branch main)" + "<input name=repo value=\"" + html_escape(repo) + "\">" +
#if LOG_UPLOAD
         "<h3>Log-Upload</h3>" +
         T("Repo für die Logs (leer = wie Updates)", "Repo for the logs (empty = same as updates)") +
         "<input name=logrepo placeholder=\"" + html_escape(repo) + "\" value=\"" + html_escape(logrepo) + "\">" +
         "Branch<input name=logbranch value=\"" + html_escape(logbranch) + "\">" + T("Ordner", "Folder") +
         "<input name=logdir value=\"" + html_escape(logdir) + "\">" +
#endif
         "<h3>" + T("Anzeige", "Display") + "</h3>" +
         "<label><input type=checkbox name=flip style='width:auto'" + (flip() ? " checked" : "") + "> " +
         T("um 180° drehen (falls die Anzeige auf dem Kopf steht)", "rotate by 180° (if the display is upside down)") +
         "</label><br><p>" +
         T("Helligkeit (Tag und Nacht) und Sprache stellst du im Menü am Display ein.",
           "Set brightness (day and night) and language in the menu on the display.") +
         "</p><button>" + T("Speichern", "Save") + "</button></form>" + "<h3>" + T("OBD-Adapter", "OBD adapter") +
         "</h3><p>" + html_escape(adapter_label()) + "</p>" + "<form method=post action=/forget><button>" +
         T("Adapter vergessen (sucht beim nächsten Start neu)", "Forget adapter (searches again at the next start)") +
         "</button></form>" + "<form method=post action=/check><button>" +
         T("Jetzt nach Update suchen", "Check for update now") + "</button></form>" + "<p><a href=/logs>" +
         T("Logs auf der SD-Karte", "Logs on the SD card") + "</a></p><p>" +
         T("Zurück zur Anzeige: aufs Display tippen.", "Back to the display: tap the screen.") + "</p>";
    server.send(200, "text/html", h);
  };

  server.on("/", HTTP_GET, page);
  server.on("/save", HTTP_POST, [&]() {
    if (server.hasArg("ssid")) save("ssid", ssid = server.arg("ssid"));
    if (server.arg("pass").length()) save("pass", pass = server.arg("pass"));
    if (server.arg("token").length()) save("token", token = server.arg("token"));
    if (server.hasArg("repo")) save("repo", repo = clean_repo(server.arg("repo"), REPO_DEFAULT));
#if LOG_UPLOAD
    if (server.hasArg("logrepo")) save("logrepo", logrepo = clean_repo(server.arg("logrepo"), ""));
    if (server.hasArg("logbranch")) save("logbranch", logbranch = clean_branch(server.arg("logbranch")));
    if (server.hasArg("logdir")) save("logdir", logdir = clean_dir(server.arg("logdir")));
#endif
    bool new_flip = server.hasArg("flip");
    if (new_flip != flip()) {
      Preferences p;
      p.begin("ui", false);
      p.putBool("flip", new_flip);
      p.end();
      settings::backup();
      result = T("Gespeichert. Die Drehung gilt nach dem Zurückkehren zur Anzeige.", "Saved. The rotation applies after returning to the display.");
      page();
      return;
    }
    settings::backup();
    WiFi.disconnect();  // neuer Zugang beim naechsten Versuch
    result = T("Gespeichert (mit Sicherung auf der SD-Karte, falls eine steckt).", "Saved (with a backup on the SD card, if one is inserted).");
    page();
  });
  server.on("/forget", HTTP_POST, [&]() {
    ElmBle::forget();
    settings::backup();
    result = T("Adapter vergessen. Beim nächsten Start nimmt er den ersten OBD-Adapter, den er findet.", "Adapter forgotten. At the next start it takes the first OBD adapter it finds.");
    page();
  });
  server.on("/check", HTTP_POST, [&]() {
    ui::status(T("Suche Update ...", "Checking for update ..."), (String(T("WLAN \"", "Wi-Fi \"")) + ssid + "\"").c_str());
    result = check_and_install(true);  // startet bei Erfolg neu
    ui::status(T("Einstellungen", "Settings"), result.c_str());
    page();
  });
  server.on("/logs", HTTP_GET, [&]() {
    String h = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
               "<title>Logs</title><style>body{font-family:sans-serif;margin:16px}td{padding:4px 12px 4px 0}</style>"
               "<h2>" + String(T("Logs auf der SD-Karte", "Logs on the SD card")) + "</h2>";
    auto files = sdlog::list();
    if (files.empty()) h += String("<p>") + T("Keine Logs (oder keine SD-Karte).", "No logs (or no SD card).") + "</p>";
    h += "<table>";
    for (auto it = files.rbegin(); it != files.rend(); ++it) {  // neueste zuerst
      String n = html_escape(it->first.c_str());
      h += "<tr><td><a href='/dl?f=" + n + "'>" + n + "</a></td><td>" + String(it->second / 1024) + " kB</td></tr>";
    }
    h += String("</table><p><a href=/>") + T("zurück", "back") + "</a></p>";
    server.send(200, "text/html", h);
  });
  server.on("/dl", HTTP_GET, [&]() {
    fs::File f;
    if (!sdlog::open_read(server.arg("f").c_str(), &f)) {
      server.send(404, "text/plain", T("nicht gefunden", "not found"));
      return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + server.arg("f") + "\"");
    server.streamFile(f, "text/csv");
    f.close();
  });
  server.onNotFound([&]() {  // Handy-Anmeldeseiten auf die Startseite leiten
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
  });
  server.begin();

  char detail[200];
  snprintf(detail, sizeof detail,
           T("1. Am Handy ins WLAN\n   \"%s\"\n   Passwort: %s\n2. Im Browser 192.168.4.1\n   öffnen\n\n"
             "Firmware-Version %d\nTippen = zurück zur Anzeige",
             "1. On the phone join the Wi-Fi\n   \"%s\"\n   Password: %s\n2. Open 192.168.4.1\n   in the browser\n\n"
             "Firmware version %d\nTap = back to the display"),
           AP_SSID, AP_PASS, FW_VERSION);
#if LOG_UPLOAD
  ui::status(T("Einstellungen", "Settings"), T("WLAN und GitHub-Schlüssel für Updates", "Wi-Fi and GitHub key for updates"), detail);
#else
  ui::status(T("Einstellungen", "Settings"), T("WLAN für Updates", "Wi-Fi for updates"), detail);
#endif
  touch::take_tap();
  for (;;) {
    server.handleClient();
    if (touch::take_tap()) ESP.restart();
    delay(5);
  }
}

}  // namespace ota
