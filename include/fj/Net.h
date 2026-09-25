#pragma once
// Rede do Fruit Jam: o ESP32-C6 da placa roda o firmware NINA da Adafruit e
// fala com o RP2350 por SPI1 (biblioteca WiFiNINA, fork da Adafruit).
//
// Diferenças que mudam o código em relação ao ESP32 original:
//
// - TLS roda DENTRO do ESP32-C6 (WiFiSSLClient). O RP2350 não faz handshake,
//   então some o problema de pilha do mbedTLS (AGENTS.md 2.3) — mas some também
//   o controle de CA: o NINA valida com o bundle de raízes dele. O ca.pem do
//   cartão deixa de ser usado.
// - Não há HTTPClient, WebServer nem DNSServer. Este módulo dá um GET HTTP(S)
//   mínimo; o portal de configuração tem servidor próprio sobre WiFiServer.
// - O SPI1 é um barramento só: todas as chamadas WiFiNINA precisam passar por
//   net::Lock (mutex recursivo), porque loop() e tarefas de rede disputam o
//   coprocessador. Uma transação cortada no meio trava o NINA até o reset.
// - Hora: o NINA faz NTP sozinho; WiFi.getTime() devolve epoch UTC.
#include <Arduino.h>
#include <WiFiNINA.h>

namespace net {

// Liga o SPI com os pinos do Fruit Jam e acorda o ESP32-C6. Pulsa o GPIO 22,
// que também zera o DAC: chamar ANTES de audioout::begin(). Idempotente.
bool beginRadio();
bool radioReady();
String firmwareVersion();

// Exclusão mútua do SPI1. RAII: `{ net::Lock l; WiFi.xxx(); }`.
struct Lock {
  Lock();
  ~Lock();
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

struct HttpResult {
  int status = 0;          // código HTTP; negativo = erro de transporte
  size_t length = 0;       // bytes do corpo gravados em buf (sem o '\0')
  bool truncated = false;  // o corpo não coube em cap-1 bytes
};

// GET síncrono. `url` começa com http:// ou https://. Escreve o corpo em `buf`
// (terminado em '\0'), trata Transfer-Encoding: chunked e Content-Length.
// `headers`: linhas extras já com "\r\n" no fim, ou nullptr. Segura o net::Lock
// durante a transação inteira. Pode rodar em tarefa FreeRTOS com 4 KB de pilha:
// não aloca nada grande na pilha.
HttpResult httpGet(const char *url, char *buf, size_t cap, uint32_t timeoutMs = 10000,
                   const char *headers = nullptr);

// Epoch UTC do NTP do ESP32-C6, ou 0 se ainda não sincronizou.
uint32_t ntpEpoch();

} // namespace net
