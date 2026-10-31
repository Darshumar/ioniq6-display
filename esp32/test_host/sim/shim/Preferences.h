#pragma once
class Preferences { public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  bool getBool(const char*, bool d = false) { return d; }
  int getInt(const char*, int d = 0) { return d; }
  size_t putInt(const char*, int) { return 4; }
};
