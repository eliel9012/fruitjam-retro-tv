#pragma once
// Stub de include/fj/Net.h para os testes nativos: sem SPI, sem ESP32-C6.
// A assinatura segue o header de verdade; o comportamento é o mínimo para
// código de rede compilar e ser exercitado com o WiFi falso.
#include "Arduino.h"
#include "WiFiNINA.h"

namespace net {

inline int fakeLockDepth = 0; // para teste conferir que o lock foi tomado e devolvido

inline bool beginRadio() { return true; }
inline bool radioReady() { return true; }
inline String firmwareVersion() { return "stub"; }

struct Lock {
  Lock() { ++fakeLockDepth; }
  ~Lock() { --fakeLockDepth; }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

struct HttpResult {
  int status = 0;
  size_t length = 0;
  bool truncated = false;
};

inline HttpResult httpGet(const char *, char *buf, size_t cap, uint32_t = 10000, const char * = nullptr) {
  if (buf && cap)
    buf[0] = 0;
  HttpResult r;
  r.status = -1; // sem rede no host
  return r;
}

inline uint32_t ntpEpoch() { return 0; }

struct TryLock {
  explicit TryLock(uint32_t = 0) { ++fakeLockDepth; }
  ~TryLock() { --fakeLockDepth; }
  explicit operator bool() const { return true; }
};

inline bool startStation(const char *ssid, const char *password) {
  WiFi.begin(ssid, password);
  return true;
}

} // namespace net
