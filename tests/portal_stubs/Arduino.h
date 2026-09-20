#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sstream>
#include <iomanip>
constexpr int HEX = 16;
class String : public std::string {
public:
  using std::string::string;
  String() : std::string() {}
  String(const std::string &s) : std::string(s) {}
  String(int x) : std::string(std::to_string(x)) {}
  String(unsigned long x) : std::string(std::to_string(x)) {}
  String(uint32_t x, int base) : std::string() {
    std::ostringstream o;
    if (base == 16)
      o << std::hex;
    o << x;
    assign(o.str());
  }
  String(double x, int decimals) : std::string() {
    std::ostringstream o;
    o << std::fixed << std::setprecision(decimals) << x;
    assign(o.str());
  }
  bool startsWith(const char *s) const { return rfind(s, 0) == 0; }
  int indexOf(char c) const {
    auto n = find(c);
    return n == npos ? -1 : static_cast<int>(n);
  }
  double toDouble() const { return atof(c_str()); }
  int toInt() const { return atoi(c_str()); }
  void replace(const String &from, const String &to) {
    size_t p = 0;
    while ((p = find(from, p)) != npos) {
      std::string::replace(p, from.size(), to);
      p += to.size();
    }
  }
};
extern uint32_t fakeMillis;
inline uint32_t millis() {
  return fakeMillis;
}
inline uint32_t esp_random() {
  static uint32_t n = 1;
  return ++n;
}
struct FakeESP {
  uint64_t getEfuseMac() const { return 12345; }
};
extern FakeESP ESP;
