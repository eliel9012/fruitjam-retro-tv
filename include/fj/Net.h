#pragma once
// Rede do Fruit Jam: o ESP32-C6 da placa roda o firmware NINA da Adafruit e
// fala com o RP2350 por SPI1 (biblioteca WiFiNINA, fork da Adafruit).
//
// Diferenças que mudam o código em relação ao ESP32 original:
//
// - TLS roda DENTRO do ESP32-C6 (WiFiSSLClient). O RP2350 não faz handshake,
//   então some o problema de pilha do mbedTLS (AGENTS.md 2.3) — mas some também
//   o controle de CA: o NINA valida com o bundle de raízes dele. O ca.pem do
//   cartão deixa de ser usado, e não há como desligar a validação (o antigo
//   allow_insecure_tls do secrets.json é lido e ignorado).
// - Não há HTTPClient, WebServer nem DNSServer. Este módulo dá um GET HTTP(S)
//   mínimo; o portal de configuração tem servidor próprio sobre net::accept().
// - O SPI1 é um barramento só: todas as chamadas WiFiNINA precisam passar por
//   net::Lock (mutex recursivo), porque loop() e tarefas de rede disputam o
//   coprocessador. Uma transação cortada no meio trava o NINA até o reset.
// - Hora: o NINA faz NTP sozinho depois de conectar; net::ntpEpoch() lê.
//
// Armadilhas da biblioteca WiFiNINA que este módulo contorna — não chame as
// versões da biblioteca no lugar das daqui:
//
// - WiFi.begin() e WiFi.beginAP() BLOQUEIAM: fazem delay(5000) em laço, até
//   10 vezes, esperando o status mudar. Use net::startStation() e
//   net::startAccessPoint(), que só mandam o comando.
// - WiFi.scanNetworks() repete delay(2000) até 10 vezes se não achar rede.
//   Use net::scanNetworks().
// - WiFi.getTime() lê a resposta num uint32_t, mas o firmware do ESP32-C6
//   (ESP-IDF 5, time_t de 64 bits) manda 8 bytes: estouro de pilha. Use
//   net::ntpEpoch().
// - WiFiServer não tem stop(), e o firmware não tem comando para fechar um
//   servidor TCP: o listen fica aberto até o reset do ESP32-C6, e um segundo
//   WiFiServer na mesma porta falha. Use net::listen()/net::accept(), que
//   mantêm um único servidor por porta durante toda a vida do aparelho e o
//   compartilham entre quem precisar (portal e transferência usam a 80).
// - Com o ESP32-C6 ausente ou travado, qualquer chamada da biblioteca espera o
//   sinal de pronto para sempre. Tudo aqui confere net::radioReady() antes.
#include <Arduino.h>
#include <WiFiNINA.h>

namespace net {

// Liga o SPI com os pinos do Fruit Jam e acorda o ESP32-C6. Pulsa o GPIO 22,
// que também zera o DAC: chamar ANTES de audioout::begin(). Idempotente: só a
// primeira chamada mexe no hardware; as outras devolvem o mesmo resultado.
// Bloqueia ~1 s (reset do ESP32-C6) e no máximo ~5 s se ele não responder.
bool beginRadio();
// true se o beginRadio() achou o ESP32-C6 respondendo. Sem isso, nenhuma
// chamada WiFiNINA pode ser feita: ela travaria esperando o coprocessador.
bool radioReady();
String firmwareVersion();

// Exclusão mútua do SPI1. RAII: `{ net::Lock l; WiFi.xxx(); }`. Recursivo: a
// mesma tarefa pode tomar de novo (as funções daqui tomam por conta própria).
struct Lock {
  Lock();
  ~Lock();
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

// Tentativa com prazo. Para o loop(): se uma tarefa está no meio de um GET, o
// rádio está ocupado — e funcionando —, então o loop() pula a vez em vez de
// travar a interface pelos segundos da transação.
//   net::TryLock l; if (!l) return;
struct TryLock {
  explicit TryLock(uint32_t waitMs = 0);
  ~TryLock();
  explicit operator bool() const { return owned; }
  TryLock(const TryLock &) = delete;
  TryLock &operator=(const TryLock &) = delete;

private:
  bool owned = false;
};

// Manda o ESP32-C6 conectar como estação e volta na hora; o progresso se
// acompanha por WiFi.status(). Senha vazia = rede aberta. false se o comando
// não foi aceito (rádio ausente, SSID vazio ou longo demais).
bool startStation(const char *ssid, const char *password);

// Sobe o ponto de acesso (WPA2; senha de 8 a 63 caracteres). O firmware só
// responde depois de criar o AP, então true já quer dizer AP no ar. O firmware
// NÃO tem comando para derrubar o AP: ele continua no ar até o próximo reset do
// ESP32-C6, mesmo depois que o portal fecha.
bool startAccessPoint(const char *ssid, const char *password, uint8_t channel = 1);
// IP do AP. É o padrão do arduino-esp32 dentro do firmware do ESP32-C6; a
// WiFiNINA não tem comando para mudar nem para ler.
IPAddress accessPointIp();

// Servidor TCP persistente na porta dada (ver armadilha acima). listen() é
// idempotente e devolve false se o ESP32-C6 recusou. accept() devolve o próximo
// cliente que conectou, ou um WiFiClient vazio (operator bool == false).
bool listen(uint16_t port);
WiFiClient accept(uint16_t port);

// Escreve tudo em pedaços de 1 KB, com o net::Lock por pedaço. Cada write() da
// WiFiNINA vira um comando SPI; um pedaço grande demais estoura o buffer de
// comando do firmware, e um write por byte vira um pacote TCP por byte.
// Devolve quantos bytes saíram (menos que `n` = conexão caiu).
size_t writeAll(WiFiClient &client, const void *data, size_t n);

// Varredura síncrona: o firmware só responde quando termina, ~2 a 4 s. Devolve
// o número de redes (depois, WiFi.SSID(i)/RSSI(i)/encryptionType(i) com o
// net::Lock tomado) ou negativo em erro.
int scanNetworks();
// Rede aberta segundo WiFi.encryptionType(i). O firmware antigo do NINA usava
// ENC_TYPE_NONE (7); o do ESP32-C6 repassa o wifi_auth_mode_t do ESP-IDF, em
// que aberta é 0. Aceita os dois.
bool isOpenNetwork(uint8_t encryptionType);

struct HttpResult {
  int status = 0;         // código HTTP; negativo = erro de transporte (HttpError)
  size_t length = 0;      // bytes do corpo gravados em buf (sem o '\0')
  bool truncated = false; // o corpo não coube em cap-1 bytes
};

enum HttpError : int {
  HTTP_ERR_RADIO = -1,    // ESP32-C6 ausente
  HTTP_ERR_URL = -2,      // URL malformada ou esquema diferente de http/https
  HTTP_ERR_CONNECT = -3,  // DNS, TCP ou handshake TLS (inclui certificado recusado)
  HTTP_ERR_WRITE = -4,    // o pedido não saiu inteiro
  HTTP_ERR_TIMEOUT = -5,  // prazo total estourado antes do fim da resposta
  HTTP_ERR_PROTOCOL = -6, // linha de status ou chunk malformado
};

// GET síncrono. `url` começa com http:// ou https://. Escreve o corpo em `buf`
// (terminado em '\0'), trata Transfer-Encoding: chunked e Content-Length.
// `headers`: linhas extras já com "\r\n" no fim, ou nullptr. Segura o net::Lock
// durante a transação inteira. Pode rodar em tarefa FreeRTOS com 4 KB de pilha:
// não aloca nada grande na pilha.
//
// `timeoutMs` conta a partir da conexão: o connect() da WiFiNINA tem prazo fixo
// próprio de 10 s (mais a resolução DNS), que não dá para encurtar.
HttpResult httpGet(const char *url, char *buf, size_t cap, uint32_t timeoutMs = 10000,
                   const char *headers = nullptr);

// Epoch UTC do NTP do ESP32-C6, ou 0 se ainda não sincronizou (o firmware só
// liga o SNTP depois da primeira conexão como estação).
uint32_t ntpEpoch();

} // namespace net
