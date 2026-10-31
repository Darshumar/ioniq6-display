// Update ueber das Internet: Beim Start verbindet sich der ESP mit dem
// hinterlegten WLAN (z. B. zu Hause in der Garage), fragt auf GitHub die
// Versionsnummer in esp32/firmware/version.txt ab und spielt bei einer neueren
// Version esp32/firmware/ioniq6_display_app.bin ein.
#pragma once
#include <string>

namespace ota {

bool configured();    // WLAN-Zugang hinterlegt?
void check_at_boot(); // sucht das WLAN kurz; ohne WLAN geht es sofort weiter
// Im Heim-WLAN die Uhr stellen (Log und Fahrt nachdatieren). Mit LOG_UPLOAD
// (board.h) laedt es auch abgeschlossene Logs von der SD-Karte in den
// Log-Ordner im Repo (braucht einen Schluessel mit Schreibrecht); sonst
// bleiben die Logs nur auf der Karte. Liefert eine Meldung fuer die Anzeige;
// schaltet das WLAN danach wieder ab.
std::string wifi_sync();
bool sync_due();  // Uhr fehlt oder (mit Upload) Logs warten
// Eigenes WLAN "Ioniq6-Display" mit Einstellungsseite (WLAN, GitHub-Schluessel,
// Update jetzt suchen). Endet mit einem Neustart, wenn man aufs Display tippt.
[[noreturn]] void settings_portal();

}  // namespace ota
