#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "SecretsManager.h"

enum class NetworkState : uint8_t { OFFLINE, CONNECTING, CONNECTED, FAILED };
class NetworkManager {
public:
  void begin(const SecretsConfig &config);
  void update();
  void disconnect();
  void reconnectNow();
  void setConfiguration(const SecretsConfig &config);
  NetworkState state() const { return _state; }
  bool connected() const { return _state == NetworkState::CONNECTED; }
  uint8_t failures() const { return _failures; }
  String ssid() const { return _config.ssid; }
  String ip() const { return WiFi.localIP().toString(); }
  int rssi() const { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

private:
  SecretsConfig _config;
  NetworkState _state = NetworkState::OFFLINE;
  uint8_t _failures = 0;
  uint32_t _attemptStarted = 0, _retryAt = 0;
  bool _attemptActive = false, _enabled = false;
  void startAttempt();
};
