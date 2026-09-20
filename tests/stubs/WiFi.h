#pragma once
#include "Arduino.h"
constexpr int WL_CONNECTED = 3, WIFI_STA = 1;
struct FakeIP {
  String toString() const { return "192.0.2.1"; }
};
struct FakeWiFi {
  int statusValue = 0, attempts = 0;
  String ssid;
  String SSID() const { return ssid; }
  int status() const { return statusValue; }
  void disconnect(bool, bool) { statusValue = 0; }
  void mode(int) {}
  void begin(const char *name, const char *) {
    ssid = name;
    ++attempts;
  }
  int RSSI() const { return -40; }
  FakeIP localIP() const { return {}; }
};
extern FakeWiFi WiFi;
