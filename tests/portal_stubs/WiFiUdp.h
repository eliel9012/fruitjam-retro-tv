#pragma once
// WiFiUDP falsa: o teste enfileira consultas DNS e confere as respostas.
#include "WiFiNINA.h"
struct WiFiUDP {
  static inline std::deque<std::vector<uint8_t>> incoming;
  static inline std::vector<std::vector<uint8_t>> sent;
  static inline bool beginOk = true;
  std::vector<uint8_t> current, pending;
  size_t pos = 0;
  bool open = false;
  uint8_t begin(uint16_t) { return open = beginOk; }
  void stop() { open = false; }
  int parsePacket() {
    if (!open || incoming.empty())
      return 0;
    current = incoming.front();
    incoming.pop_front();
    pos = 0;
    return int(current.size());
  }
  int read(uint8_t *dst, size_t n) {
    const size_t k = std::min(n, current.size() - pos);
    std::copy_n(current.data() + pos, k, dst);
    pos += k;
    return int(k);
  }
  IPAddress remoteIP() const { return IPAddress(192, 168, 4, 2); }
  uint16_t remotePort() const { return 5353; }
  int beginPacket(IPAddress, uint16_t) {
    pending.clear();
    return 1;
  }
  size_t write(const uint8_t *p, size_t n) {
    pending.insert(pending.end(), p, p + n);
    return n;
  }
  int endPacket() {
    sent.push_back(pending);
    return 1;
  }
};
