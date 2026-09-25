#pragma once
// WiFi falso para os testes nativos. Cobre a interseção do WiFi do ESP32
// (upstream) com o WiFiNINA (port): begin/status/SSID/RSSI/localIP/disconnect.
// mode() e WIFI_STA só existem no ESP32 e ficam para o código antigo compilar.
#include "Arduino.h"

constexpr int WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4,
              WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6, WL_NO_MODULE = 255;
constexpr int WIFI_STA = 1;
struct FakeIP {
  String toString() const { return "192.0.2.1"; }
};
struct FakeWiFi {
  int statusValue = 0, attempts = 0;
  String ssid;
  String SSID() const { return ssid; }
  int status() const { return statusValue; }
  // ESP32: disconnect(wifioff, eraseap). WiFiNINA: disconnect(), devolve int.
  int disconnect(bool = false, bool = false) {
    statusValue = 0;
    return statusValue;
  }
  void mode(int) {}
  int begin(const char *name, const char *) {
    ssid = name;
    ++attempts;
    return statusValue;
  }
  int32_t RSSI() const { return -40; }
  FakeIP localIP() const { return {}; }
  uint32_t getTime() const { return 0; } // WiFiNINA: epoch do NTP, 0 = ainda não
};
inline FakeWiFi WiFi;
