#pragma once
#include "Arduino.h"
constexpr int WL_CONNECTED = 3, WIFI_STA = 1, WIFI_AP_STA = 3, WIFI_AUTH_OPEN = 0;
struct IPAddress {
  IPAddress(int, int, int, int) {}
};
struct FakeWiFi {
  int statusValue = 0, attempts = 0;
  String ssid;
  void disconnect(bool, bool) { statusValue = 0; }
  void mode(int) {}
  void begin(const char *name, const char *) {
    ssid = name;
    ++attempts;
  }
  bool softAPConfig(IPAddress, IPAddress, IPAddress) { return true; }
  bool softAP(const char *, const char *) { return true; }
  void softAPdisconnect(bool) {}
  int scanNetworks(bool, bool) { return -1; }
  int scanComplete() { return 0; }
  void scanDelete() {}
  String SSID(int = 0) const { return ssid; }
  int RSSI(int) const { return -40; }
  int encryptionType(int) const { return 1; }
  int status() const { return statusValue; }
  int softAPgetStationNum() const { return 0; }
};
extern FakeWiFi WiFi;
