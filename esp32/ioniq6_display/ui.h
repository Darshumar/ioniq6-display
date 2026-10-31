// Anzeige fuer das Waveshare ESP32-S3-Touch-LCD-4.3 (800x480 quer), Seiten wie in der Android-Auto-App.
#pragma once
#include <time.h>

#include <string>
#include <vector>

#include "decode.h"

namespace ui {

void begin(bool backlight = true);
bool flipped();           // Anzeige um 180 Grad gedreht (Einstellung)
void backlight(bool on);  // zum Schlafen: bleibt im Tiefschlaf aus
// Helligkeit (in Software, das Licht ist nur an/aus): Werte fuer Tag und
// Nacht in %, nachts gilt die Nacht-Helligkeit, falls dunkler
constexpr int BRIGHT_MIN = 20;  // darunter ist das Panel praktisch schwarz
int brightness(bool night);
void save_brightness(int day, int night);
void preview_brightness(int pct);  // sofort so anzeigen (beim Einstellen)
void auto_brightness(time_t now);
// Regler-Bildschirm; brightness_hit aendert day/night und liefert die
// geaenderte Zeile (0 Tag, 1 Nacht), BR_DONE oder BR_NONE
constexpr int BR_NONE = -1, BR_DONE = -2;
void brightness_view(int day, int night);
int brightness_hit(int x, int y, int& day, int& night);
// Statuszeile, solange keine Daten da sind (z. B. "Suche Adapter ...")
void status(const char* line1, const char* line2 = "", const char* detail = "");
// Update: Titel, Fortschritt in % (< 0: ohne Balken), Zeile darunter, Hinweis unten
void update_view(const char* title, int pct, const char* line, const char* hint);
// Auswahlliste (z. B. OBD-Adapter): Eintrag sel ist markiert
struct ListItem {
  std::string title, sub;
  bool accent = false;  // Titel gruen (z. B. sieht nach OBD-Adapter aus)
};
void choice_list(const char* title, const std::vector<ListItem>& items, int sel, const char* footer);
// Welcher Eintrag der Liste liegt bei y (gleicher Bildlauf wie choice_list), -1 = keiner
int list_hit(int y, int sel, int count);
// Menue-Schaltflaeche: oben rechts auf den Seiten, unten auf den Statusbildschirmen
void menu_button();  // unten auf einen Statusbildschirm zeichnen
bool menu_hit(int x, int y, bool dashboard);
int sleep_hit(int x, int y);  // Statusbildschirm: 1 "Schlafen", 2 "Ausschalten", 0 keins
// Fehlersuche: wo ein Tippen auf dem Statusbildschirm ankam
void touch_debug(int x, int y, int raw_x, int raw_y);
// Touch-Test: Kreuz zum Antippen, danach Pruefbild mit Rohdaten
void touch_target(int n, int x, int y, const char* hint);
void touch_check(bool clear, const uint8_t* d, bool touching, int x, int y, int rx, int ry, const char* cal);
void set_demo(bool on);  // Kopfzeile zeigt dann DEMO statt OBD
constexpr int PAGES = 7;
constexpr int PAGE_DRIVE = 0, PAGE_BATTERY = 1, PAGE_CHARGE = 2, PAGE_DRIVETRAIN = 3, PAGE_TRIPS = 4,
              PAGE_TEMPS = 5, PAGE_CAR = 6;
// Versteckte Unterseiten ohne eigenen Reiter (hoch/runter wischen): G-Kraft
// unter Antrieb, Zellen unter Akku (Marc 27.10.)
constexpr int PAGE_G = PAGES, PAGE_CELLS = PAGES + 1;
// Reiter, zu dem eine Seite gehoert, und die Seite beim Hoch-/Runterwischen (-1 = keine)
constexpr int tab_of(int page) {
  return page == PAGE_G ? PAGE_DRIVETRAIN : (page == PAGE_CELLS ? PAGE_BATTERY : page);
}
constexpr int vswipe_page(int page) {
  return page == PAGE_DRIVETRAIN ? PAGE_G : page == PAGE_BATTERY ? PAGE_CELLS : page >= PAGES ? tab_of(page) : -1;
}
// Welche Seite liegt in der Kopfzeile bei x/y (Seitenleiste wie in der App), -1 = keine
int tab_hit(int x, int y);
// Antrieb-Seite: Punkte auf den HV-Leitungen weiterbewegen (oft aufrufen,
// zeichnet hoechstens alle 60 ms und nur, wenn die Seite zu sehen ist)
void animate();
// Fahrtenbuch ohne Auto (aus dem Menue); Tippen schliesst es
void trips_view(const TripLog& log);
void dashboard(const BmsData& d, const Consumption& c, const ChargeSession& cs, const Tires& t,
               const Cells& cells, const Vehicle& v, const TripLog& log, bool link_ok, int page);

}  // namespace ui
