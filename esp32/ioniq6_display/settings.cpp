#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>

#include "board.h"

namespace settings {

namespace {

constexpr const char* FILE_NAME = "/einstellungen.txt";

enum Type { STR, INT, BOOL, BYTES };
struct Key {
  const char* ns;
  const char* key;
  Type type;
};
// Alles, was man nach einem Neu-Flashen sonst neu eingeben muesste
constexpr Key KEYS[] = {
    {"ota", "ssid", STR},       {"ota", "pass", STR},          {"ota", "token", STR},
    {"ota", "repo", STR},
#if LOG_UPLOAD
    {"ota", "logrepo", STR},    {"ota", "logbranch", STR},     {"ota", "logdir", STR},
#endif
    {"ui", "bright", INT},         {"ui", "night", INT},
    {"ui", "flip", BOOL},       {"ui", "lang", INT},       {"ioniq", "adapter", STR},     {"ioniq", "adapter_name", STR},
    {"touch", "cal", BYTES},    {"touch", "yinv", BOOL},       {"touch", "skip", BOOL},
};

String hex(const uint8_t* b, size_t n) {
  static const char* H = "0123456789abcdef";
  String s;
  for (size_t i = 0; i < n; i++) {
    s += H[b[i] >> 4];
    s += H[b[i] & 15];
  }
  return s;
}

String to_text() {
  String t = "# Ioniq-6-Anzeige: Sicherung der Einstellungen. Wird nach einem\n"
             "# Kabel-Flashen automatisch zurueckgeladen. Enthaelt WLAN-Passwort\n"
             "# und GitHub-Schluessel im Klartext.\n";
  for (const Key& k : KEYS) {
    Preferences p;
    if (!p.begin(k.ns, true)) continue;
    if (p.isKey(k.key)) {
      String v;
      switch (k.type) {
        case STR: v = p.getString(k.key, ""); break;
        case INT: v = String((int)p.getInt(k.key, 0)); break;
        case BOOL: v = p.getBool(k.key, false) ? "1" : "0"; break;
        case BYTES: {
          uint8_t buf[128];
          size_t n = p.getBytes(k.key, buf, sizeof buf);
          v = hex(buf, n);
          break;
        }
      }
      if (v.indexOf('\n') < 0) t += String(k.ns) + "." + k.key + "=" + v + "\n";
    }
    p.end();
  }
  return t;
}

bool card() { return SD.cardType() != CARD_NONE; }

}  // namespace

void backup() {
  if (!card()) return;
  String t = to_text();
  File f = SD.open(FILE_NAME, FILE_READ);
  if (f) {
    bool same = f.readString() == t;
    f.close();
    if (same) return;
  }
  f = SD.open(FILE_NAME, FILE_WRITE);
  if (!f) return;
  f.print(t);
  f.close();
  Serial.println("Einstellungen auf SD gesichert");
}

bool restore() {
  if (!card()) return false;
  {
    // Ist schon etwas eingestellt, gilt das NVS (die Karte ist nur die Kopie)
    Preferences p;
    p.begin("ota", true);
    bool have = p.isKey("ssid") || p.isKey("token");
    p.end();
    if (have) return false;
  }
  File f = SD.open(FILE_NAME, FILE_READ);
  if (!f) return false;
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (line.startsWith("#")) continue;
    int dot = line.indexOf('.'), eq = line.indexOf('=');
    if (dot < 1 || eq < dot) continue;
    String ns = line.substring(0, dot), key = line.substring(dot + 1, eq), v = line.substring(eq + 1);
    for (const Key& k : KEYS) {
      if (ns != k.ns || key != k.key) continue;
      Preferences p;
      p.begin(k.ns, false);
      switch (k.type) {
        case STR: p.putString(k.key, v); break;
        case INT: p.putInt(k.key, v.toInt()); break;
        case BOOL: p.putBool(k.key, v == "1"); break;
        case BYTES: {
          uint8_t buf[128];
          size_t len = std::min((size_t)v.length() / 2, sizeof buf);
          for (size_t i = 0; i < len; i++) buf[i] = strtol(v.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);
          p.putBytes(k.key, buf, len);
          break;
        }
      }
      p.end();
      n++;
    }
  }
  f.close();
  Serial.printf("Einstellungen von SD zurueckgeladen: %d Werte\n", n);
  return n > 0;
}

}  // namespace settings
