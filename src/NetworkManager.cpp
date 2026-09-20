#include "NetworkManager.h"
#include "UiLogic.h"
void NetworkManager::begin(const SecretsConfig &config) {
  _config = config;
  _failures = 0;
  _attemptActive = false;
  _enabled = config.valid();
  _retryAt = millis();
  _state = _enabled ? NetworkState::CONNECTING : NetworkState::OFFLINE;
}
void NetworkManager::setConfiguration(const SecretsConfig &config) {
  disconnect();
  begin(config);
}
void NetworkManager::disconnect() {
  WiFi.disconnect(false, false);
  _attemptActive = false;
  _enabled = false;
  _state = NetworkState::OFFLINE;
}
void NetworkManager::reconnectNow() {
  WiFi.disconnect(false, false);
  begin(_config);
}
void NetworkManager::startAttempt() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(_config.ssid.c_str(), _config.password.c_str());
  _attemptStarted = millis();
  _attemptActive = true;
  _state = NetworkState::CONNECTING;
}
void NetworkManager::update() {
  if (!_enabled || !_config.valid())
    return;
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() != _config.ssid) {
    WiFi.disconnect(false, false);
    _attemptActive = false;
    _retryAt = millis();
  }
  if (WiFi.status() == WL_CONNECTED) {
    _state = NetworkState::CONNECTED;
    _failures = 0;
    _attemptActive = false;
    _retryAt = millis();
    return;
  }
  const uint32_t now = millis();
  if (_attemptActive && now - _attemptStarted >= 10000) {
    WiFi.disconnect(false, false);
    _attemptActive = false;
    const uint8_t exponent = _failures < 3 ? _failures : 3;
    if (_failures < 255)
      ++_failures;
    _retryAt = now + (5000UL << exponent);
    _state = NetworkState::FAILED;
  }
  if (!_attemptActive && timeReached(now, _retryAt))
    startAttempt();
}
