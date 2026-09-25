#pragma once
// Portal de configuração: o aparelho sobe um ponto de acesso Wi-Fi, o celular
// entra nele e preenche um formulário em http://192.168.4.1/.
//
// No Fruit Jam não existem o WebServer nem o DNSServer do arduino-esp32 (o
// Wi-Fi é o ESP32-C6 com firmware NINA, ver fj/Net.h). Daqui:
//
// - HTTP: servidor mínimo sobre net::accept(80). Uma requisição por conexão
//   (Connection: close), GET e POST de formulário urlencoded, cabeçalho e corpo
//   de até 2 KB cada. Até 3 conexões ao mesmo tempo, cada uma com 3 s para
//   mandar a requisição inteira: navegador abre conexão especulativa e não
//   manda nada, e ela não pode segurar a fila.
// - DNS cativo: WiFiUDP na porta 53 respondendo toda consulta A com o IP do AP,
//   para o celular abrir o portal sozinho. Se o firmware recusar a porta 53, o
//   portal continua funcionando pelo IP digitado (a tela já mostra qual).
// - O firmware NINA não derruba o AP: depois de stop() a rede do portal
//   continua visível até o próximo reset do ESP32-C6, só que sem ninguém
//   atendendo (DNS fechado, HTTP sem leitor).
//
// Tudo roda no loop(), dentro de update(). Nada aqui bloqueia por segundos,
// com uma exceção pedida pelo usuário: a página de redes disponíveis faz a
// varredura síncrona (~2 a 4 s), porque o firmware NINA não tem varredura
// assíncrona.
#include <Arduino.h>
#include <WiFiNINA.h>
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
  struct Connection {
    WiFiClient client;
    String data;
    uint32_t openedAt = 0;
    bool used = false;
  };
  static const int MAX_CONNECTIONS = 3;
  Connection _conns[MAX_CONNECTIONS];
  // Requisição em atendimento (válidos só dentro de dispatch()).
  WiFiClient *_client = nullptr;
  String _method, _path, _query, _body;

  SecretsManager *_storage = nullptr;
  SecretsConfig _savedSecrets;
  RadarConfig _savedSettings;
  PortalSavedCallback _callback = nullptr;
  String _apSsid, _apPassword, _formToken;
  bool _active = false, _pageOpened = false, _dnsUp = false;
  // O callback do "salvar" roda depois que a resposta saiu e a conexão fechou:
  // ele manda o rádio conectar como estação, o que pode derrubar o AP.
  bool _callbackPending = false;
  PortalStatus _status = PortalStatus::PARADO;
  uint32_t _lastActivity = 0;
  bool _testPending = false;
  uint32_t _testStarted = 0;
  String _testResult;
  SecretsConfig _testSecrets;

  void markActivity();
  void serviceDns();
  void acceptConnections();
  void pump(Connection &c);
  void close(Connection &c);
  void dispatch(Connection &c, size_t headLen, size_t bodyLen);
  void send(int code, const char *type, const String &body, const char *location = nullptr);
  String arg(const char *name) const;
  bool validatePosted(const SecretsConfig &s, const RadarConfig &r);
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

// Resposta do DNS cativo para a consulta `query` (n bytes): copia o cabeçalho e
// a pergunta e, se for tipo A (ou ANY) classe IN, acrescenta uma resposta com
// `ip`, TTL de 60 s. Outros tipos (AAAA, HTTPS...) saem sem resposta e sem erro,
// o que faz o celular cair para o A. Devolve o tamanho escrito em `out`, ou 0
// para descartar (pacote malformado, não é pergunta, mais de uma pergunta).
size_t captiveDnsReply(const uint8_t *query, size_t n, uint8_t *out, size_t cap, const uint8_t ip[4]);
