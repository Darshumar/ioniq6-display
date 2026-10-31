// Sprache der Anzeige und des Portals: Deutsch oder Englisch (Marc 24.10.).
// Texte stehen direkt im Code als T("deutsch", "english"); fuer eine weitere
// Sprache bekommt T einen dritten Text und Lang einen Eintrag.
#pragma once
#include <stdint.h>

namespace i18n {

enum Lang : uint8_t { DE = 0, EN = 1 };
inline Lang lang = DE;  // inline: decode.cpp laeuft so auch in den PC-Tests ohne i18n.cpp
void load();          // aus den Einstellungen (ui/lang), beim Start
void set(Lang l);     // umschalten und merken
inline bool en() { return lang == EN; }

}  // namespace i18n

inline const char* T(const char* de, const char* en) { return i18n::lang == i18n::EN ? en : de; }
