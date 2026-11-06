#include "ui.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>

#include <algorithm>
#include <time.h>

#include "board.h"
#include "car_top_img.h"
#include "chassis_img.h"
#include "exio.h"
#include "i18n.h"
#include "logo.h"
#include "sdlog.h"

namespace {

// Waveshare ESP32-S3-Touch-LCD-4.3: RGB-Panel 800x480 (Timing aus den
// Arduino_GFX-Beispielen fuer dieses Board); Licht und Reset am CH422G
Arduino_ESP32RGBPanel* rgb = new Arduino_ESP32RGBPanel(
    5 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 7 /* PCLK */,
    1 /* R0 */, 2 /* R1 */, 42 /* R2 */, 41 /* R3 */, 40 /* R4 */,
    39 /* G0 */, 0 /* G1 */, 45 /* G2 */, 48 /* G3 */, 47 /* G4 */, 21 /* G5 */,
    14 /* B0 */, 38 /* B1 */, 18 /* B2 */, 17 /* B3 */, 10 /* B4 */,
    // Porches wie in Waveshares eigenem ESP-IDF-Beispiel fuer das 4.3"-Board
    // (vorher die Werte aus den GFX-Beispielen 40/48/88, 13/3/32: Fransen an weissen Ziffern).
    0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
    // Senkrecht laengere Pause (48 statt 20 Zeilen, ca. 2,4 ms): der Treiber
    // startet in jedem VSYNC neu und kopiert dabei beide Bounce-Buffer aus dem
    // PSRAM. Ist der Bus durch Bluetooth (Code aus dem Flash) belegt und wird
    // gleichzeitig neu gezeichnet, reichte die kurze Pause nicht: das ganze Bild
    // wackelte seitlich (Marc 23.10., FW 59-62).
    0 /* vsync_polarity */, 13 /* vsync_front_porch */, 3 /* vsync_pulse_width */, 32 /* vsync_back_porch */,
    // 16 MHz lassen: mit 12 MHz (FW 53) blieb das Panel dunkel (Marc 23.10.).
    1 /* pclk_active_neg */, 16000000 /* prefer_speed */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // Bounce-Buffer (2 x 10 Zeilen im internen RAM, ab Core 3 / IDF 5) gegen das
    // Flackern beim Neuzeichnen (Core 2.0.17 ohne Bounce-Buffer flackerte, FW 52-56).
    // Klein halten: beide werden im VSYNC-Interrupt nachgeladen (FW 59-62 mit
    // 40 Zeilen = 128 KB je Bild, das dauerte laenger als die Pause).
    // Braucht die Flash-Sperre in main.cpp (--wrap=spi_flash_..._other_cpu).
    , false /* useBigEndian */, 0 /* de_idle_high */, 0 /* pclk_idle_high */, 800 * 10 /* bounce_buffer_size_px */
#endif
);
Arduino_GFX* panel = new Arduino_RGB_Display(800, 480, rgb, 0, true);
// Gezeichnet wird in einen Puffer im PSRAM und dann am Stueck uebertragen,
// so flackert nichts und die Drehung geht in Software.
Arduino_Canvas* gfx = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel, 0, 0, 0);

// Puffer aufs Panel bringen, aber nur die Zeilen, die sich seit dem letzten
// Mal geaendert haben. Das ganze Bild (750 KB PSRAM -> PSRAM) bei jedem
// Neuzeichnen umzukopieren nimmt dem RGB-Panel die Speicherbandbreite, dann
// flackert das Bild bei jeder Aktualisierung.
uint32_t row_hash[SCREEN_H];

// Helligkeit in Software: das Licht des 4.3" ist nur an/aus (CH422G), also
// werden die Farben beim Uebertragen abgedunkelt. Schwarz bleibt so hell wie
// vorher, alles andere wird dunkler. 256 = unveraendert.
int dim = 256;
uint8_t dim_r[32], dim_g[64];  // 5- und 6-Bit-Kanal -> abgedunkelt (Blau wie Rot)
uint16_t* dim_buf = nullptr;    // 16 Zeilen im internen RAM

void set_dim(int level) {
  level = std::max(16, std::min(256, level));
  if (level == dim) return;
  dim = level;
  for (int i = 0; i < 32; i++) dim_r[i] = (i * level + 128) >> 8;
  for (int i = 0; i < 64; i++) dim_g[i] = (i * level + 128) >> 8;
  memset(row_hash, 0, sizeof row_hash);  // naechstes show() uebertraegt alles neu
}

void push(const uint16_t* fb, int y0, int y1) {
  if (dim >= 256) {
    panel->draw16bitRGBBitmap(0, y0, const_cast<uint16_t*>(fb + y0 * SCREEN_W), SCREEN_W, y1 - y0);
    return;
  }
  if (!dim_buf) {
    dim_buf = (uint16_t*)heap_caps_malloc(SCREEN_W * 16 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dim_buf) dim_buf = (uint16_t*)heap_caps_malloc(SCREEN_W * 16 * 2, MALLOC_CAP_SPIRAM);
  }
  for (int y = y0; y < y1; y += 16) {
    int n = std::min(16, y1 - y);
    const uint16_t* src = fb + y * SCREEN_W;
    for (int i = 0; i < n * SCREEN_W; i++) {
      uint16_t c = src[i];
      dim_buf[i] = (dim_r[c >> 11] << 11) | (dim_g[(c >> 5) & 63] << 5) | dim_r[c & 31];
    }
    panel->draw16bitRGBBitmap(0, y, dim_buf, SCREEN_W, n);
  }
}

void show() {
  uint16_t* fb = gfx->getFramebuffer();
  // Vor ui::begin() gibt es noch keinen Puffer (auto_brightness laeuft im
  // setup() davor): FW 65 stuerzte nachts deshalb beim Start ab, Bild schwarz
  if (!fb) return;
  int start = -1;
  for (int y = 0; y <= SCREEN_H; y++) {
    // Alle 16 Zeilen kurz Pause: Flash und PSRAM teilen sich einen Bus. Laeuft
    // Bluetooth (Code aus dem Flash auf Kern 0), reichte die Bandbreite beim
    // Durchrechnen des ganzen Bildes nicht mehr fuer das Display, das ganze
    // Bild wackelte bei jedem Neuzeichnen (Marc 23.10., FW 59-61).
    if (y % 16 == 0 && y) {
      if (start >= 0) {  // angefangenen Block erst uebertragen
        push(fb, start, y);
        start = -1;
      }
      vTaskDelay(1);
    }
    bool changed = false;
    if (y < SCREEN_H) {
      const uint32_t* p = reinterpret_cast<const uint32_t*>(fb + y * SCREEN_W);
      uint32_t h = 2166136261u;  // FNV-1a ueber die Zeile
      for (int i = 0; i < SCREEN_W / 2; i++) h = (h ^ p[i]) * 16777619u;
      changed = h != row_hash[y];
      row_hash[y] = h;
    }
    if (changed && start < 0) start = y;
    if (!changed && start >= 0) {
      push(fb, start, y);
      start = -1;
    }
  }
}

// Farben wie in der Android-Auto-App (DashboardRenderer.kt), als RGB565
constexpr uint16_t C_BG = 0x08A3, C_FG = 0xFFFF, C_DIM = 0x8CD4, C_LINE = 0x29A8, C_PANEL = 0x1925;
constexpr uint16_t C_GREEN = 0x2E2F, C_YELLOW = 0xF647, C_RED = 0xE227, C_BLUE = 0x4C9B, C_ORANGE = 0xFD06;
constexpr uint16_t C_FLOW = 0xFE8F;  // Punkte auf den HV-Leitungen beim Antreiben

const uint8_t* F_TINY = u8g2_font_helvR12_tf;
const uint8_t* F_TINYB = u8g2_font_helvB12_tf;
const uint8_t* F_SMALL = u8g2_font_helvR14_tf;
const uint8_t* F_LABEL = u8g2_font_helvR18_tf;
const uint8_t* F_MED = u8g2_font_helvB18_tf;
const uint8_t* F_LARGE = u8g2_font_helvB24_tf;
const uint8_t* F_NUM = u8g2_font_logisoso24_tn;  // nur Ziffern . - :
const uint8_t* F_BIG = u8g2_font_logisoso62_tn;

uint16_t temp_color(int t) {
  if (t < 10) return C_BLUE;
  if (t < 35) return C_GREEN;
  if (t < 45) return C_YELLOW;
  return C_RED;
}

void text(int x, int y, const uint8_t* font, uint16_t color, const char* s) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);  // y = Grundlinie
  gfx->print(s);
}

int text_width(const uint8_t* font, const char* s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void text_right(int x_right, int y, const uint8_t* font, uint16_t color, const char* s) {
  text(x_right - text_width(font, s), y, font, color, s);
}

void text_center(int x, int y, const uint8_t* font, uint16_t color, const char* s) {
  text(x - text_width(font, s) / 2, y, font, color, s);
}

// Dezimalpunkt -> Komma (wie die App); nur fuer Schriften mit Komma (helv).
// Auf Englisch bleibt der Punkt.
char* de(char* s) {
  if (i18n::en()) return s;
  for (char* p = s; *p; p++)
    if (*p == '.' && p[1] >= '0' && p[1] <= '9' && p > s && p[-1] >= '0' && p[-1] <= '9') *p = ',';
  return s;
}

// Feld: Beschriftung oben, Wert darunter; passt der Wert nicht in maxw, kleiner
void field(int x, int y, const char* label, const char* value, uint16_t color = C_FG, int maxw = 9999) {
  text(x, y, F_SMALL, C_DIM, label);
  text(x, y + 32, text_width(F_LARGE, value) > maxw ? F_MED : F_LARGE, color, value);
}

void hline(int y) { gfx->drawFastHLine(24, y, SCREEN_W - 48, C_LINE); }

}  // namespace

namespace ui {

constexpr int W = SCREEN_W, H = SCREEN_H;
constexpr int TOP = 46;  // Hoehe der Kopfzeile
int shown_page = -1;  // welche Seite gerade zu sehen ist, -1 = anderer Bildschirm

bool bl_on = false;

void backlight(bool on) {
  bl_on = on;
  exio::set(exio::DISP, on);  // nur an/aus, gedimmt wird in show()
}

// Regler-Werte in %: [0] Tag, [1] Nacht (Sonne unter dem Horizont)
int bright_pct[2] = {-1, -1};

void load_brightness() {
  if (bright_pct[0] >= 0) return;
  Preferences p;
  p.begin("ui", true);
  bright_pct[0] = std::max(BRIGHT_MIN, std::min(100, (int)p.getInt("bright", 100)));
  // frueher im Portal, dort gingen auch 5 %: auf dem Panel praktisch schwarz
  bright_pct[1] = std::max(BRIGHT_MIN, std::min(100, (int)p.getInt("night", 25)));
  p.end();
}

int brightness(bool night) {
  load_brightness();
  return bright_pct[night];
}

void save_brightness(int day, int night) {
  bright_pct[0] = day;
  bright_pct[1] = night;
  Preferences p;
  p.begin("ui", false);
  p.putInt("bright", day);
  p.putInt("night", night);
  p.end();
}

void preview_brightness(int pct) {
  set_dim(std::max(BRIGHT_MIN, std::min(100, pct)) * 256 / 100);
  show();
}

// Nachts (Sonne unter dem Horizont, Mitte Deutschlands) die Nacht-Helligkeit,
// falls sie dunkler ist als die fuer den Tag
void auto_brightness(time_t now) {
  load_brightness();
  int pct = bright_pct[0];
  if (now > 1700000000 && bright_pct[1] < pct) {
    struct tm tm;
    gmtime_r(&now, &tm);
    float elev = sun_elevation(tm.tm_yday, tm.tm_hour + tm.tm_min / 60.0f, 51.0f, 10.0f);
    if (elev < -1) pct = bright_pct[1];
  }
  if (pct * 256 / 100 == dim) return;
  set_dim(pct * 256 / 100);
  show();
}

// Zwei Regler (Tag, Nacht): Minus, Balken, Plus; darunter "Fertig"
constexpr int BR_H = 70, BR_X0 = 150, BR_X1 = W - 150, BR_DONE_Y = 360;
constexpr int BR_Y[2] = {110, 250};

void brightness_view(int day, int night) {
  shown_page = -1;
  gfx->fillScreen(C_BG);
  text(24, 40, F_LARGE, C_FG, T("Helligkeit", "Brightness"));
  const char* label[2] = {T("Tag", "Day"), T("Nacht (nach Sonnenuntergang)", "Night (after sunset)")};
  int val[2] = {day, night};
  for (int r = 0; r < 2; r++) {
    int y0 = BR_Y[r];
    char b[16];
    snprintf(b, sizeof b, "%d %%", val[r]);
    text(24, y0 - 12, F_LABEL, C_DIM, label[r]);
    text(W - 24 - text_width(F_LARGE, b), y0 - 10, F_LARGE, C_FG, b);
    for (int i = 0; i < 2; i++) {  // Minus / Plus
      int x = i ? W - 124 : 24;
      gfx->fillRoundRect(x, y0, 100, BR_H, 10, C_PANEL);
      gfx->drawRoundRect(x, y0, 100, BR_H, 10, C_DIM);
      gfx->fillRect(x + 30, y0 + BR_H / 2 - 4, 40, 8, C_FG);
      if (i) gfx->fillRect(x + 46, y0 + BR_H / 2 - 20, 8, 40, C_FG);
    }
    int fill = BR_X0 + (BR_X1 - BR_X0) * (val[r] - BRIGHT_MIN) / (100 - BRIGHT_MIN);
    gfx->fillRoundRect(BR_X0, y0 + 17, BR_X1 - BR_X0, BR_H - 34, 18, C_PANEL);
    gfx->fillRoundRect(BR_X0, y0 + 17, std::max(36, fill - BR_X0), BR_H - 34, 18, C_BLUE);
    gfx->fillCircle(fill, y0 + BR_H / 2, 24, C_FG);
  }
  gfx->fillRoundRect(W / 2 - 120, BR_DONE_Y, 240, 56, 10, C_PANEL);
  gfx->drawRoundRect(W / 2 - 120, BR_DONE_Y, 240, 56, 10, C_DIM);
  text(W / 2 - text_width(F_MED, T("Fertig", "Done")) / 2, BR_DONE_Y + 36, F_MED, C_FG, T("Fertig", "Done"));
  gfx->drawFastHLine(0, H - 40, W, C_LINE);
  text(24, H - 14, F_SMALL, C_DIM, T("Nachts gilt die Nacht-Helligkeit, wenn sie dunkler ist", "At night the night brightness applies if it is darker"));
  show();
}

int brightness_hit(int x, int y, int& day, int& night) {
  if (y >= BR_DONE_Y - 10 && y < BR_DONE_Y + 70 && x > W / 2 - 140 && x < W / 2 + 140) return BR_DONE;
  for (int r = 0; r < 2; r++) {
    if (y < BR_Y[r] - 25 || y > BR_Y[r] + BR_H + 15) continue;
    int& pct = r ? night : day;
    int v;
    if (x < 140) v = pct - 5;
    else if (x > W - 140) v = pct + 5;
    else v = (BRIGHT_MIN + (x - BR_X0) * (100 - BRIGHT_MIN) / (BR_X1 - BR_X0) + 2) / 5 * 5;
    v = std::max(BRIGHT_MIN, std::min(100, v));
    if (v == pct) return BR_NONE;
    pct = v;
    return r;
  }
  return BR_NONE;
}

bool flipped() {
  Preferences p;
  p.begin("ui", true);
  bool flip = p.getBool("flip", false);
  p.end();
  return flip;
}

#ifdef IONIQ6_DIAG
// Testbau: Start des Displays ueber USB protokollieren (Monitor 115200 Baud)
void diag(const char* step) {
  Serial.printf("DIAG %6lu ms  %-28s  intern frei %u  PSRAM frei %u\n", millis(), step,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.flush();
}
#else
void diag(const char*) {}
#endif

void begin(bool light) {
#ifdef IONIQ6_DIAG
  delay(6000);  // Zeit, den seriellen Monitor zu oeffnen
  Serial.printf("DIAG Core %d.%d.%d, PSRAM %s\n", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH, psramFound() ? "ja" : "NEIN");
#endif
  bool flip = flipped();
  diag("vor exio::begin");
  exio::begin();  // Display aus dem Reset, I2C-Bus
  diag("vor gfx->begin");
  bool ok = gfx->begin();
  diag(ok ? "gfx->begin ok" : "gfx->begin FEHLGESCHLAGEN");
#ifdef IONIQ6_DIAG
  Serial.printf("DIAG Canvas-Puffer %p\n", (void*)gfx->getFramebuffer());
#endif
  (void)ok;
  gfx->setRotation(flip ? 2 : 0);  // quer, auf Wunsch um 180 Grad gedreht
  gfx->setUTF8Print(true);
  gfx->fillScreen(C_BG);
  show();
  diag("erstes Bild gezeigt");
  backlight(light);
  diag("Licht an");
}

// Logo mit Deckung 0..15 in Farbe r/g/b auf den Hintergrund mischen
void draw_logo(int x0, int y0, uint8_t r, uint8_t g, uint8_t b) {
  constexpr int bg_r = 11, bg_g = 20, bg_b = 26;
  for (int y = 0; y < LOGO_H; y++) {
    for (int x = 0; x < LOGO_W; x++) {
      int i = y * LOGO_W + x;
      int a = (LOGO_A[i >> 1] >> ((i & 1) ? 0 : 4)) & 0x0F;
      if (!a) continue;
      gfx->drawPixel(x0 + x, y0 + y,
                     gfx->color565(bg_r + (r - bg_r) * a / 15, bg_g + (g - bg_g) * a / 15, bg_b + (b - bg_b) * a / 15));
    }
  }
}

void status(const char* line1, const char* line2, const char* detail) {
  shown_page = -1;
  gfx->fillScreen(C_BG);
  draw_logo(W - LOGO_W - 40, 150, 0x16, 0x3D, 0x7D);  // Hyundai-Blau, dunkel genug fuer Text darueber
  // Uhrzeit oben links, sobald die Uhr gestellt ist (WLAN oder aus dem Schlaf weitergezaehlt)
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    char clock[8];
    strftime(clock, sizeof clock, "%H:%M", &tm);
    text(24, 34, F_MED, C_FG, clock);
  }
  text_center(W / 2, 34, F_LARGE, C_FG, "Ioniq 6");
  text(24, 92, F_LARGE, C_FG, line1);
  text(24, 124, F_LABEL, C_DIM, line2);
  // Diagnose: eine Zeile pro Eintrag (durch \n getrennt), zu lange werden gekuerzt
  char row[64];
  int y = 162;
  for (const char* p = detail; *p && y < H - 140; y += 24) {
    int n = 0;
    while (*p && *p != '\n' && n < 56) row[n++] = *p++;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    row[n] = 0;
    text(24, y, F_SMALL, C_YELLOW, row);
  }
  show();
}

// Update-Bildschirm: Titel, Fortschrittsbalken (pct < 0: ohne Balken) und
// Hinweis. Wird beim Herunterladen alle 5 % neu gezeichnet; waehrend des
// Flash-Schreibens bleibt das Licht aus (das Bild verzerrt sonst, FW 79).
void update_view(const char* title, int pct, const char* line, const char* hint) {
  shown_page = -1;
  gfx->fillScreen(C_BG);
  text_center(W / 2, 34, F_LARGE, C_FG, "Ioniq 6");
  text(24, 92, F_LARGE, C_FG, title);
  if (pct >= 0) {
    char buf[8];
    snprintf(buf, sizeof buf, "%d", pct);
    text(24, 220, F_BIG, C_FG, buf);
    text(24 + text_width(F_BIG, buf) + 8, 220, F_LARGE, C_DIM, "%");
    constexpr int BX = 24, BY = 250, BW = W - 48, BH = 28;
    gfx->fillRoundRect(BX, BY, BW, BH, BH / 2, C_LINE);
    int fw = BW * std::min(pct, 100) / 100;
    if (fw >= BH) gfx->fillRoundRect(BX, BY, fw, BH, BH / 2, C_BLUE);
  }
  text(24, pct >= 0 ? 316 : 140, F_LABEL, C_DIM, line);
  text(24, H - 40, F_MED, C_YELLOW, hint);
  show();
}

namespace {

bool demo = false;

const char* const TITLES_DE[PAGES] = {"Fahren", "Akku", "Laden", "Antrieb", "Fahrten", "Temp.", "Auto"};
const char* const TITLES_EN[PAGES] = {"Drive", "Battery", "Charge", "Motors", "Trips", "Temp.", "Car"};
#define TITLES (i18n::en() ? TITLES_EN : TITLES_DE)
int tab_x0[PAGES], tab_x1[PAGES];
constexpr int MENU_X = W - 56;  // ab hier oben rechts: Menue-Zeichen

void menu_icon() {
  for (int i = 0; i < 3; i++) gfx->fillRoundRect(W - 46, 12 + i * 9, 26, 4, 2, C_FG);
}

// Kopfzeile wie in der App: links Verbindung, rechts die Seiten zum Antippen
void header(bool link_ok, int page) {
  page = tab_of(page);  // Unterseite: ihr Reiter bleibt markiert
  if (demo) text(16, 30, F_TINYB, C_YELLOW, "DEMO");
  else text(16, 30, F_SMALL, link_ok ? C_GREEN : C_RED, "OBD");
  if (sdlog::active()) text(62, 30, F_TINY, C_DIM, "SD");  // es wird mitgeloggt
  int x = MENU_X - 16;
  for (int i = PAGES - 1; i >= 0; i--) {
    const uint8_t* f = i == page ? F_MED : F_LABEL;
    int w = text_width(f, TITLES[i]);
    text(x - w, 30, f, i == page ? C_FG : C_DIM, TITLES[i]);
    if (i == page) gfx->fillRect(x - w, 38, w, 3, C_BLUE);
    tab_x0[i] = x - w;
    tab_x1[i] = x;
    x -= w + 14;
  }
  menu_icon();
}

const char* thermal_label(const BmsData& d, uint16_t& color) {
  switch (battery_thermal(d)) {
    case Thermal::Pump: color = C_FG; return T("Pumpe läuft", "Pump on");
    case Thermal::Heating: color = C_ORANGE; return T("Heizung an", "Heating");
    case Thermal::Cooling: color = C_BLUE; return T("Kühlung an", "Cooling");
    default: color = C_DIM; return T("aus", "off");
  }
}

// Ladestand der 12-V-Batterie: orange unter 70 %, rot unter 50 %
uint16_t aux_color(const Vehicle& v) { return v.aux_soc < 50 ? C_RED : (v.aux_soc < 70 ? C_ORANGE : C_FG); }

// Minuten als "45 min" oder "1:20 h", negativ = "-"
void fmt_minutes(char* buf, size_t n, float min) {
  if (min < 0) {
    snprintf(buf, n, "-");
    return;
  }
  int m = (int)(min + 0.5f);
  if (m < 60) snprintf(buf, n, "%d min", m);
  else snprintf(buf, n, "%d:%02d h", m / 60, m % 60);
}

// Grosse Zahl mit kleiner Einheit dahinter; x ist die linke Kante
void big_value(int x, int y, const char* value, const char* unit, uint16_t color,
               const uint8_t* font = F_BIG, const uint8_t* unit_font = F_LABEL) {
  text(x, y, font, color, value);
  text(x + text_width(font, value) + 8, y, unit_font, C_DIM, unit);
}

// Drei Spalten oben wie in der App (Fahren, Laden)
constexpr int PAD = 24, COL = (W - 2 * PAD) / 3;
constexpr int X1 = PAD, X2 = X1 + COL, X3 = X2 + COL;
constexpr int Y_LABEL = 82, Y_BIG = 154, Y_SUB = 184, Y_DIV = 236;

// Leistung gegen das, was der Akku gerade hergibt: links rekuperieren bis zur
// Ladegrenze, rechts antreiben bis zur Entladegrenze (beide meldet das BMS)
void power_bar(const BmsData& d, int x, int y, int bw, int bh, uint16_t color) {
  float lo = d.charge_limit_kw > 0 ? d.charge_limit_kw : 100, hi = d.discharge_limit_kw > 0 ? d.discharge_limit_kw : 250;
  int zero = x + (int)(bw * lo / (lo + hi));
  gfx->fillRoundRect(x, y, bw, bh, bh / 2, C_LINE);
  float p = d.power_kw;
  if (p > 0) gfx->fillRect(zero, y, (int)(std::min(p / hi, 1.0f) * (x + bw - zero)), bh, color);
  if (p < 0) {
    int w = (int)(std::min(-p / lo, 1.0f) * (zero - x));
    gfx->fillRect(zero - w, y, w, bh, color);
  }
  gfx->fillRect(zero - 1, y - 3, 3, bh + 6, C_FG);
}

// Verbrauch je km als Saeulen, gelbe Linie beim Fahrtschnitt
void consumption_bars(const Consumption& c, int x, int y, int bw, int bh) {
  constexpr float TOP_KWH = 40;
  float slot = (float)bw / Consumption::HIST;
  gfx->fillRect(x, y + bh - 2, bw, 2, C_LINE);
  for (int i = 0; i < c.hist_n; i++) {
    float f = std::min(std::max(c.hist[i], 0.0f) / TOP_KWH, 1.0f);
    int h = (int)(f * bh);
    gfx->fillRect(x + (int)(i * slot + slot * 0.15f), y + bh - h, std::max(1, (int)(slot * 0.7f)), h, C_BLUE);
  }
  float t = c.trip_kwh_100km();
  if (t > 0 && t < TOP_KWH) gfx->fillRect(x, y + bh - (int)(t / TOP_KWH * bh) - 1, bw, 3, C_YELLOW);
  if (!c.hist_n) text_center(x + bw / 2, y + bh / 2, F_LABEL, C_DIM, T("noch kein voller km", "no full km yet"));
}

// Ladekurve: x Ladestand 0..100 %, y kW bis zur naechsten 50er-Stufe ueber der Spitze
void charge_curve(const ChargeSession& cs, int x, int y, int cw, int ch) {
  char buf[16];
  float top = cs.peak_kw > 11 ? (float)(((int)cs.peak_kw / 50 + 1) * 50) : 12;
  gfx->fillRect(x, y + ch - 2, cw, 2, C_LINE);
  gfx->fillRect(x, y, 2, ch, C_LINE);
  snprintf(buf, sizeof buf, "%.0f", top);
  text_right(x - 6, y + 12, F_TINY, C_DIM, buf);
  for (int p = 0; p <= 100; p += 20) {
    snprintf(buf, sizeof buf, "%d", p);
    text_center(x + p * cw / 100, y + ch + 18, F_TINY, C_DIM, buf);
  }
  auto px = [&](int k) { return x + (int)(cs.curve_soc[k] * cw / 100); };
  auto py = [&](int k) { return y + ch - 1 - (int)(std::min(cs.curve_kw[k], top) / top * (ch - 2)); };
  for (int i = 1; i < cs.curve_n; i++) {
    for (int o = -1; o <= 1; o++) gfx->drawLine(px(i - 1), py(i - 1) + o, px(i), py(i) + o, C_GREEN);
  }
}

void page_drive(const BmsData& d, const Consumption& c, const Vehicle& car) {
  char buf[64];
  // Ladestand
  text(X1, Y_LABEL, F_LABEL, C_DIM, T("Ladestand", "State of charge"));
  snprintf(buf, sizeof buf, "%.1f", d.soc_display);
  big_value(X1, Y_BIG, buf, "%", C_FG);
  snprintf(buf, sizeof buf, d.energy_kwh > 0 ? "%.1f kWh" : "~%.1f kWh", remaining_kwh(d));
  text(X1, Y_SUB, F_LABEL, C_DIM, de(buf));

  // Leistung: gruen beim Rekuperieren/Laden, orange ab 50 kW
  float p = d.power_kw;
  uint16_t pc = p < -0.5f ? C_GREEN : (p > 50 ? C_ORANGE : C_FG);
  text(X2, Y_LABEL, F_LABEL, C_DIM, T("Leistung", "Power"));
  snprintf(buf, sizeof buf, fabsf(p) >= 100 ? "%.0f" : "%.1f", p);
  big_value(X2, Y_BIG, buf, "kW", pc);
  snprintf(buf, sizeof buf, "%.0f km/h · %.0f V · %.0f A", speed_kmh(d, car), d.voltage, d.current);
  text(X2, Y_SUB, F_SMALL, C_DIM, buf);
  power_bar(d, X2, Y_SUB + 14, COL - PAD, 14, pc);

  // Reichweite aus dem Verbrauch der letzten bis zu 25 km
  text(X3, Y_LABEL, F_LABEL, C_DIM, T("Reichweite", "Range"));
  float range = c.range_km(d);
  if (range >= 0) {
    snprintf(buf, sizeof buf, "%.0f", range);
    big_value(X3, Y_BIG, buf, "km", C_FG);
    snprintf(buf, sizeof buf, T("bei %.1f kWh/100 km", "at %.1f kWh/100 km"), c.recent_kwh_100km());
    text(X3, Y_SUB, F_SMALL, C_DIM, de(buf));
  } else {
    text(X3, Y_BIG - 12, F_LARGE, C_DIM, T("ab 3 km", "after 3 km"));
    text(X3, Y_SUB, F_SMALL, C_DIM, T("erst nach 3 gefahrenen km", "available after 3 km driven"));
  }
  hline(Y_DIV);

  // Verlauf links, Fahrtwerte rechts
  text(X1, Y_DIV + 30, F_SMALL, C_DIM, T("Verbrauch je km, letzte 20 km (0-40 kWh/100 km)", "Consumption per km, last 20 km (0-40 kWh/100 km)"));
  consumption_bars(c, X1, Y_DIV + 44, 2 * COL - PAD, H - PAD - (Y_DIV + 44));
  constexpr int FX2 = X3 + COL / 2, FY1 = Y_DIV + 30, FY2 = FY1 + 104;
  snprintf(buf, sizeof buf, "%ld km", c.trip_km());
  constexpr int FW = COL / 2 - 8;
  field(X3, FY1, T("Fahrt", "Trip"), buf, C_FG, FW);
  float trip = c.trip_kwh_100km();
  if (trip >= 0) snprintf(buf, sizeof buf, "%.1f", trip);
  else snprintf(buf, sizeof buf, "-");
  field(FX2, FY1, "kWh/100", de(buf), C_FG, FW);
  snprintf(buf, sizeof buf, "%.2f kWh", c.regen_kwh);
  field(X3, FY2, T("rekuperiert", "regenerated"), de(buf), C_GREEN, FW);
  snprintf(buf, sizeof buf, "%d-%d °C", d.temp_min, d.temp_max);
  field(FX2, FY2, T("Akku", "Battery"), buf, temp_color(d.temp_max), FW);
}

void page_battery(const BmsData& d, const Vehicle& v) {
  char buf[64];
  // 16 Module als 4 x 4 Kacheln auf der linken Haelfte, waermstes umrandet
  int lo = d.modules[0], hi = d.modules[0];
  for (int i = 1; i < 16; i++) {
    lo = std::min(lo, (int)d.modules[i]);
    hi = std::max(hi, (int)d.modules[i]);
  }
  constexpr int GAP = 12, GX = PAD, GY = TOP + 18, GW = W / 2 - PAD;
  constexpr int TW = (GW - 3 * GAP) / 4, TH = (H - GY - PAD - 3 * GAP) / 4;
  for (int i = 0; i < 16; i++) {
    int x = GX + (i % 4) * (TW + GAP), y = GY + (i / 4) * (TH + GAP);
    gfx->fillRoundRect(x, y, TW, TH, 9, temp_color(d.modules[i]));
    if (d.modules[i] == hi && hi != lo) {
      gfx->drawRoundRect(x - 4, y - 4, TW + 8, TH + 8, 11, C_FG);
      gfx->drawRoundRect(x - 3, y - 3, TW + 6, TH + 6, 10, C_FG);
    }
    snprintf(buf, sizeof buf, "%d", i + 1);
    text(x + 8, y + 20, F_TINY, C_BG, buf);
    snprintf(buf, sizeof buf, "%d°", d.modules[i]);
    text_center(x + TW / 2, y + TH - 18, F_LARGE, C_BG, buf);
  }

  constexpr int bx1 = W / 2 + 28, bcol = (W - PAD - bx1) / 2, bx2 = bx1 + bcol;
  snprintf(buf, sizeof buf, T("Module %d-%d °C (Spreizung %d)", "Modules %d-%d °C (spread %d)"), lo, hi, hi - lo);
  text(bx1, GY + 20, F_LABEL, C_DIM, buf);
  constexpr int by1 = GY + 70, brow = 84;
  snprintf(buf, sizeof buf, "%.2f-%.2f V", d.cell_v_min, d.cell_v_max);
  field(bx1, by1, T("Zellspannung", "Cell voltage"), de(buf));
  float diff = (d.cell_v_max - d.cell_v_min) * 1000;
  char lab[40];
  snprintf(lab, sizeof lab, T("Differenz (#%d / #%d)", "Difference (#%d / #%d)"), d.cell_v_min_no, d.cell_v_max_no);
  snprintf(buf, sizeof buf, "%.0f mV", diff);
  field(bx2, by1, lab, buf, diff >= 100 ? C_ORANGE : C_FG);
  snprintf(buf, sizeof buf, "%.1f %%", d.soh);
  field(bx1, by1 + brow, "SoH", de(buf));
  snprintf(buf, sizeof buf, "%.1f %%", d.soc_bms);
  field(bx2, by1 + brow, T("BMS-Ladestand", "BMS SoC"), de(buf));
  if (d.has_coolant) snprintf(buf, sizeof buf, "%d °C", d.coolant_inlet);
  else snprintf(buf, sizeof buf, "-");
  field(bx1, by1 + 2 * brow, T("Kühlmittel", "Coolant"), buf);
  uint16_t sc;
  const char* st = thermal_label(d, sc);
  field(bx2, by1 + 2 * brow, T("Temperierung", "Thermal mgmt"), d.has_coolant ? st : "-", sc);
  // 12-V-Batterie: Ladestand und Strom vom Wandler (ICCU), sonst nur die Spannung vom BMS
  if (v.has_aux) {
    snprintf(buf, sizeof buf, "%.0f %% · %.1f V", v.aux_soc, v.aux_v);
    field(bx1, by1 + 3 * brow, T("12-V-Batterie", "12 V battery"), de(buf), aux_color(v), bcol - 8);
    snprintf(buf, sizeof buf, "%+.1f A · %.0f °C", v.aux_a, v.aux_c);
    field(bx2, by1 + 3 * brow, T("12 V Strom, Temp.", "12 V current, temp."), de(buf), C_FG, bcol - 8);
  } else {
    snprintf(buf, sizeof buf, "%.1f V", d.aux_v);
    field(bx1, by1 + 3 * brow, T("12-V-Batterie", "12 V battery"), de(buf), d.aux_v >= 5 && d.aux_v <= 12 ? C_ORANGE : C_FG);
  }
  // Isolationswiderstand des HV-Akkus gegen die Karosserie
  if (d.isolation_kohm >= 0) {
    if (d.isolation_kohm >= 1000) snprintf(lab, sizeof lab, "%.1f MOhm", d.isolation_kohm / 1000.0f);
    else snprintf(lab, sizeof lab, "%d kOhm", d.isolation_kohm);
    snprintf(buf, sizeof buf, T("Isolation %s", "Insulation %s"), de(lab));
    text(bx1, H - 12, F_SMALL, d.isolation_kohm < 500 ? C_YELLOW : C_DIM, buf);
  }
}

void page_charge(const BmsData& d, const ChargeSession& cs, const Vehicle& v) {
  char buf[64];
  text(X1, Y_LABEL, F_LABEL, C_DIM, T("Ladestand", "State of charge"));
  snprintf(buf, sizeof buf, "%.1f", d.soc_display);
  big_value(X1, Y_BIG, buf, "%", C_FG);
  snprintf(buf, sizeof buf, d.energy_kwh > 0 ? "%.1f kWh" : "~%.1f kWh", remaining_kwh(d));
  text(X1, Y_SUB, F_LABEL, C_DIM, de(buf));

  // AC und DC zeigen unten verschiedene Werte: DC die Saeule und was das Auto
  // anfordert, AC das Netz und den Bordlader
  bool dc = v.has_charge ? v.dc : cs.dc();
  bool ac = cs.active && !dc;
  text(X2, Y_LABEL, F_LABEL, C_DIM, T("Ladeleistung", "Charging power"));
  snprintf(buf, sizeof buf, "%.1f", cs.active ? cs.kw : 0.0f);
  big_value(X2, Y_BIG, buf, "kW", cs.active ? C_GREEN : C_DIM);
  if (ac && v.has_obc) {
    snprintf(buf, sizeof buf, T("Netz %.0f V · %.1f A · %.0f Hz", "Grid %.0f V · %.1f A · %.0f Hz"), v.obc_ac_v, v.obc_ac_a, v.obc_ac_hz);
    text(X2, Y_SUB, F_SMALL, C_DIM, de(buf));
  } else if (dc && v.has_charge && cs.active && v.evse_v > 0) {
    // Die Saeule liefert mehr als im Akku ankommt, solange das Auto den Akku
    // heizt (31.10./02.11.: 4-6 kW), sonst gleich (+-1,5 kW)
    snprintf(buf, sizeof buf, T("%.0f V · %.0f A · Säule %.0f kW", "%.0f V · %.0f A · charger %.0f kW"),
             d.voltage, -d.current, v.evse_v * v.evse_a / 1000);
    text(X2, Y_SUB, F_SMALL, C_DIM, de(buf));
  } else {
    snprintf(buf, sizeof buf, "%.0f V · %.0f A", d.voltage, -d.current);
    text(X2, Y_SUB, F_LABEL, C_DIM, buf);
  }

  // Restzeit bei der aktuellen Leistung (DC wird oberhalb ~80 % langsamer)
  text(X3, Y_LABEL, F_LABEL, C_DIM, T("bis 80 %", "to 80 %"));
  float t80 = cs.minutes_to(80, d);
  fmt_minutes(buf, sizeof buf, t80);
  text(X3, Y_BIG - 10, u8g2_font_fub30_tf, t80 >= 0 ? C_FG : C_DIM, buf);
  char m100[24];
  fmt_minutes(m100, sizeof m100, cs.minutes_to(100, d));
  snprintf(buf, sizeof buf, T("bis 100 %%: %s", "to 100 %%: %s"), m100);
  text(X3, Y_SUB, F_LABEL, C_DIM, buf);
  hline(Y_DIV);

  if (!cs.seen) {
    text(X1, Y_DIV + 80, F_LARGE, C_DIM, T("Keine Ladung seit dem Start", "No charging since start"));
    text(X1, Y_DIV + 116, F_LABEL, C_DIM, T("Die Werte erscheinen, sobald Strom in den Akku fließt.", "Values appear once current flows into the battery."));
    return;
  }
  char evse[32] = "";
  if (dc && v.has_charge && v.evse_max_kw > 0) snprintf(evse, sizeof evse, T(" · Säule bis %.0f kW", " · max %.0f kW"), v.evse_max_kw);
  snprintf(buf, sizeof buf, T("Ladekurve (kW) · %s%s", "Curve (kW) · %s%s"),
           cs.active ? (dc ? T("Lädt (DC)", "charging DC") : T("Lädt (AC)", "charging AC")) : T("beendet", "finished"), evse);
  text(X1, Y_DIV + 30, F_SMALL, cs.active ? C_GREEN : C_DIM, buf);
  charge_curve(cs, X1 + 36, Y_DIV + 46, 2 * COL - PAD - 36, H - PAD - 24 - (Y_DIV + 46));

  constexpr int FX2 = X3 + COL / 2, FY = Y_DIV + 30, FH = 70, FW = COL / 2 - 8;
  snprintf(buf, sizeof buf, "+%.1f kWh", cs.kwh());
  field(X3, FY, T("Geladen", "Charged"), de(buf), C_GREEN, FW);
  fmt_minutes(buf, sizeof buf, cs.minutes());
  field(FX2, FY, T("Dauer", "Duration"), buf, C_FG, FW);
  snprintf(buf, sizeof buf, "%.0f kW", cs.peak_kw);
  field(X3, FY + FH, T("Spitze", "Peak"), buf, C_FG, FW);
  snprintf(buf, sizeof buf, "%.0f-%.0f %%", cs.start_soc, cs.soc);
  field(FX2, FY + FH, T("Ladestand", "SoC"), buf, C_FG, FW);
  // Ladesteuerung (744) bzw. Bordlader (7E5): nur waehrend der Ladung, sonst Striche
  bool live = v.has_charge && cs.active;
  if (ac) {
    int obc = std::max(v.obc_c[0], v.obc_c[1]);
    if (v.has_obc) snprintf(buf, sizeof buf, "%d °C", obc);
    else snprintf(buf, sizeof buf, "-");
    field(X3, FY + 2 * FH, T("Bordlader", "On-board charger"), buf, v.has_obc && obc >= 80 ? C_ORANGE : C_FG, FW);
  } else {
    float req = live ? v.target_v * v.target_a / 1000 : 0;
    if (req > 0) snprintf(buf, sizeof buf, "%.0f kW", req);
    else snprintf(buf, sizeof buf, "-");
    field(X3, FY + 2 * FH, T("Angefordert", "Requested"), buf, C_FG, FW);
  }
  int hot = 0;
  if (live) {
    hot = dc ? std::max(v.dc_inlet_c[0], v.dc_inlet_c[1]) : v.ac_inlet_c;
    snprintf(buf, sizeof buf, "%d °C", hot);
  } else {
    snprintf(buf, sizeof buf, "-");
  }
  field(FX2, FY + 2 * FH, T("Ladebuchse", "Charge port"), buf, hot >= 70 ? C_RED : (hot >= 55 ? C_ORANGE : C_FG), FW);
}

// "Antrieb": Fahrgestell von oben (vorn links), jeder Wert an seinem Bauteil.
// Die Masse kommen aus der App (Entwurf 1713x636, Foto bei x 236..1476,
// y 20..616); cx/cy rechnen sie auf das hier 603x290 grosse Foto um.
constexpr int CH_X = (W - CHASSIS_W) / 2, CH_Y = 92;
constexpr float CK = (float)CHASSIS_H / 596;
int cx(float x) { return CH_X + (int)((x - 236) * CK + 0.5f); }
int cy(float y) { return CH_Y + (int)((y - 20) * CK + 0.5f); }
int cs_(float s) { return (int)(s * CK + 0.5f); }

// HV-Leitungen als Punktreihen, die Punkte wandern mit der Zeit (vorwaerts =
// zum Motor). Damit sie zwischen den Abfragen fluessig laufen, merkt sich die
// Seite, was sie zuletzt gezeichnet hat; animate() setzt nur die Punkte neu.
const float FLOW_FRONT[] = {532, 352, 470, 352, 440, 355, 420, 372, 400, 382, 332, 382, 322, 362};
const float FLOW_REAR[] = {1176, 300, 1184, 330, 1190, 410, 1200, 426, 1220, 428, 1270, 428, 1290, 420, 1292, 386};
constexpr int MAX_DOTS = 64;
struct FlowState {
  bool on = false, front = false, forward = false;
  uint16_t color = 0;
  int n = 0;
  int16_t x[MAX_DOTS], y[MAX_DOTS];  // zuletzt gezeichnete Punkte
} flow_state;

void flow_dots(const float* pts, int n) {
  constexpr float PERIOD = 26;
  FlowState& f = flow_state;
  float shift = (millis() % 1100) / 1100.0f * PERIOD;
  float acc = f.forward ? shift : PERIOD - shift;
  for (int i = 0; i + 1 < n; i++) {
    float ax = pts[2 * i], ay = pts[2 * i + 1], bx = pts[2 * i + 2], by = pts[2 * i + 3];
    float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    float s = acc;
    for (; s < len; s += PERIOD) {
      int x = cx(ax + (bx - ax) * s / len), y = cy(ay + (by - ay) * s / len);
      gfx->fillCircle(x, y, 2, f.color);
      if (f.n < MAX_DOTS) f.x[f.n] = x, f.y[f.n] = y, f.n++;
    }
    acc = s - len;
  }
}

void draw_flow() {
  flow_state.n = 0;
  if (!flow_state.on) return;
  if (flow_state.front) flow_dots(FLOW_FRONT, 7);
  flow_dots(FLOW_REAR, 8);
}

// Punkte entfernen: dort wieder das Foto (die Leitungen liegen nur auf dem Foto)
void erase_flow() {
  for (int i = 0; i < flow_state.n; i++) {
    for (int y = flow_state.y[i] - 2; y <= flow_state.y[i] + 2; y++) {
      for (int x = flow_state.x[i] - 2; x <= flow_state.x[i] + 2; x++) {
        int px = x - CH_X, py = y - CH_Y;
        bool in = px >= 0 && px < CHASSIS_W && py >= 0 && py < CHASSIS_H;
        gfx->drawPixel(x, y, in ? CHASSIS[py * CHASSIS_W + px] : C_BG);
      }
    }
  }
  flow_state.n = 0;
}

void page_drivetrain(const BmsData& d, const Consumption& c, const Tires& tires, const Vehicle& car) {
  char buf[64];
  // Canvas verdeckt die Fassung fuer Bilder im Flash; die der Basisklasse
  // zeichnet Punkt fuer Punkt und beachtet so auch die Drehung
  static_cast<Arduino_GFX*>(gfx)->draw16bitRGBBitmap(CH_X, CH_Y, CHASSIS, CHASSIS_W, CHASSIS_H);

  // HV-Leitungen: Punkte laufen vom Akku zu den Motoren, beim Rekuperieren/Laden zurueck
  float p = d.power_kw;
  flow_state.on = fabsf(p) >= 0.5f;
  flow_state.front = d.rpm_front != 0 || p < 0;
  flow_state.forward = p > 0;
  flow_state.color = p < 0 ? C_GREEN : C_FLOW;
  draw_flow();

  // Akku: 16 Module als Kacheln (Lage schematisch), waermstes umrandet
  gfx->fillRoundRect(cx(540), cy(138), cs_(616), cs_(356), 5, C_BG);
  int lo = d.modules[0], hi = d.modules[0];
  for (int i = 1; i < 16; i++) {
    lo = std::min(lo, (int)d.modules[i]);
    hi = std::max(hi, (int)d.modules[i]);
  }
  for (int i = 0; i < 16; i++) {
    float x = 596 + (i % 8) * 64, y = i < 8 ? 150 : 362;
    gfx->fillRoundRect(cx(x), cy(y), cs_(58), cs_(118), 4, temp_color(d.modules[i]));
    if (d.modules[i] == hi && hi != lo) gfx->drawRoundRect(cx(x) - 1, cy(y) - 1, cs_(58) + 2, cs_(118) + 2, 5, C_FG);
    snprintf(buf, sizeof buf, "%d°", d.modules[i]);
    text_center(cx(x + 29), cy(y + 80), F_TINYB, C_BG, buf);
  }
  gfx->fillRoundRect(cx(600), cy(280), cs_(508), cs_(74), 6, C_BG);
  gfx->drawRoundRect(cx(600), cy(280), cs_(508), cs_(74), 6, C_LINE);
  snprintf(buf, sizeof buf, "%.1f", d.soc_display);
  int vw = text_width(F_NUM, buf), uw = text_width(F_SMALL, " %");
  int vx = cx(706) - (vw + uw) / 2;
  text(vx, cy(338), F_NUM, C_FG, buf);
  text(vx + vw + 4, cy(338), F_SMALL, C_DIM, "%");
  snprintf(buf, sizeof buf, d.energy_kwh > 0 ? T("Akku · %.1f kWh", "Battery · %.1f kWh") : T("Akku · ~%.1f kWh", "Battery · ~%.1f kWh"), remaining_kwh(d));
  text(cx(810), cy(306), F_TINY, C_DIM, de(buf));
  float diff = (d.cell_v_max - d.cell_v_min) * 1000;
  snprintf(buf, sizeof buf, "%.2f-%.2f V · %.0f mV", d.cell_v_min, d.cell_v_max, diff);
  text(cx(810), cy(338), F_TINY, diff >= 100 ? C_ORANGE : C_DIM, de(buf));

  // Reifen in den Raedern: oben die rechte Seite (Ansicht von oben, vorn links)
  static const char* const NAMES_DE[4] = {"VL", "VR", "HL", "HR"};
  static const char* const NAMES_EN[4] = {"FL", "FR", "RL", "RR"};
  const char* const* NAMES = i18n::en() ? NAMES_EN : NAMES_DE;
  static const float SPOT[4][2] = {{284, 543}, {284, 38}, {1205, 543}, {1205, 38}};  // je Reifen 0..3
  for (int i = 0; i < 4; i++) {
    float x = SPOT[i][0], y = SPOT[i][1];
    float bar = tires.valid ? tires.bar[i] : 0;
    uint16_t col = bar <= 0 ? C_DIM : (bar < 2.2f ? C_RED : (bar < 2.4f || bar > 3.2f ? C_ORANGE : C_FG));
    int l = cx(x), t = cy(y), w = cs_(200), h = cs_(54);
    gfx->fillRoundRect(l, t, w, h, h / 2, C_BG);
    bool warn = col != C_FG && col != C_DIM;
    gfx->drawRoundRect(l, t, w, h, h / 2, warn ? col : C_LINE);
    if (warn) gfx->drawRoundRect(l + 1, t + 1, w - 2, h - 2, h / 2 - 1, col);
    text(cx(x + 18), cy(y + 35), F_TINYB, C_DIM, NAMES[i]);
    if (bar > 0) {
      snprintf(buf, sizeof buf, "%.1f", bar);
      text_center(cx(x + 100), cy(y + 38), F_MED, col, de(buf));
      snprintf(buf, sizeof buf, "%d°", tires.temp[i]);
      text_right(cx(x + 188), cy(y + 35), F_TINY, C_DIM, buf);
    } else {
      text_center(cx(x + 100), cy(y + 38), F_MED, C_DIM, "-");
    }
  }

  // Oben: Tempo links, Leistung in der Mitte, Reichweite rechts
  float speed = speed_kmh(d, car);
  text(PAD, 72, F_SMALL, C_DIM, T("Tempo", "Speed"));
  snprintf(buf, sizeof buf, "%.0f", speed);
  big_value(PAD, 106, buf, "km/h", C_FG, F_NUM, F_SMALL);
  text_center(W / 2, 70, F_SMALL, C_DIM, T("Leistung", "Power"));
  uint16_t pc = p < -0.5f ? C_GREEN : (p > 50 ? C_ORANGE : C_FG);
  snprintf(buf, sizeof buf, fabsf(p) >= 100 ? "%.0f" : "%.1f", p);
  vw = text_width(F_NUM, buf);
  uw = text_width(F_SMALL, "kW");
  vx = W / 2 - (vw + 6 + uw) / 2;
  text(vx, 106, F_NUM, pc, buf);
  text(vx + vw + 6, 106, F_SMALL, C_DIM, "kW");
  snprintf(buf, sizeof buf, "%.0f V   %.0f A", d.voltage, d.current);
  text(W / 2 + 70, 70, F_SMALL, C_DIM, buf);
  bool moving = d.rpm_rear != 0 || d.rpm_front != 0;
  const char* state = p < -0.5f && !moving ? T("Laden", "Charging") : (p < -0.5f ? T("Rekuperation", "Regen") : (moving ? T("Fahren", "Driving") : T("Stand", "Stopped")));
  text_right(W / 2 - 70, 70, F_SMALL, C_DIM, state);
  float range = c.range_km(d);
  text_right(W - PAD, 72, F_SMALL, C_DIM, T("Reichweite", "Range"));
  if (range >= 0) {
    snprintf(buf, sizeof buf, "%.0f", range);
    int rw = text_width(F_NUM, buf);
    text_right(W - PAD, 106, F_SMALL, C_DIM, "km");
    text(W - PAD - text_width(F_SMALL, "km") - 6 - rw, 106, F_NUM, C_FG, buf);
  } else {
    text_right(W - PAD, 104, F_SMALL, C_DIM, T("ab 3 km", "after 3 km"));
  }

  // Seiten: Motoren mit Strich zum Bauteil
  text(PAD, 222, F_SMALL, C_DIM, T("Motor vorn", "Motor front"));
  snprintf(buf, sizeof buf, "%d", d.rpm_front);
  text(PAD, 254, F_LARGE, C_FG, buf);
  text(PAD, 276, F_TINY, C_DIM, T("U/min", "rpm"));
  gfx->drawFastHLine(PAD + 76, 240, cx(300) - PAD - 76, C_DIM);
  gfx->fillCircle(cx(300), 240, 3, C_FG);
  text_right(W - PAD, 222, F_SMALL, C_DIM, T("Motor hinten", "Motor rear"));
  snprintf(buf, sizeof buf, "%d", d.rpm_rear);
  text_right(W - PAD, 254, F_LARGE, C_FG, buf);
  text_right(W - PAD, 276, F_TINY, C_DIM, T("U/min", "rpm"));
  gfx->drawFastHLine(cx(1400), 250, W - PAD - 86 - cx(1400), C_DIM);
  gfx->fillCircle(cx(1400), 250, 3, C_FG);

  // Unten: Waerme und Bordnetz
  hline(CH_Y + CHASSIS_H + 10);
  constexpr int YL = 420, YV = 456;
  if (car.has_aux) {  // Ladestand gross, Spannung klein daneben
    snprintf(buf, sizeof buf, "12 V  %.1f V", car.aux_v);
    text(PAD, YL, F_SMALL, C_DIM, de(buf));
    snprintf(buf, sizeof buf, "%.0f %%", car.aux_soc);
    text(PAD, YV, F_LARGE, aux_color(car), buf);
  } else {
    text(PAD, YL, F_SMALL, C_DIM, T("12-V-Batterie", "12 V battery"));
    snprintf(buf, sizeof buf, "%.1f V", d.aux_v);
    text(PAD, YV, F_LARGE, d.aux_v >= 5 && d.aux_v <= 12 ? C_ORANGE : C_FG, de(buf));
  }
  text(170, YL, F_SMALL, C_DIM, T("Kühlmittel", "Coolant"));
  if (d.has_coolant) snprintf(buf, sizeof buf, "%d °C", d.coolant_inlet);
  else snprintf(buf, sizeof buf, "-");
  text(170, YV, F_LARGE, C_FG, buf);
  uint16_t sc;
  const char* st = thermal_label(d, sc);
  text(300, YL, F_SMALL, C_DIM, T("Temperierung", "Thermal mgmt"));
  // Bis zur Modulspalte bei x 480 ist Platz fuer 165 px: "Pumpe laeuft" passt
  // gross nicht hinein (lief in die Modultemperatur, Marc 23.10.), dann kleiner
  const char* tl = d.has_coolant ? st : "-";
  text(300, YV, text_width(F_LARGE, tl) <= 165 ? F_LARGE : F_MED, sc, tl);
  text(480, YL, F_SMALL, C_DIM, T("Module", "Modules"));
  snprintf(buf, sizeof buf, "%d-%d °C", lo, hi);
  text(480, YV, F_LARGE, temp_color(hi), buf);
  text_right(W - PAD, YL, F_SMALL, C_DIM, T("rekuperiert", "regenerated"));
  snprintf(buf, sizeof buf, "%.2f kWh", c.regen_kwh);
  text_right(W - PAD, YV, F_LARGE, C_GREEN, de(buf));
}

void page_cells(const BmsData& d, const Cells& c, const Vehicle& v) {
  char buf[64];
  float lo = 9, hi = 0, sum = 0;
  int n = 0, lo_i = 0, hi_i = 0;
  for (int i = 0; i < CELLS; i++) {
    if (c.v[i] <= 0) continue;
    if (c.v[i] < lo) lo = c.v[i], lo_i = i;
    if (c.v[i] > hi) hi = c.v[i], hi_i = i;
    sum += c.v[i];
    n++;
  }
  constexpr int BX = PAD, BW = 96 * 5 + 2;  // 5 px je Zelle
  text_right(W - PAD, H - 12, F_TINY, C_DIM, T("hoch/runter wischen: Akku", "swipe up/down: Battery"));
  if (!n) {
    text(PAD, 100, F_MED, C_DIM, T("Zellspannungen werden gelesen ...", "Reading cell voltages ..."));
  } else {
    snprintf(buf, sizeof buf, T("%d Zellen, Ø %.3f V", "%d cells, avg %.3f V"), n, sum / n);
    text(PAD, 78, F_LABEL, C_DIM, de(buf));
    // zwei Reihen à 96 Zellen; Skala: 20 mV unter der niedrigsten bis zur hoechsten
    float base = lo - 0.02f, top = std::max(hi, lo + 0.02f);
    for (int row = 0; row < 2; row++) {
      int y = 92 + row * 150;
      gfx->drawRect(BX, y, BW, 120, C_LINE);
      for (int k = 0; k < 96; k++) {
        int i = row * 96 + k;
        if (c.v[i] <= 0) continue;
        int bh = (int)((c.v[i] - base) / (top - base) * 118);
        uint16_t col = i == lo_i ? C_RED : (i == hi_i ? C_ORANGE : C_BLUE);
        gfx->fillRect(BX + 2 + k * 5, y + 119 - bh, 4, bh, col);
      }
      snprintf(buf, sizeof buf, "%d", row * 96 + 1);
      text(BX + 2, y + 138, F_TINY, C_DIM, buf);
      snprintf(buf, sizeof buf, "%d", row * 96 + 96);
      text_right(BX + BW - 2, y + 138, F_TINY, C_DIM, buf);
    }
    constexpr int FY = 400;
    snprintf(buf, sizeof buf, "%.3f V  #%d", lo, lo_i + 1);
    field(PAD, FY, T("niedrigste", "lowest"), de(buf), C_RED, 160);
    snprintf(buf, sizeof buf, "%.3f V  #%d", hi, hi_i + 1);
    field(PAD + 175, FY, T("höchste", "highest"), de(buf), C_ORANGE, 160);
    snprintf(buf, sizeof buf, "%.0f mV", (hi - lo) * 1000);
    field(PAD + 350, FY, T("Differenz", "Difference"), buf, hi - lo > 0.05f ? C_YELLOW : C_FG);
  }

  // Lebenslauf des Akkus aus den BMS-Zaehlern, rechts
  constexpr int RX = BX + BW + 36;
  text(RX, 78, F_LABEL, C_DIM, T("Akku-Lebenslauf", "Battery history"));
  snprintf(buf, sizeof buf, "%.0f", d.cum_charged_kwh / CAPACITY_NEW_KWH);
  field(RX, 120, T("Vollzyklen", "Full cycles"), buf);
  snprintf(buf, sizeof buf, "%.0f h", d.operating_h);
  field(RX + 120, 120, T("Betrieb", "Operation"), buf);
  snprintf(buf, sizeof buf, "%.0f kWh", d.cum_charged_kwh);
  field(RX, 190, T("geladen", "charged"), buf);
  snprintf(buf, sizeof buf, "%.0f kWh", d.cum_discharged_kwh);
  field(RX, 260, T("entladen", "discharged"), buf, C_FG, 110);
  if (d.odometer_km > 0) {
    snprintf(buf, sizeof buf, "%.1f", d.cum_discharged_kwh / d.odometer_km * 100);
    field(RX + 120, 260, T("Ø kWh/100", "avg kWh/100"), de(buf));
  }
  if (v.has_stats) {  // Ladestatistik aus 220111: Anzahl und kWh je Ladeart
    snprintf(buf, sizeof buf, "%u× %u kWh", (unsigned)v.ac_count, (unsigned)v.ac_kwh);
    field(RX, 330, T("AC-Ladungen", "AC charges"), buf);
    snprintf(buf, sizeof buf, "%u× %u kWh", (unsigned)v.dc_count, (unsigned)v.dc_kwh);
    field(RX, 400, T("DC-Ladungen", "DC charges"), buf);
  } else {
    snprintf(buf, sizeof buf, "%.1f %%", d.soh);
    field(RX, 330, "SoH", de(buf));
  }
}

const char* const MONTH_DE[12] = {"Jan", "Feb", "Mär", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
const char* const MONTH_EN[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
#define MONTH (i18n::en() ? MONTH_EN : MONTH_DE)

// Fahrtenbuch: links die letzten Fahrten, rechts die Monate
void page_trips(const TripLog& log) {
  char buf[48];
  constexpr int L = PAD, R = W / 2 + 24;
  text(L, 80, F_LABEL, C_DIM, T("Letzte Fahrten", "Recent trips"));
  if (!log.n) {
    text(L, 120, F_MED, C_DIM, T("Noch keine Fahrten gespeichert", "No trips saved yet"));
    text(L, 148, F_SMALL, C_DIM, T("Eine Fahrt kommt am Fahrtende dazu", "A trip is added when a drive ends"));
  } else {
    text(L, 108, F_TINY, C_DIM, T("Beginn", "Start"));
    text_right(L + 190, 108, F_TINY, C_DIM, "km");
    text_right(L + 280, 108, F_TINY, C_DIM, "kWh/100");
    text_right(R - 48, 108, F_TINY, C_DIM, T("Akku %", "SoC %"));
    // neueste zuerst
    for (int r = 0; r < 12 && r < log.n; r++) {
      const Trip& t = log.trips[log.n - 1 - r];
      int y = 136 + r * 27;
      if (t.start) {
        time_t s = t.start;
        struct tm tm;
        localtime_r(&s, &tm);
        strftime(buf, sizeof buf, "%d.%m. %H:%M", &tm);
      } else {
        snprintf(buf, sizeof buf, "-");
      }
      text(L, y, F_SMALL, C_FG, buf);
      snprintf(buf, sizeof buf, "%.1f", t.km);
      text_right(L + 190, y, F_SMALL, C_FG, de(buf));
      float c = t.kwh_100km();
      snprintf(buf, sizeof buf, "%.1f", c);
      text_right(L + 280, y, F_SMALL, c > 25 ? C_ORANGE : C_FG, de(buf));
      snprintf(buf, sizeof buf, "%d-%d", t.soc_from, t.soc_to);
      text_right(R - 48, y, F_SMALL, C_FG, buf);
    }
  }
  gfx->drawFastVLine(R - 24, TOP + 20, H - TOP - 40, C_LINE);

  text(R, 80, F_LABEL, C_DIM, T("Monate", "Months"));
  if (!log.nm) {
    text(R, 120, F_SMALL, C_DIM, T("Braucht die Uhrzeit (Start im WLAN)", "Needs the time (start within Wi-Fi)"));
    return;
  }
  text(R, 108, F_TINY, C_DIM, T("Monat", "Month"));
  text_right(R + 140, 108, F_TINY, C_DIM, T("Fahrten", "Trips"));
  text_right(R + 210, 108, F_TINY, C_DIM, "km");
  text_right(R + 270, 108, F_TINY, C_DIM, "kWh");
  text_right(W - PAD, 108, F_TINY, C_DIM, T("pro 100", "per 100"));
  for (int r = 0; r < 12 && r < log.nm; r++) {
    const MonthStat& m = log.months[log.nm - 1 - r];
    int y = 136 + r * 27;
    snprintf(buf, sizeof buf, "%s %u", MONTH[(m.ym % 100 + 11) % 12], (unsigned)(m.ym / 100));
    text(R, y, F_SMALL, C_FG, buf);
    snprintf(buf, sizeof buf, "%u", (unsigned)m.trips);
    text_right(R + 140, y, F_SMALL, C_FG, buf);
    snprintf(buf, sizeof buf, "%.0f", m.km);
    text_right(R + 210, y, F_SMALL, C_FG, buf);
    snprintf(buf, sizeof buf, "%.0f", m.kwh);
    text_right(R + 270, y, F_SMALL, C_FG, buf);
    if (m.km > 0) snprintf(buf, sizeof buf, "%.1f", m.kwh / m.km * 100);
    else snprintf(buf, sizeof buf, "-");
    text_right(W - PAD, y, F_SMALL, C_FG, de(buf));
  }
}

// Alle Temperaturen auf einen Blick: Klima, Akku, Heckantrieb nebeneinander
void page_temps(const BmsData& d, const Vehicle& v) {
  char buf[32];
  auto deg = [&](bool ok, float t) {
    if (ok) snprintf(buf, sizeof buf, "%.1f °C", t);
    else snprintf(buf, sizeof buf, "-");
    return de(buf);
  };
  constexpr int C1 = PAD, C2 = PAD + COL, C3 = PAD + 2 * COL, Y0 = 84, RH = 84;
  text(C1, Y0, F_MED, C_FG, T("Klima", "Climate"));
  field(C1, Y0 + 44, T("Außen", "Outside"), deg(v.has_climate, v.outside_c), v.has_climate && v.outside_c < 3 ? C_BLUE : C_FG);
  field(C1, Y0 + 44 + RH, T("Innen", "Inside"), deg(v.has_climate, v.inside_c));

  text(C2, Y0, F_MED, C_FG, T("Akku", "Battery"));
  snprintf(buf, sizeof buf, "%d / %d °C", d.temp_min, d.temp_max);
  field(C2, Y0 + 44, "min / max", buf, temp_color(d.temp_max));
  snprintf(buf, sizeof buf, "%d °C", d.inlet_c);
  field(C2, Y0 + 44 + RH, T("Einlass", "Inlet"), buf);
  if (d.has_coolant) {
    snprintf(buf, sizeof buf, "%d °C", d.coolant_inlet);
    field(C2, Y0 + 44 + 2 * RH, T("Kühlmittel", "Coolant"), buf);
    uint16_t sc;
    const char* st = thermal_label(d, sc);
    field(C2, Y0 + 44 + 3 * RH, T("Temperierung", "Thermal mgmt"), st, sc);
  }

  text(C3, Y0, F_MED, C_FG, T("Antrieb hinten", "Rear drive"));
  field(C3, Y0 + 44, "Motor", deg(v.has_motor, v.motor_c),
        v.motor_c >= 140 ? C_RED : (v.motor_c >= 110 ? C_YELLOW : C_FG));
  field(C3, Y0 + 44 + RH, "Inverter", deg(v.has_motor, v.inverter_c),
        v.inverter_c >= 80 ? C_RED : (v.inverter_c >= 65 ? C_YELLOW : C_FG));
  field(C3, Y0 + 44 + 2 * RH, T("Kühlkörper", "Heat sink"), deg(v.has_motor, v.heatsink_c));
  text(C3, Y0 + 44 + 3 * RH, F_SMALL, C_DIM, T("Frontmotor: keine", "Front motor: no"));
  text(C3, Y0 + 44 + 3 * RH + 22, F_SMALL, C_DIM, T("Temperaturen bekannt", "temperatures known"));
}


// "Auto": Draufsicht (vorn links) in Biophilic Blue; offene Tueren klappen
// aus, offene Klappen werden orange, Standlicht als helle Punkte vorn,
// Abblend- und Fernlicht als Lichtkegel, Ruecklicht und Bremslicht als rote Leiste. Masse in Pixeln des 500x206-Bildes.
constexpr int CAR_X = 236, CAR_Y = 132;
uint16_t mix(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, int a, int n) {
  return gfx->color565(r0 + (r1 - r0) * a / n, g0 + (g1 - g0) * a / n, b0 + (b1 - b0) * a / n);
}

// Lack Biophilic Blue Pearl (XB9): dunkles Blau, zur Wagenmitte (Zeile y) hin
// heller wie der Perlglanz; je kraeftiger die Linie (k), desto dunkler
uint16_t car_color(int y, int k, bool open) {
  int a = std::min(180, k);
  if (open) return mix(240, 140, 40, 60, 30, 8, a, 180);
  float t = 1 - fabsf(y - CAR_H / 2.0f) / (CAR_H / 2.0f);  // 0 Rand .. 1 Mitte
  t = t * t * (3 - 2 * t);
  return mix(12 + (int)(40 * t), 24 + (int)(60 * t), 46 + (int)(84 * t), 6, 10, 22, a, 180);
}

constexpr float DOOR_DEG = 38;  // Oeffnungswinkel der Tueren

// Flaeche einer Klappe (je Zeile x-Bereich aus car_top_img.h)
bool in_flap(const int16_t (*f)[2], int x, int y) { return x >= f[y][0] && x <= f[y][1]; }

void page_car(const Vehicle& v) {
  // Karosserie; offene Klappen orange, wo eine offene Tuer sass, sieht man
  // in den dunklen Innenraum
  const int16_t (*doors[4])[2] = {CAR_DOOR_FL, CAR_DOOR_FR, CAR_DOOR_RL, CAR_DOOR_RR};
  for (int y = 0; y < CAR_H; y++) {
    for (int x = 0; x < CAR_W; x++) {
      uint8_t k = CAR_TOP[y * CAR_W + x];
      if (!k) continue;
      bool hi = (v.hood && in_flap(CAR_HOOD, x, y)) || (v.trunk && in_flap(CAR_TRUNK, x, y));
      uint16_t c = car_color(y, k, hi);
      for (int i = 0; i < 4; i++)
        if (v.door[i] && in_flap(doors[i], x, y)) c = mix(34, 30, 28, 4, 4, 6, std::min(180, (int)k), 180);
      gfx->drawPixel(CAR_X + x, CAR_Y + y, c);
    }
  }
  // Offene Tueren: das Tuerblatt (Blech und Scheibe) um das vordere Scharnier
  // nach aussen gedreht und orange
  for (int i = 0; i < 4; i++) {
    if (!v.door[i]) continue;
    const int16_t (*f)[2] = doors[i];
    bool left = i % 2 == 0;  // Fahrerseite = unten im Bild
    int fx0 = CAR_W, fx1 = 0, fy0 = -1, fy1 = -1;
    for (int y = 0; y < CAR_H; y++) {
      if (f[y][1] < f[y][0]) continue;
      fx0 = std::min(fx0, (int)f[y][0]);
      fx1 = std::max(fx1, (int)f[y][1]);
      if (fy0 < 0) fy0 = y;
      fy1 = y;
    }
    if (fy0 < 0) continue;
    float hx = fx0, hy = left ? fy1 : fy0;  // Scharnier: vorn an der Aussenkante
    float a = (left ? 1 : -1) * DOOR_DEG * 3.14159265f / 180, ca = cosf(a), sa = sinf(a);
    int len = fx1 - fx0, dep = fy1 - fy0;
    for (int py = (int)hy - len - dep; py <= (int)hy + len + dep; py++) {
      for (int px = fx0 - dep; px <= fx1; px++) {
        float dx = px - hx, dy = py - hy;  // zurueckdrehen: woher in der Zeichnung?
        int sx = (int)lroundf(hx + dx * ca + dy * sa), sy = (int)lroundf(hy - dx * sa + dy * ca);
        if (sy < 0 || sy >= CAR_H || !in_flap(f, sx, sy)) continue;
        int k = CAR_TOP[sy * CAR_W + sx];
        if (!k) continue;
        gfx->drawPixel(CAR_X + px, CAR_Y + py, car_color(sy, k / 3, true));  // Linien weicher: ein Stueck
      }
    }
  }
  // Licht an (BC03): Kegel nach vorn (links), zum Ende hin schwaecher und
  // breiter. Stand- und Abblendlicht lassen sich am Ioniq 6 noch nicht
  // unterscheiden (FW 90 versuchte es mit den Kona-Bits, die passen nicht).
  bool low = v.has_switch ? v.beam_on : v.lights_on;  // FW 96: Schalter-Byte, sonst wie bisher
  if (low) {
    constexpr int REACH = 170;
    for (int side = 0; side < 2; side++) {
      int sy = CAR_Y + (side ? 176 : 30), dir = side ? 1 : -1;
      for (int i = 0; i < REACH; i++) {
        int h = 4 + i * 44 / REACH, mid = sy + dir * i / 7;  // leicht nach aussen
        gfx->drawFastVLine(CAR_X + 8 - i, mid - h / 2, h, mix(8, 20, 24, 255, 236, 150, REACH - i, REACH + 30));
      }
    }
  }
  if (low) {
    for (int side = 0; side < 2; side++) {
      int sy = CAR_Y + (side ? 176 : 30);
      for (int k = -1; k <= 1; k++) gfx->fillRect(CAR_X + 6, sy + k * 6 - 2, 4, 4, gfx->color565(250, 250, 255));
    }
  }
  // Rueckleuchten: Band an der Hinterkante entlang ihrer Rundung, an den
  // Ecken als "Parametric Pixels"; Bremslicht heller mit Schein nach hinten
  if (low || v.brake_light) {
    constexpr int Y0 = 16, N = CAR_H - 2 * Y0;
    static int16_t edge[N];
    static bool have_edge = false;
    if (!have_edge) {  // Hinterkante je Zeile, geglaettet
      int16_t raw[N];
      for (int i = 0; i < N; i++) {
        int y = Y0 + i, xe = CAR_W - 1;
        while (xe > CAR_W - 80 && CAR_TOP[y * CAR_W + xe] < 40) xe--;
        raw[i] = xe;
      }
      // oben und unten gemittelt, damit das Band spiegelgleich ist
      for (int i = 0; i < N; i++) {
        int sum = 0, n = 0;
        for (int j = std::max(0, i - 4); j <= std::min(N - 1, i + 4); j++) sum += raw[j] + raw[N - 1 - j], n += 2;
        edge[i] = sum / n;
      }
      have_edge = true;
    }
    bool brake = v.brake_light;
    uint8_t lr = brake ? 255 : 210, lg = brake ? 60 : 30;
    int glow = brake ? 16 : 7;
    for (int i = 0; i < N; i++) {
      int y = CAR_Y + Y0 + i, xe = CAR_X + edge[i];
      for (int g = 1; g <= glow; g++)
        gfx->drawPixel(xe + g, y, mix(8, 20, 24, lr, lg, lg, glow + 1 - g, glow * 2));
    }
    for (int i = 0; i < N; i++) {
      int y = CAR_Y + Y0 + i, xe = CAR_X + edge[i];
      int j = std::min(i, N - 1 - i);  // Abstand zum naechsten Ende
      if (j < 30) {
        if (j % 6 == 0) gfx->fillRect(xe - 4, i < N / 2 ? y : y - 3, 4, 4, gfx->color565(lr, lg, lg));
        continue;
      }
      gfx->drawFastHLine(xe - (brake ? 5 : 4), y, brake ? 5 : 4, gfx->color565(lr, lg, lg));
      gfx->drawPixel(xe - 2, y, gfx->color565(255, brake ? 170 : 110, brake ? 150 : 90));  // heller Kern
    }
  }

  // Zusammenfassung unten
  char buf[96];
  if (!v.has_body) {
    text_center(W / 2, 440, F_MED, C_DIM, T("Keine Daten vom Karosserie-Steuergerät", "No data from body control unit"));
    return;
  }
  const char* names_de[4] = {"Fahrertür", "Beifahrertür", "hinten links", "hinten rechts"};
  const char* names_en[4] = {"driver door", "passenger door", "rear left", "rear right"};
  std::string open;
  for (int i = 0; i < 4; i++)
    if (v.door[i]) open += std::string(open.empty() ? "" : ", ") + (i18n::en() ? names_en[i] : names_de[i]);
  if (v.trunk) open += std::string(open.empty() ? "" : ", ") + T("Kofferraum", "trunk");
  if (v.hood) open += std::string(open.empty() ? "" : ", ") + T("Frunk", "frunk");
  if (open.empty()) snprintf(buf, sizeof buf, "%s", T("Alles zu", "All closed"));
  else snprintf(buf, sizeof buf, "%s %s", T("Offen:", "Open:"), open.c_str());
  text(PAD, 438, F_MED, open.empty() ? C_GREEN : C_ORANGE, buf);
  int open_w = text_width(F_MED, buf);
  const char* light = low ? T("Licht an", "Lights on") : T("Licht aus", "Lights off");
  snprintf(buf, sizeof buf, "%s%s", light, v.brake_light ? T(", Bremse", ", brake") : "");
  // passt es nicht neben die offenen Tueren, eine Zeile hoeher
  int ly = PAD + open_w + 24 + text_width(F_MED, buf) > W - PAD ? 404 : 438;
  text_right(W - PAD, ly, F_MED, low ? C_YELLOW : C_DIM, buf);
  // Rohdaten zum Bestaetigen der Bits am Auto (Tuer oeffnen, Licht schalten)
  snprintf(buf, sizeof buf, "770 BC03: %02X %02X %02X %02X   BC10: %d", v.body_raw[0], v.body_raw[1], v.body_raw[2],
           v.body_raw[3], v.light_code);
  text(PAD, 468, F_TINY, C_DIM, buf);
  if (v.has_beam) snprintf(buf, sizeof buf, "BC09: %02X %02X %02X %02X", v.beam_raw[0], v.beam_raw[1], v.beam_raw[2], v.beam_raw[3]);
  else snprintf(buf, sizeof buf, "BC09: --");
  std::string s = buf;
  if (v.has_switch)
    snprintf(buf, sizeof buf, "   BC05: %02X %02X %02X %02X", v.switch_raw[0], v.switch_raw[1], v.switch_raw[2], v.switch_raw[3]);
  else snprintf(buf, sizeof buf, "   BC05: --");
  s += buf;
  text_right(W - PAD, 468, F_TINY, C_DIM, s.c_str());
}


// "G": Punkt im Kreis so, wie man die Kraft im Sitz spuert (Marc 28.10.):
// oben = bremsen, unten = beschleunigen, Linkskurve nach rechts und
// umgekehrt; dahinter die Spur der letzten Werte;
// rechts die Zahlen und die Spitzen der Fahrt
constexpr int GM_X = 236, GM_Y = 266, GM_R = 178;
constexpr float GM_MAX = 0.45f;  // Rand des Kreises; mehr bleibt am Rand stehen

uint16_t g_color(float g) { return g < 0.2f ? C_GREEN : (g < 0.3f ? C_YELLOW : C_ORANGE); }

void page_g(const Vehicle& v) {
  char buf[48];
  for (int i = 4; i >= 1; i--) {  // Ringe 0,1 ... 0,4 g
    int r = (int)(GM_R * i * 0.1f / GM_MAX);
    gfx->fillCircle(GM_X, GM_Y, r, i % 2 ? C_PANEL : C_BG);
    gfx->drawCircle(GM_X, GM_Y, r, C_LINE);
  }
  gfx->drawCircle(GM_X, GM_Y, GM_R, C_DIM);
  gfx->drawFastHLine(GM_X - GM_R, GM_Y, 2 * GM_R, C_LINE);
  gfx->drawFastVLine(GM_X, GM_Y - GM_R, 2 * GM_R, C_LINE);
  for (int i = 1; i <= 4; i++) {
    snprintf(buf, sizeof buf, "0.%d", i);
    text(GM_X + 4, GM_Y - (int)(GM_R * i * 0.1f / GM_MAX) + 14, F_TINY, C_DIM, de(buf));
  }
  text_center(GM_X, GM_Y - GM_R - 8, F_TINY, C_DIM, T("Bremsen", "Brake"));
  text_center(GM_X, GM_Y + GM_R + 20, F_TINY, C_DIM, T("Beschleunigen", "Accelerate"));
  text(GM_X - GM_R + 8, GM_Y - 8, F_TINY, C_DIM, T("Rechtskurve", "Right turn"));
  text_right(GM_X + GM_R - 8, GM_Y - 8, F_TINY, C_DIM, T("Linkskurve", "Left turn"));
  auto gx = [](float lat) { return GM_X + (int)(std::max(-GM_MAX, std::min(GM_MAX, lat)) / GM_MAX * GM_R); };
  auto gy = [](float lon) { return GM_Y + (int)(std::max(-GM_MAX, std::min(GM_MAX, lon)) / GM_MAX * GM_R); };
  // Spur: aeltere Punkte kleiner und dunkler
  for (int i = 0; i + 1 < v.trail_n; i++) {
    int a = 1 + i * 4 / v.trail_n;
    gfx->fillCircle(gx(v.trail_lat[i]), gy(v.trail_lon[i]), 1 + a, mix(8, 20, 24, 76, 147, 222, a, 5));
  }
  float g = sqrtf(v.lat_g * v.lat_g + v.lon_g * v.lon_g);
  if (v.has_dyn) {
    gfx->fillCircle(gx(v.lat_g), gy(v.lon_g), 11, g_color(g));
    gfx->drawCircle(gx(v.lat_g), gy(v.lon_g), 12, C_FG);
  }

  // Zahlen rechts
  constexpr int GX1 = 480, GX2 = 640, GFW = 150;
  if (!v.has_dyn) {
    text(GX1, 110, F_MED, C_DIM, T("Keine Daten vom ESC (730)", "No data from ESC (730)"));
    return;
  }
  text(GX1, 84, F_LABEL, C_DIM, T("Jetzt", "Now"));
  snprintf(buf, sizeof buf, "%.2f", g);
  big_value(GX1, 150, de(buf), "g", g_color(g));
  snprintf(buf, sizeof buf, T("längs %+.2f   quer %+.2f", "long %+.2f   lat %+.2f"), v.lon_g, v.lat_g);
  text(GX1, 180, F_SMALL, C_DIM, de(buf));
  gfx->drawFastHLine(GX1, 200, W - PAD - GX1, C_LINE);
  text(GX1, 234, F_LABEL, C_DIM, T("Spitzen dieser Fahrt", "Peaks this drive"));
  snprintf(buf, sizeof buf, "%.2f g", v.peak_acc);
  field(GX1, 270, T("Beschleunigen", "Accelerate"), de(buf), g_color(v.peak_acc), GFW);
  snprintf(buf, sizeof buf, "%.2f g", v.peak_brake);
  field(GX2, 270, T("Bremsen", "Brake"), de(buf), g_color(v.peak_brake), GFW);
  snprintf(buf, sizeof buf, "%.2f g", v.peak_left);
  field(GX1, 350, T("Kurve links", "Left turn"), de(buf), g_color(v.peak_left), GFW);
  snprintf(buf, sizeof buf, "%.2f g", v.peak_right);
  field(GX2, 350, T("Kurve rechts", "Right turn"), de(buf), g_color(v.peak_right), GFW);
  snprintf(buf, sizeof buf, T("Lenkrad %.0f°   Räder %.0f/%.0f/%.0f/%.0f km/h", "Wheel %.0f°   wheels %.0f/%.0f/%.0f/%.0f km/h"),
           v.steer_deg, v.wheel_kmh[0], v.wheel_kmh[1], v.wheel_kmh[2], v.wheel_kmh[3]);
  text(GX1, 460, F_TINY, C_DIM, buf);
  text_right(W - PAD, 84, F_TINY, C_DIM, T("hoch/runter wischen: Antrieb", "swipe up/down: Motors"));
}

}  // namespace

constexpr int LIST_Y0 = 58, LIST_ROW = 47, LIST_ROWS = 8;  // Menue hat bis zu 8 Eintraege

int list_first(int sel, int count) { return std::max(0, std::min(sel - LIST_ROWS / 2, count - LIST_ROWS)); }

int list_hit(int y, int sel, int count) {
  if (y < LIST_Y0) return -1;
  int r = (y - LIST_Y0) / LIST_ROW;
  if (r >= LIST_ROWS) return -1;
  int i = list_first(sel, count) + r;
  return i < count ? i : -1;
}

// Statusbildschirm: unten links "Menü", darueber "Schlafen"
constexpr int BTN_X = 24, BTN_W = 360;
void menu_button() {
  // Schlafen links, Ausschalten rechts daneben (halbe Breite)
  constexpr int HW = (BTN_W - 12) / 2;
  const char* label[2] = {T("Schlafen", "Sleep"), T("Ausschalten", "Power off")};
  for (int i = 0; i < 2; i++) {
    int x = BTN_X + i * (HW + 12);
    gfx->fillRoundRect(x, H - 132, HW, 52, 10, C_PANEL);
    gfx->drawRoundRect(x, H - 132, HW, 52, 10, C_DIM);
    text_center(x + HW / 2, H - 98, F_MED, C_FG, label[i]);
  }
  gfx->fillRoundRect(BTN_X, H - 72, BTN_W, 56, 10, C_LINE);
  gfx->drawRoundRect(BTN_X, H - 72, BTN_W, 56, 10, C_FG);
  text_center(BTN_X + BTN_W / 2, H - 35, F_LARGE, C_FG, T("Menü", "Menu"));
  show();
}

void touch_debug(int x, int y, int raw_x, int raw_y) {
  char buf[64];
  snprintf(buf, sizeof buf, T("Tipp: x %d  y %d  (Chip %d / %d)", "Tap: x %d  y %d  (chip %d / %d)"), x, y, raw_x, raw_y);
  gfx->fillRect(0, H - 166, BTN_X + BTN_W + 20, 26, C_BG);
  text(BTN_X, H - 146, F_TINY, C_DIM, buf);
  show();
}

// Touch-Test: Fadenkreuz an x/y, Schritt n von 4
void touch_target(int n, int x, int y, const char* hint) {
  shown_page = -1;
  gfx->fillScreen(C_BG);
  char buf[48];
  snprintf(buf, sizeof buf, "Touch-Test  %d / 4", n + 1);
  text_center(W / 2, H / 2 - 30, F_LARGE, C_FG, buf);
  text_center(W / 2, H / 2 + 2, F_LABEL, C_FG, T("Tippe genau auf das gelbe Kreuz", "Tap exactly on the yellow cross"));
  if (hint && *hint) text_center(W / 2, H / 2 + 36, F_SMALL, C_ORANGE, hint);
  gfx->drawFastHLine(x - 22, y, 45, C_YELLOW);
  gfx->drawFastVLine(x, y - 22, 45, C_YELLOW);
  gfx->drawCircle(x, y, 12, C_YELLOW);
  gfx->drawCircle(x, y, 13, C_YELLOW);
  show();
}

// Pruefbild: Finger malt Punkte, oben die Rohdaten
constexpr int TT_TOP = 120;  // darueber: Schaltflaeche "Nochmal" und Werte
void touch_check(bool clear, const uint8_t* d, bool touching, int x, int y, int rx, int ry, const char* cal) {
  if (clear) {
    shown_page = -1;
    gfx->fillScreen(C_BG);
    gfx->fillRoundRect(12, 8, W - 24, 44, 10, C_LINE);
    gfx->drawRoundRect(12, 8, W - 24, 44, 10, C_FG);
    text_center(W / 2, 37, F_MED, C_FG, T("Nochmal kalibrieren", "Calibrate again"));
    gfx->fillRoundRect(12, H - 66, W - 24, 52, 10, C_GREEN);
    text_center(W / 2, H - 31, F_LARGE, C_BG, T("Fertig", "Done"));
    text(12, H - 80, F_TINY, C_DIM, T("Malen zum Prüfen", "Draw to check"));
    // Raster zum Zielen
    for (int gx = 40; gx < W; gx += 80) gfx->drawFastVLine(gx, TT_TOP, H - 100 - TT_TOP, C_LINE);
    for (int gy = TT_TOP + 40; gy < H - 100; gy += 80) gfx->drawFastHLine(0, gy, W, C_LINE);
  }
  gfx->fillRect(0, 56, W, TT_TOP - 58, C_BG);
  char buf[80];
  text(12, 74, F_TINY, C_DIM, cal);
  snprintf(buf, sizeof buf, "%02X %02X %02X %02X %02X %02X %02X %02X", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
  text(12, 94, F_TINY, C_DIM, buf);
  if (touching) {
    snprintf(buf, sizeof buf, "x %d  y %d   Chip %d / %d", x, y, rx, ry);
    text(12, 114, F_TINY, C_FG, buf);
    if (y >= TT_TOP && y < H - 100) gfx->fillCircle(x, y, 4, C_YELLOW);
  }
  show();
}

bool menu_hit(int x, int y, bool dashboard) {
  if (x < 0) return false;  // BOOT-Taste (gibt es auf dem 4.3" nicht)
  if (dashboard) return x >= MENU_X && y < TOP + 10;  // oben rechts: Menue-Zeichen
  return y >= H - 76 && x < BTN_X + BTN_W + 20;
}

int sleep_hit(int x, int y) {
  if (x < 0 || x >= BTN_X + BTN_W + 20 || y < H - 136 || y >= H - 76) return 0;
  return x < BTN_X + BTN_W / 2 ? 1 : 2;
}

int tab_hit(int x, int y) {
  if (x < 0 || y >= TOP + 10) return -1;
  for (int i = 0; i < PAGES; i++)
    if (x >= tab_x0[i] - 12 && x < tab_x1[i] + 12) return i;
  return -1;
}

void choice_list(const char* title, const std::vector<ListItem>& items, int sel, const char* footer) {
  shown_page = -1;
  gfx->fillScreen(C_BG);
  text(24, 40, F_LARGE, C_FG, title);
  constexpr int Y0 = LIST_Y0, ROW = LIST_ROW, ROWS = LIST_ROWS;
  int first = list_first(sel, (int)items.size());
  for (int i = first; i < (int)items.size() && i < first + ROWS; i++) {
    int y = Y0 + (i - first) * ROW;
    if (i == sel) {
      gfx->fillRoundRect(12, y, W - 24, ROW - 6, 8, C_LINE);
      gfx->drawRoundRect(12, y, W - 24, ROW - 6, 8, C_FG);
    } else if (sel < 0) {  // Menue: jeder Eintrag als Schaltflaeche
      gfx->fillRoundRect(12, y, W - 24, ROW - 6, 8, C_PANEL);
      gfx->drawRoundRect(12, y, W - 24, ROW - 6, 8, C_DIM);
    }
    // Titel links, Erklaerung rechts daneben (quer ist genug Platz)
    text(28, y + 28, F_MED, items[i].accent ? C_GREEN : C_FG, items[i].title.c_str());
    text(270, y + 27, F_SMALL, C_DIM, items[i].sub.c_str());
  }
  // Bildlauf-Hinweis
  if (first > 0) gfx->fillTriangle(W - 30, Y0 - 12, W - 38, Y0 - 2, W - 22, Y0 - 2, C_DIM);
  if (first + ROWS < (int)items.size()) {
    int y = Y0 + ROWS * ROW;
    gfx->fillTriangle(W - 30, y + 6, W - 38, y - 4, W - 22, y - 4, C_DIM);
  }
  gfx->drawFastHLine(0, H - 40, W, C_LINE);
  text(24, H - 14, F_SMALL, C_DIM, footer);
  show();
}

void set_demo(bool on) { demo = on; }

void animate() {
  static uint32_t last = 0;
  if (shown_page != PAGE_DRIVETRAIN || !flow_state.on || millis() - last < 60) return;
  last = millis();
  erase_flow();
  draw_flow();
  show();
}

// Rote Warnung oben rechts statt der Seitenleiste, solange etwas nicht
// stimmt; mehrere wechseln alle 3 s (wie in der App)
bool warning_header(const BmsData& d, const Tires& t) {
  char w[MAX_WARN][40];
  int n = warnings(d, t, w);
  if (!n) return false;
  int i = (millis() / 3000) % n;
  char label[56];
  if (n > 1) snprintf(label, sizeof label, "%s  (%d/%d)", w[i], i + 1, n);
  else snprintf(label, sizeof label, "%s", w[i]);
  int tw = text_width(F_MED, label);
  int right = MENU_X - 12;
  gfx->fillRoundRect(right - tw - 28, 6, tw + 28, 34, 8, C_RED);
  text(right - tw - 14, 30, F_MED, C_FG, label);
  return true;
}

void trips_view(const TripLog& log) {
  shown_page = -1;
  gfx->fillScreen(C_BG);
  text(24, 32, F_MED, C_FG, T("Fahrten", "Trips"));
  text_right(W - PAD, 32, F_SMALL, C_DIM, T("Tippen schließt", "Tap to close"));
  page_trips(log);
  show();
}

void dashboard(const BmsData& d, const Consumption& c, const ChargeSession& cs, const Tires& t,
               const Cells& cells, const Vehicle& v, const TripLog& log, bool link_ok, int page) {
  shown_page = page;
  gfx->fillScreen(C_BG);
  if (page == PAGE_DRIVE) page_drive(d, c, v);
  else if (page == PAGE_BATTERY) page_battery(d, v);
  else if (page == PAGE_CHARGE) page_charge(d, cs, v);
  else if (page == PAGE_DRIVETRAIN) page_drivetrain(d, c, t, v);
  else if (page == PAGE_CELLS) page_cells(d, cells, v);
  else if (page == PAGE_TRIPS) page_trips(log);
  else if (page == PAGE_CAR) page_car(v);
  else if (page == PAGE_G) page_g(v);
  else page_temps(d, v);
  // Kopfzeile zuletzt, damit nichts von der Seite darueber liegt
  gfx->fillRect(0, 0, W, TOP, C_BG);
  if (!link_ok || !warning_header(d, t)) header(link_ok, page);
  else {
    for (int i = 0; i < PAGES; i++) tab_x0[i] = tab_x1[i] = -100;  // Warnung verdeckt die Seiten
    if (demo) text(16, 30, F_TINYB, C_YELLOW, "DEMO");
    else text(16, 30, F_SMALL, C_GREEN, "OBD");
    menu_icon();
  }
  gfx->drawFastHLine(0, TOP, W, C_LINE);
  show();
}

}  // namespace ui
