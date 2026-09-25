#pragma once
// Substituto de include/fj/Net.h para os testes nativos: sem SPI, sem FreeRTOS.
// O NetworkManager só precisa do lock e do início de conexão, que aqui viram o
// WiFi falso de tests/stubs/WiFi.h.
#include "WiFi.h"
namespace net {
struct Lock {
  Lock() {}
  ~Lock() {} // não trivial: sem isso o -Wunused-variable acusa `net::Lock l;`
};
struct TryLock {
  explicit TryLock(uint32_t = 0) {}
  explicit operator bool() const { return true; }
};
inline bool radioReady() {
  return true;
}
inline bool startStation(const char *ssid, const char *password) {
  WiFi.begin(ssid, password);
  return true;
}
} // namespace net
