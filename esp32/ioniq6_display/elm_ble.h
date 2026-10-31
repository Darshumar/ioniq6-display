// ELM327 ueber BLE mit NimBLE. Getestet mit dem Vgate iCar 2 ("IOS-Vlink"),
// andere BLE-Adapter werden ueber ihre Charakteristiken erkannt.
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>
#include <vector>

// Ein Geraet aus der Bluetooth-Suche fuer die Adapterauswahl
struct BleDevice {
  std::string name, address;
  int rssi = 0;
  bool obd = false;  // Name sieht nach OBD-Adapter aus
};

class ElmBle {
 public:
  // Sucht den Adapter (gespeicherte Adresse, sonst Name wie "vlink") und verbindet.
  bool connect(uint32_t scan_ms = 10000);
  // Alle Bluetooth-Geraete in Reichweite, OBD-artige zuerst, dann nach Signal
  std::vector<BleDevice> scan_all(uint32_t ms);
  // Gemerkter Adapter (Preferences ioniq/adapter, adapter_name)
  static std::string saved_address();
  static std::string saved_name();
  static void remember(const BleDevice& d);
  static void forget();
  bool connected();
  void disconnect();
  // Schickt einen Befehl und wartet auf den Prompt ">". Leer bei Timeout.
  std::string send(const char* cmd, uint32_t timeout_ms = 2000);
  // Wird beim Warten auf die Antwort alle paar ms aufgerufen (z. B. Animation)
  static void (*idle)();
  std::string address() const { return address_; }

 private:
  static void on_notify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool);
  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* notify_ = nullptr;
  NimBLERemoteCharacteristic* write_ = nullptr;
  std::string address_;
};
