#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
using std::min; using std::max;
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#define pgm_read_word(a) (*(const uint16_t*)(a))
#define pgm_read_dword(a) (*(const uint32_t*)(a))
#define pgm_read_pointer(a) (*(void* const*)(a))
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
typedef bool boolean;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
inline void yield() {}
inline void vTaskDelay(int) {}
#include <cstdlib>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
#define UNUSED(x) (void)(x)
#include "Print.h"
class __FlashStringHelper;
#include <string>
class String {
  std::string s;
 public:
  String(const char* c = "") : s(c) {}
  unsigned int length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
};
typedef int gpio_num_t;
inline void gpio_hold_dis(gpio_num_t) {}
inline void gpio_hold_en(gpio_num_t) {}
inline void gpio_deep_sleep_hold_en() {}
inline double ledcSetup(uint8_t, double f, uint8_t) { return f; }
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcDetachPin(uint8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}
