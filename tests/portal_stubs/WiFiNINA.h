#pragma once
// WiFiNINA falsa para o teste nativo do portal. Cada WiFiClient compartilha um
// estado com quem o criou: o teste escreve a requisição em `in` e lê a
// resposta em `out`.
#include "Arduino.h"
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>
constexpr int WL_CONNECTED = 3, ENC_TYPE_NONE = 7;
struct IPAddress {
  uint8_t b[4] = {0, 0, 0, 0};
  IPAddress() = default;
  IPAddress(int x0, int x1, int x2, int x3) : b{(uint8_t)x0, (uint8_t)x1, (uint8_t)x2, (uint8_t)x3} {}
  uint8_t operator[](int i) const { return b[i]; }
  String toString() const {
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
           std::to_string(b[3]);
  }
};
struct FakeConnection {
  std::string in, out;
  size_t pos = 0;
  bool peerOpen = true, stopped = false;
};
class WiFiClient {
public:
  std::shared_ptr<FakeConnection> conn;
  WiFiClient() = default;
  explicit WiFiClient(std::shared_ptr<FakeConnection> c) : conn(std::move(c)) {}
  explicit operator bool() const { return conn != nullptr; }
  int available() const { return conn ? int(conn->in.size() - conn->pos) : 0; }
  int read(uint8_t *dst, size_t n) {
    const size_t k = std::min(n, conn->in.size() - conn->pos);
    std::copy_n(conn->in.data() + conn->pos, k, dst);
    conn->pos += k;
    return int(k);
  }
  bool connected() const { return conn && (available() > 0 || conn->peerOpen); }
  size_t write(const uint8_t *p, size_t n) {
    conn->out.append(reinterpret_cast<const char *>(p), n);
    return n;
  }
  void stop() {
    if (conn)
      conn->stopped = true;
  }
};
struct FakeWiFi {
  int statusValue = 0, attempts = 0;
  String ssid;
  void disconnect() { statusValue = 0; }
  void begin(const char *name, const char *) {
    ssid = name;
    ++attempts;
  }
  String SSID() const { return ssid; }
  String SSID(uint8_t i) const { return i == 0 ? "Casa <5G>" : "Aberta"; }
  int RSSI(uint8_t) const { return -40; }
  uint8_t encryptionType(uint8_t i) const { return i == 0 ? 4 : 0; }
  int status() const { return statusValue; }
  uint8_t *macAddress(uint8_t *mac) const {
    const uint8_t m[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    std::copy_n(m, 6, mac);
    return mac;
  }
};
extern FakeWiFi WiFi;
