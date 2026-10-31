#include "elm_ble.h"

#include <Preferences.h>

#include <algorithm>

namespace {

// Bekannte Charakteristik des Vgate iCar 2 (notify + write)
const NimBLEUUID VGATE_CHAR("bef8d6c9-9c21-4c9e-b632-bd58c1009f9f");
const char* NAME_HINTS[] = {"vlink", "v-link", "icar", "vgate", "obd", "elm"};

// Notify-Callback laeuft im NimBLE-Task, nicht im Interrupt: Mutex reicht
SemaphoreHandle_t buf_mux = nullptr;  // wird in connect() angelegt
std::string rx_buf;
volatile bool prompt = false;

bool looks_like_obd(std::string name) {
  for (auto& c : name) c = tolower(c);
  for (auto h : NAME_HINTS) {
    if (name.find(h) != std::string::npos) return true;
  }
  return false;
}

class Finder : public NimBLEAdvertisedDeviceCallbacks {
 public:
  std::string wanted;  // gespeicherte Adresse, leer = nach Namen suchen
  NimBLEAddress found;
  std::string found_name;
  bool hit = false;
  void onResult(NimBLEAdvertisedDevice* dev) override {
    bool match = wanted.empty() ? looks_like_obd(dev->getName()) : dev->getAddress().toString() == wanted;
    if (match && !hit) {
      found = dev->getAddress();
      found_name = dev->getName();
      hit = true;
      NimBLEDevice::getScan()->stop();
    }
  }
};

// Bluetooth auf Kern 0 starten: der Controller legt seine Interrupts auf den
// Kern, der ihn startet. Auf Kern 1 laufen die Display-Interrupts (Bounce-
// Buffer, Neustart im VSYNC); kommen die zu spaet, wackelt das ganze Bild
// (Marc 23.10., nur bei Bluetooth-Verbindung; weniger Sendeleistung in FW 60
// und kein Flash-Zugriff laut diag-Log, es liegt also nicht am Funk selbst).
void init_task(void* caller) {
  NimBLEDevice::init("");
  xTaskNotifyGive((TaskHandle_t)caller);
  vTaskDelete(nullptr);
}

void ensure_init() {
  static bool init = false;
  if (init) return;
  buf_mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(init_task, "ble_init", 8192, xTaskGetCurrentTaskHandle(), 5, nullptr, 0);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  init = true;
}

// Notify- und Schreib-Charakteristik suchen: im selben Dienst, bekannte
// Adapter-Dienste bevorzugt (Vgate, FFF0 mit FFF1/FFF2, FFE0 mit FFE1)
bool pick_chars(NimBLEClient* client, NimBLERemoteCharacteristic*& nc, NimBLERemoteCharacteristic*& wc) {
  static const uint16_t SKIP[] = {0x1800, 0x1801, 0x180A, 0x180F};  // GAP, GATT, Geraeteinfo, Batterie
  int best = 0;
  nc = wc = nullptr;
  for (auto* svc : *client->getServices(true)) {
    bool skip = false;
    for (uint16_t u : SKIP) skip |= svc->getUUID() == NimBLEUUID(u);
    if (skip) continue;
    NimBLERemoteCharacteristic *n = nullptr, *w = nullptr;
    int score = 1;
    for (auto* ch : *svc->getCharacteristics(true)) {
      if (ch->getUUID() == VGATE_CHAR) {
        n = w = ch;
        score = 3;
        break;
      }
      if (!n && (ch->canNotify() || ch->canIndicate())) n = ch;
      if (!w && (ch->canWrite() || ch->canWriteNoResponse())) w = ch;
    }
    if (svc->getUUID() == NimBLEUUID((uint16_t)0xFFF0) || svc->getUUID() == NimBLEUUID((uint16_t)0xFFE0)) score = 2;
    if (n && w && score > best) {
      nc = n;
      wc = w;
      best = score;
    }
  }
  return best > 0;
}

bool scan_for(Finder& f, uint32_t ms) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&f, false);
  scan->setActiveScan(true);
  scan->start((ms + 999) / 1000, false);  // blockiert, bis gefunden oder Zeit um
  scan->clearResults();
  return f.hit;
}

}  // namespace

void ElmBle::on_notify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  xSemaphoreTake(buf_mux, portMAX_DELAY);
  rx_buf.append((const char*)data, len);
  if (memchr(data, '>', len)) prompt = true;
  xSemaphoreGive(buf_mux);
}

std::string ElmBle::saved_address() {
  Preferences p;
  p.begin("ioniq", true);
  std::string a = p.getString("adapter", "").c_str();
  p.end();
  return a;
}

std::string ElmBle::saved_name() {
  Preferences p;
  p.begin("ioniq", true);
  std::string a = p.getString("adapter_name", "").c_str();
  p.end();
  return a;
}

void ElmBle::remember(const BleDevice& d) {
  Preferences p;
  p.begin("ioniq", false);
  p.putString("adapter", d.address.c_str());
  p.putString("adapter_name", d.name.c_str());
  p.end();
}

void ElmBle::forget() {
  Preferences p;
  p.begin("ioniq", false);
  p.remove("adapter");
  p.remove("adapter_name");
  p.end();
}

std::vector<BleDevice> ElmBle::scan_all(uint32_t ms) {
  ensure_init();
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(nullptr, false);
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->start((ms + 999) / 1000, false);
  std::vector<BleDevice> out;
  for (int i = 0; i < res.getCount(); i++) {
    NimBLEAdvertisedDevice d = res.getDevice(i);
    BleDevice b;
    b.name = d.getName();
    b.address = d.getAddress().toString();
    b.rssi = d.getRSSI();
    b.obd = looks_like_obd(b.name);
    out.push_back(b);
  }
  scan->clearResults();
  std::sort(out.begin(), out.end(), [](const BleDevice& a, const BleDevice& b) {
    if (a.obd != b.obd) return a.obd;
    if (a.name.empty() != b.name.empty()) return !a.name.empty();  // ohne Namen ans Ende
    return a.rssi > b.rssi;
  });
  return out;
}

bool ElmBle::connect(uint32_t scan_ms) {
  ensure_init();
  // Ein gemerkter Adapter wird nur selbst gesucht (der Nutzer hat ihn gewaehlt),
  // ohne einen nach dem Namen
  Finder f;
  f.wanted = saved_address();
  bool fresh = f.wanted.empty();
  if (!scan_for(f, scan_ms)) return false;

  if (!client_) client_ = NimBLEDevice::createClient();
  client_->setConnectTimeout(10);
  if (!client_->connect(f.found)) return false;
  address_ = f.found.toString();

  // CCCD mit Bestaetigung schreiben (Write Request): NimBLE schickt sonst ein
  // Write Command, das viele Adapter ignorieren -> es kommen nie Antworten.
  if (!pick_chars(client_, notify_, write_) || !notify_->subscribe(notify_->canNotify(), on_notify, true)) {
    client_->disconnect();
    return false;
  }
  if (fresh) {  // per Name gefunden: fuer das naechste Mal merken
    BleDevice d;
    d.address = address_;
    d.name = f.found_name;
    remember(d);
  }
  return true;
}

bool ElmBle::connected() { return client_ && client_->isConnected(); }

void ElmBle::disconnect() {
  if (connected()) client_->disconnect();
}

void (*ElmBle::idle)() = nullptr;

std::string ElmBle::send(const char* cmd, uint32_t timeout_ms) {
  if (!connected()) return "";
  xSemaphoreTake(buf_mux, portMAX_DELAY);
  rx_buf.clear();
  prompt = false;
  xSemaphoreGive(buf_mux);

  std::string line = std::string(cmd) + "\r";
  write_->writeValue((const uint8_t*)line.data(), line.size(), write_->canWrite());

  uint32_t t0 = millis();
  while (!prompt && millis() - t0 < timeout_ms) {
    if (idle) idle();
    delay(5);
  }

  xSemaphoreTake(buf_mux, portMAX_DELAY);
  std::string text = rx_buf;
  xSemaphoreGive(buf_mux);
  if (!prompt) return "";
  // ">" und Echo-Reste entfernen
  size_t p = text.find('>');
  if (p != std::string::npos) text.erase(p);
  return text;
}
