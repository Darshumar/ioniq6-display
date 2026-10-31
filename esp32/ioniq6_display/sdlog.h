// Logbuch auf der SD-Karte: eine CSV-Datei pro Start in /logs, mit denselben
// Spaltennamen wie obd_reader.py am PC plus Rohdaten.
#pragma once
#include <string>
#include <vector>

#include "board.h"
#include "decode.h"

namespace sdlog {

// Bindet die Karte ein; die Datei entsteht mit der ersten Zeile. Liefert eine Meldung fuer die
// Anzeige ("SD-Karte: 29,7 GB frei" oder warum es nicht geht).
// new_log = false: nur einbinden (Einstellungsseite), keine neue Datei
std::string begin(bool new_log = true);
bool active();  // es wird gerade geloggt (oder sobald Daten kommen)
void stop();    // nichts loggen (Demo-Modus)
// Eine Zeile pro Abfrage; raw = Antworten 220101, 220105, 220106, 22C00B als Hex
void write(const BmsData& d, const Consumption& c, const Tires& t, const char* thermal,
           const std::vector<std::string>& raw);
// Schliesst die laufende Datei (Fahrtende)
void finish();
#if LOG_UPLOAD
// Abgeschlossene Logs, die noch nicht auf GitHub liegen (nur mit Upload)
std::vector<std::string> pending();
void mark_uploaded(const std::string& name);
#endif
// Uhr erst nach der Fahrt gestellt (WLAN nach der Fahrt): start_*.csv dieses Starts
// bekommt nachtraeglich Datum und Uhrzeit des Fahrtbeginns als Namen
void date_untimed();
// Eine Zeile ins Diagnose-Log dieser Fahrt (diag_*.csv), Datei wechselt mit finish()
void diag(const char* line);
// Fuer die Einstellungsseite
std::vector<std::pair<std::string, size_t>> list();  // Dateien in /logs mit Groesse
bool open_read(const std::string& name, void* file_out);  // fs::File*

}  // namespace sdlog
