#include "NetworkManager.h"
#include "UiLogic.h"
#include "fj/Net.h"
#include <WiFiNINA.h>

// Tempo que uma tentativa tem para chegar a WL_CONNECTED antes de contar como
// falha. Associação WPA2 + DHCP no ESP32-C6 leva 2 a 5 s.
static const uint32_t ATTEMPT_MS = 10000;
// O RSSI muda devagar e cada leitura é uma transação SPI.
static const uint32_t RSSI_PERIOD_MS = 2000;

void NetworkManager::begin(const SecretsConfig &config) {
  _config = config;
  _failures = 0;
  _attemptActive = false;
  _enabled = config.valid();
  _retryAt = millis();
  forget();
  _state = _enabled ? NetworkState::CONNECTING : NetworkState::OFFLINE;
}
void NetworkManager::setConfiguration(const SecretsConfig &config) {
  disconnect();
  begin(config);
}
void NetworkManager::forget() {
  _ip = String();
  _rssi = 0;
}
// Com o ESP32-C6 ausente, qualquer chamada da WiFiNINA espera para sempre.
void NetworkManager::dropRadio() {
  if (!net::radioReady())
    return;
  net::Lock l;
  WiFi.disconnect();
}
void NetworkManager::disconnect() {
  dropRadio();
  _attemptActive = false;
  _enabled = false;
  forget();
  _state = NetworkState::OFFLINE;
}
void NetworkManager::reconnectNow() {
  dropRadio();
  begin(_config);
}
void NetworkManager::startAttempt() {
  // net::startStation só manda o comando: o WiFi.begin() da biblioteca ficaria
  // em delay(5000) até 50 s dentro do loop().
  net::startStation(_config.ssid.c_str(), _config.password.c_str());
  _attemptStarted = millis();
  _attemptActive = true;
  _state = NetworkState::CONNECTING;
}
void NetworkManager::update() {
  if (!_enabled || !_config.valid())
    return;
  if (!net::radioReady()) {
    _state = NetworkState::FAILED;
    return;
  }
  // Rádio ocupado por uma tarefa de rede = rádio funcionando. Tenta na próxima.
  net::TryLock lock;
  if (!lock)
    return;
  const uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && _state != NetworkState::CONNECTED) {
    // Chegou agora. Pode ser a rede que o portal testou, não a configurada.
    if (String(WiFi.SSID()) != _config.ssid) {
      WiFi.disconnect();
      _attemptActive = false;
      _retryAt = now;
      up = false;
    } else {
      _ip = WiFi.localIP().toString();
      _rssi = WiFi.RSSI();
      _rssiAt = now;
    }
  }
  if (up) {
    _state = NetworkState::CONNECTED;
    _failures = 0;
    _attemptActive = false;
    _retryAt = now;
    if (now - _rssiAt >= RSSI_PERIOD_MS) {
      _rssi = WiFi.RSSI();
      _rssiAt = now;
    }
    return;
  }
  if (_state == NetworkState::CONNECTED)
    forget(); // caiu; o _retryAt de quando estava conectado dispara a volta já
  if (_attemptActive && now - _attemptStarted >= ATTEMPT_MS) {
    WiFi.disconnect();
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
