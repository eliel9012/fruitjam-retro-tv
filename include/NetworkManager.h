#pragma once
// Conexão Wi-Fi como estação, com retentativa e backoff (AGENTS.md 7, item 4).
//
// No Fruit Jam o Wi-Fi é o ESP32-C6 com firmware NINA, atrás do SPI1 (ver
// fj/Net.h). Duas consequências para quem usa esta classe:
//
// - update() nunca espera o rádio: se uma tarefa de rede está no meio de um GET
//   segurando o net::Lock, esta volta é pulada. Por isso ip() e rssi() devolvem
//   valores guardados na última volta, e não perguntam ao rádio na hora — o
//   desenho da tela não pode travar atrás de um GET de 10 s.
// - disconnect() e reconnectNow() tomam o lock de verdade: são ações do usuário,
//   raras, e precisam acontecer.
#include <Arduino.h>
#include <WiFiNINA.h>
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
  // Últimos valores lidos pelo update(); vazios/zero fora do CONNECTED.
  String ip() const { return _ip; }
  int rssi() const { return _rssi; }

private:
  SecretsConfig _config;
  NetworkState _state = NetworkState::OFFLINE;
  uint8_t _failures = 0;
  uint32_t _attemptStarted = 0, _retryAt = 0, _rssiAt = 0;
  bool _attemptActive = false, _enabled = false;
  String _ip;
  int _rssi = 0;
  void startAttempt();
  void dropRadio();
  void forget();
};
