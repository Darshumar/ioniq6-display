// Fahrtenbuch im Flash des ESP (Preferences), ueberlebt Schlaf und Updates.
#pragma once
#include "decode.h"

namespace trips {

void load(TripLog& log);
// Fahrt am Fahrtende eintragen (ab 1 km) und speichern
void record(TripLog& log, const Trip& t);

}  // namespace trips
