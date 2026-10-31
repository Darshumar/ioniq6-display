#include "trips.h"

#include <Preferences.h>
#include <time.h>

namespace {
constexpr uint32_t MAGIC = 0x7121F001;  // aendert sich, wenn sich TripLog aendert
}

namespace trips {

void load(TripLog& log) {
  Preferences p;
  p.begin("trips", true);
  uint32_t magic = p.getUInt("magic", 0);
  if (magic == MAGIC && p.getBytesLength("log") == sizeof(TripLog)) p.getBytes("log", &log, sizeof(TripLog));
  p.end();
}

void record(TripLog& log, const Trip& t) {
  if (t.km < 1) return;
  uint32_t ym = 0;
  if (t.start > 1700000000) {
    time_t s = t.start;
    struct tm tm;
    localtime_r(&s, &tm);
    ym = (tm.tm_year + 1900) * 100 + tm.tm_mon + 1;
  }
  log.add(t, ym);
  Preferences p;
  p.begin("trips", false);
  p.putUInt("magic", MAGIC);
  p.putBytes("log", &log, sizeof(TripLog));
  p.end();
}

}  // namespace trips
