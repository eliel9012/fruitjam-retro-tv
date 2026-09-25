#pragma once
// Substituto de include/fj/Net.h para o teste nativo do portal: o servidor HTTP
// vira uma fila de conexões falsas que o teste enche.
#include "WiFiNINA.h"
namespace net {
struct Lock {
  Lock() {}
  ~Lock() {} // não trivial: sem isso o -Wunused-variable acusa `net::Lock l;`
};
struct TryLock {
  explicit TryLock(uint32_t = 0) {}
  ~TryLock() {}
  explicit operator bool() const { return true; }
};
struct Fake {
  static inline bool radio = true, apOk = true, listenOk = true;
  static inline int apStarts = 0, scanResult = 2;
  static inline String apSsid, apPassword;
  static inline std::deque<WiFiClient> pending;
  // Enfileira uma conexão com a requisição já escrita e devolve o estado dela.
  static std::shared_ptr<FakeConnection> connect(const std::string &request) {
    auto c = std::make_shared<FakeConnection>();
    c->in = request;
    pending.push_back(WiFiClient(c));
    return c;
  }
};
inline bool radioReady() {
  return Fake::radio;
}
inline bool startStation(const char *ssid, const char *password) {
  WiFi.begin(ssid, password);
  return true;
}
inline bool startAccessPoint(const char *ssid, const char *password, uint8_t = 1) {
  ++Fake::apStarts;
  Fake::apSsid = ssid;
  Fake::apPassword = password;
  return Fake::apOk;
}
inline IPAddress accessPointIp() {
  return IPAddress(192, 168, 4, 1);
}
inline bool listen(uint16_t) {
  return Fake::listenOk;
}
inline WiFiClient accept(uint16_t) {
  if (Fake::pending.empty())
    return WiFiClient();
  WiFiClient c = Fake::pending.front();
  Fake::pending.pop_front();
  return c;
}
inline size_t writeAll(WiFiClient &c, const void *data, size_t n) {
  return c.write(static_cast<const uint8_t *>(data), n);
}
inline int scanNetworks() {
  return Fake::scanResult;
}
inline bool isOpenNetwork(uint8_t t) {
  return t == ENC_TYPE_NONE || t == 0;
}
} // namespace net
