#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "SecretsManager.h"

using PortalSavedCallback = void (*)(const SecretsConfig &, const RadarConfig &);

enum class PortalStatus : uint8_t {
  PARADO,
  AGUARDANDO,
  CELULAR_CONECTADO,
  CONFIGURANDO,
  TESTANDO,
  SALVO,
  FALHA
};

class ConfigurationPortal {
public:
  void begin(SecretsManager &storage, const SecretsConfig &secrets, const RadarConfig &settings,
             PortalSavedCallback callback);
  void update();
  void stop();
  bool active() const { return _active; }
  PortalStatus status() const { return _status; }
  const String &apSsid() const { return _apSsid; }
  const String &apPassword() const { return _apPassword; }
  bool timedOutWarning() const;
  void extendSession();

private:
  WebServer _server{80};
  DNSServer _dns;
  SecretsManager *_storage = nullptr;
  SecretsConfig _savedSecrets;
  RadarConfig _savedSettings;
  PortalSavedCallback _callback = nullptr;
  String _apSsid, _apPassword, _formToken;
  bool _routesRegistered = false;
  bool validatePosted(const SecretsConfig &s, const RadarConfig &r);
  bool _active = false, _pageOpened = false, _tokenChangeRequested = false;
  PortalStatus _status = PortalStatus::PARADO;
  uint32_t _lastActivity = 0;
  int _scanState = -2;
  bool _testPending = false;
  uint32_t _testStarted = 0;
  String _testResult;
  SecretsConfig _testSecrets;
  void markActivity();
  void registerRoutes();
  void handleRoot();
  void handleSave();
  void handleTest();
  void handleNetworks();
  void handleTestStatus();
  void redirectPortal();
  String page();
  String escapeHtml(const String &value) const;
  SecretsConfig postedSecrets(bool keepExistingToken);
  RadarConfig postedRadar();
};
