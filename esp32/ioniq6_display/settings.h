// Sicherung der Einstellungen auf der SD-Karte (/einstellungen.txt).
// Gespeichert wird weiter im NVS des ESP; die Datei ist die Kopie, die nach
// einem Kabel-Flashen (loescht das NVS) automatisch zurueckgeladen wird.
#pragma once

namespace settings {

// NVS -> Karte (nur wenn sich etwas geaendert hat); ohne Karte ohne Wirkung
void backup();
// Karte -> NVS, aber nur wenn das NVS leer ist (frisch geflasht).
// true = etwas zurueckgeladen, dann neu starten, damit alles es liest.
bool restore();

}  // namespace settings
