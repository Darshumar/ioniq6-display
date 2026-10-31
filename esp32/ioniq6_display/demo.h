// Versteckter Demo-Modus: simuliert Fahrt und Schnellladung, damit sich die
// Anzeige ohne Auto vorfuehren laesst. Ohne Arduino-Abhaengigkeiten, damit
// der PC-Renderer ihn auch zeigen kann.
#pragma once
#include "decode.h"

struct Demo {
  BmsData d;
  Consumption cons;
  ChargeSession charge;
  Tires tires;
  Cells cells;
  Vehicle veh;
  TripLog trips;  // erfundenes Fahrtenbuch

  Demo();
  // Ein Schritt von dt Sekunden Echtzeit; charging = Lade-Seite ist offen,
  // dann steht das Auto an der Schnellladesaeule, sonst faehrt es
  void step(float dt, bool charging);

 private:
  double vt = 0;        // simulierte Zeit, laeuft schneller als die echte
  double odo = 52746;   // km-Stand mit Nachkommastellen
  float soc = 68.0f, v = 0, tire_warm = 0;
  bool was_charging = false;
  void sim(float dt_v, bool charging);  // dt_v in simulierter Zeit
};
