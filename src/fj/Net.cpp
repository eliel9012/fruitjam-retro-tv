// Rede do Fruit Jam sobre o ESP32-C6 com firmware NINA. Ver fj/Net.h para as
// armadilhas da WiFiNINA que motivam cada função daqui.
#include "fj/Net.h"
#include "fj/Platform.h"

#include "utility/server_drv.h"
#include "utility/spi_drv.h"
#include "utility/wifi_drv.h"
#include "utility/wifi_spi.h"

#include <atomic>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace net {
namespace {

// ---- mutex do SPI1 -----------------------------------------------------------
// Criado no primeiro uso, sem alocação (buffer estático) e sem depender da ordem
// dos construtores globais: quem ganha a troca 0 -> 1 cria; quem perde espera o
// 2. A espera é vTaskDelay, não taskYIELD, para não matar de fome um criador de
// prioridade menor preso no mesmo núcleo.
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutexHandle = nullptr;
std::atomic<int> mutexState{0};

SemaphoreHandle_t spiMutex() {
  if (mutexState.load(std::memory_order_acquire) == 2)
    return mutexHandle;
  int expected = 0;
  if (mutexState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
    mutexHandle = xSemaphoreCreateRecursiveMutexStatic(&mutexStorage);
    mutexState.store(2, std::memory_order_release);
  } else {
    while (mutexState.load(std::memory_order_acquire) != 2)
      vTaskDelay(1);
  }
  return mutexHandle;
}

// ---- estado do rádio ---------------------------------------------------------
// Só mudam com o lock tomado; `ready` é atômico porque radioReady() é lido sem
// lock por quem só quer decidir se vale a pena tentar.
std::atomic<bool> ready{false};
bool attempted = false;
String version;

// Servidores TCP abertos. Nunca fecham (ver fj/Net.h): a tabela é a memória de
// qual socket do firmware atende cada porta.
struct Listener {
  uint16_t port;
  uint8_t sock;
};
Listener listeners[4];
uint8_t listenerCount = 0;

// Rascunhos do httpGet. Estáticos para não pesar na pilha de tarefas de 4 KB;
// seguros porque o httpGet segura o net::Lock do começo ao fim.
char lineBuf[256];
uint8_t sinkBuf[256];

// Resposta do GET_TIME_CMD. waitResponseCmd copia quantos bytes o firmware
// mandar (o tamanho vem num uint8_t), então o destino precisa aguentar 255.
uint8_t timeReply[256];

constexpr size_t WRITE_CHUNK = 1024;

// ---- leitura com prazo ---------------------------------------------------------
class Reader {
public:
  Reader(WiFiClient &c, uint32_t timeoutMs) : _c(c), _start(millis()), _timeout(timeoutMs) {}

  // Espera haver bytes. false = conexão fechada sem mais nada, ou prazo vencido.
  bool wait() {
    for (;;) {
      if (_c.available() > 0)
        return true;
      if (!_c.connected()) {
        closed = true;
        return false;
      }
      if (millis() - _start >= _timeout) {
        timedOut = true;
        return false;
      }
      delay(1);
    }
  }
  int byte() { return wait() ? _c.read() : -1; }
  // Lê até `n` bytes (pelo menos 1, se houver). 0 = fim ou prazo.
  size_t some(uint8_t *dst, size_t n) {
    if (!n || !wait())
      return 0;
    const int got = _c.read(dst, n);
    return got > 0 ? (size_t)got : 0;
  }
  // Uma linha sem o CRLF. Linha maior que o buffer é cortada em silêncio (só
  // interessam a linha de status, Content-Length, Transfer-Encoding e o tamanho
  // do chunk). false = acabou antes de qualquer '\n'.
  bool line(char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
      const int ch = byte();
      if (ch < 0) {
        out[n] = 0;
        return false;
      }
      if (ch == '\n')
        break;
      if (ch != '\r' && n + 1 < cap)
        out[n++] = (char)ch;
    }
    out[n] = 0;
    return true;
  }

  bool closed = false, timedOut = false;

private:
  WiFiClient &_c;
  uint32_t _start, _timeout;
};

struct Url {
  bool tls = false;
  char host[128];
  uint16_t port = 80;
  const char *path = "/";
};

bool parseUrl(const char *url, Url &u) {
  if (!url)
    return false;
  const char *p;
  if (!strncasecmp(url, "https://", 8)) {
    u.tls = true;
    u.port = 443;
    p = url + 8;
  } else if (!strncasecmp(url, "http://", 7)) {
    p = url + 7;
  } else {
    return false;
  }
  size_t n = 0;
  while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?')
    ++n;
  if (!n || n >= sizeof(u.host))
    return false;
  memcpy(u.host, p, n);
  u.host[n] = 0;
  p += n;
  if (*p == ':') {
    char *end = nullptr;
    const unsigned long port = strtoul(p + 1, &end, 10);
    if (end == p + 1 || port == 0 || port > 65535)
      return false;
    u.port = (uint16_t)port;
    p = end;
  }
  if (*p == '/')
    u.path = p;
  else if (*p == '?')
    return false; // "http://host?x" não tem caminho; não vale inventar um
  else if (*p)
    return false;
  return true;
}

// "chunked" em qualquer caixa, em qualquer posição da lista de codificações.
bool mentionsChunked(const char *value) {
  static const char word[] = "chunked";
  for (const char *p = value; *p; ++p)
    if (!strncasecmp(p, word, sizeof(word) - 1))
      return true;
  return false;
}

// Tira do prefixo "Nome:" e dos espaços; nullptr se a linha não é esse cabeçalho.
const char *headerValue(const char *line, const char *name) {
  const size_t n = strlen(name);
  if (strncasecmp(line, name, n) || line[n] != ':')
    return nullptr;
  const char *v = line + n + 1;
  while (*v == ' ' || *v == '\t')
    ++v;
  return v;
}

} // namespace

// ---- lock ----------------------------------------------------------------------
Lock::Lock() {
  xSemaphoreTakeRecursive(spiMutex(), portMAX_DELAY);
}
Lock::~Lock() {
  xSemaphoreGiveRecursive(spiMutex());
}

TryLock::TryLock(uint32_t waitMs) {
  owned = xSemaphoreTakeRecursive(spiMutex(), pdMS_TO_TICKS(waitMs)) == pdTRUE;
}
TryLock::~TryLock() {
  if (owned)
    xSemaphoreGiveRecursive(spiMutex());
}

// ---- rádio ---------------------------------------------------------------------
bool beginRadio() {
  Lock l;
  if (attempted)
    return ready;
  attempted = true;
  // A WiFiNINA ainda sobrescreve CS, BUSY e o barramento com as macros
  // SPIWIFI_* do variant (são as mesmas); reset e GPIO0 só vêm daqui.
  WiFi.setPins(SPIWIFI_SS, SPIWIFI_ACK, ESP32_RESETN, ESP32_GPIO0, &SPIWIFI);
  // SpiDrv::begin(): pulsa o reset (GPIO 22 — zera o DAC junto) e espera 750 ms.
  WiFiDrv::wifiDriverInit();
  // Sem o ESP32-C6 o BUSY nunca desce e a primeira transação da biblioteca
  // esperaria para sempre: só pergunta a versão com o BUSY baixo. O firmware
  // leva ~1 s para subir depois do reset, e durante o boot o pino pode passar
  // por baixo antes de o firmware atender — daí repetir até vir uma versão
  // que comece por dígito.
  const uint32_t start = millis();
  while (!ready && millis() - start < 4000) {
    if (digitalRead(SPIWIFI_ACK) == LOW) {
      const char *v = WiFiDrv::getFwVersion();
      version = v ? String(v) : String();
      ready = version.length() > 0 && isdigit((unsigned char)version[0]);
    }
    if (!ready)
      delay(100);
  }
  if (ready)
    Serial.printf("[NET] ESP32-C6 pronto, firmware NINA %s\n", version.c_str());
  else
    Serial.println("[NET] ESP32-C6 nao respondeu; rede desligada ate o proximo boot");
  return ready;
}

bool radioReady() {
  return ready.load();
}

String firmwareVersion() {
  Lock l;
  return version;
}

bool startStation(const char *ssid, const char *password) {
  Lock l;
  if (!ready || !ssid)
    return false;
  const size_t sl = strlen(ssid), pl = password ? strlen(password) : 0;
  if (!sl || sl > WL_SSID_MAX_LENGTH || pl > WL_WPA_KEY_MAX_LENGTH)
    return false;
  // Os comandos crus, sem o laço de delay(5000) do WiFi.begin().
  const int8_t r = pl ? WiFiDrv::wifiSetPassphrase(ssid, (uint8_t)sl, password, (uint8_t)pl)
                      : WiFiDrv::wifiSetNetwork(ssid, (uint8_t)sl);
  return r == WL_SUCCESS;
}

bool startAccessPoint(const char *ssid, const char *password, uint8_t channel) {
  Lock l;
  if (!ready || !ssid || !password)
    return false;
  const size_t sl = strlen(ssid), pl = strlen(password);
  if (!sl || sl > WL_SSID_MAX_LENGTH || pl < 8 || pl > WL_WPA_KEY_MAX_LENGTH)
    return false;
  // O firmware responde 1 só depois de WiFi.AP.create() dar certo.
  return WiFiDrv::wifiSetApPassphrase(ssid, (uint8_t)sl, password, (uint8_t)pl, channel) == 1;
}

IPAddress accessPointIp() {
  return IPAddress(192, 168, 4, 1);
}

bool listen(uint16_t port) {
  Lock l;
  if (!ready)
    return false;
  for (uint8_t i = 0; i < listenerCount; ++i)
    if (listeners[i].port == port)
      return true;
  if (listenerCount >= sizeof(listeners) / sizeof(listeners[0]))
    return false;
  const uint8_t sock = ServerDrv::getSocket();
  if (sock == NO_SOCKET_AVAIL)
    return false;
  ServerDrv::startServer(port, sock);
  if (!ServerDrv::getServerState(sock)) {
    // O firmware não abriu: devolve o slot para não perder um dos 10 sockets.
    ServerDrv::stopClient(sock);
    return false;
  }
  listeners[listenerCount++] = {port, sock};
  return true;
}

WiFiClient accept(uint16_t port) {
  Lock l;
  if (!listen(port))
    return WiFiClient();
  uint8_t sock = NO_SOCKET_AVAIL;
  for (uint8_t i = 0; i < listenerCount; ++i)
    if (listeners[i].port == port)
      sock = listeners[i].sock;
  // availServer() volta na hora (255) quando o GPIO0 do NINA diz que não há
  // nada pendente, então chamar a cada volta do loop() é barato.
  const uint8_t client = ServerDrv::availServer(sock);
  if (client == NO_SOCKET_AVAIL)
    return WiFiClient();
  return WiFiClient(client);
}

size_t writeAll(WiFiClient &client, const void *data, size_t n) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t sent = 0;
  while (sent < n) {
    const size_t chunk = n - sent < WRITE_CHUNK ? n - sent : WRITE_CHUNK;
    size_t wrote;
    {
      Lock l;
      if (!ready)
        break;
      wrote = client.write(p + sent, chunk);
    }
    if (!wrote)
      break;
    sent += wrote;
  }
  return sent;
}

int scanNetworks() {
  Lock l;
  if (!ready)
    return -1;
  if (WiFiDrv::startScanNetworks() == WL_FAILURE)
    return -1;
  return WiFiDrv::getScanNetworks();
}

bool isOpenNetwork(uint8_t encryptionType) {
  return encryptionType == ENC_TYPE_NONE || encryptionType == 0;
}

// ---- GET -------------------------------------------------------------------------
HttpResult httpGet(const char *url, char *buf, size_t cap, uint32_t timeoutMs, const char *headers) {
  HttpResult res;
  if (cap)
    buf[0] = 0;
  Url u;
  if (!parseUrl(url, u)) {
    res.status = HTTP_ERR_URL;
    return res;
  }
  Lock l;
  if (!ready) {
    res.status = HTTP_ERR_RADIO;
    return res;
  }
  // As duas classes existem na pilha (são pequenas: só o número do socket); o
  // connect() é virtual e o WiFiSSLClient manda o firmware abrir em TLS.
  WiFiClient plain;
  WiFiSSLClient secure;
  WiFiClient &c = u.tls ? static_cast<WiFiClient &>(secure) : plain;
  if (!c.connect(u.host, u.port)) {
    c.stop();
    res.status = HTTP_ERR_CONNECT;
    return res;
  }

  // Pedido montado inteiro e mandado de uma vez: cada write() é um comando SPI
  // e, do outro lado, um segmento TCP.
  String req;
  req.reserve(160 + strlen(u.path) + strlen(u.host) + (headers ? strlen(headers) : 0));
  req += "GET ";
  req += u.path;
  req += " HTTP/1.1\r\nHost: ";
  req += u.host;
  if (u.port != (u.tls ? 443 : 80)) {
    req += ':';
    req += String(u.port);
  }
  req += "\r\nUser-Agent: FruitJamRetroTV/1.0\r\nAccept: */*\r\nConnection: close\r\n";
  if (headers)
    req += headers;
  req += "\r\n";
  if (writeAll(c, req.c_str(), req.length()) != req.length()) {
    c.stop();
    res.status = HTTP_ERR_WRITE;
    return res;
  }

  Reader r(c, timeoutMs);
  auto fail = [&](int code) {
    c.stop();
    res.status = r.timedOut ? HTTP_ERR_TIMEOUT : code;
    if (cap)
      buf[res.length < cap ? res.length : cap - 1] = 0;
    return res;
  };

  // Linha de status. "HTTP/1.1 200 OK"; "HTTP/1.0" também serve.
  if (!r.line(lineBuf, sizeof(lineBuf)) || strncmp(lineBuf, "HTTP/", 5))
    return fail(HTTP_ERR_PROTOCOL);
  const char *sp = strchr(lineBuf, ' ');
  const int status = sp ? atoi(sp + 1) : 0;
  if (status < 100 || status > 999)
    return fail(HTTP_ERR_PROTOCOL);

  bool chunked = false;
  bool haveLength = false;
  size_t contentLength = 0;
  for (;;) {
    if (!r.line(lineBuf, sizeof(lineBuf)))
      return fail(HTTP_ERR_PROTOCOL);
    if (!lineBuf[0])
      break;
    if (const char *v = headerValue(lineBuf, "Content-Length")) {
      haveLength = true;
      contentLength = strtoul(v, nullptr, 10);
    } else if (const char *te = headerValue(lineBuf, "Transfer-Encoding")) {
      chunked = mentionsChunked(te);
    }
  }

  // Guarda o que couber em buf (até cap-1) e descarta o resto, contando.
  // Devolve quantos bytes foram consumidos da conexão.
  auto consume = [&](size_t want, bool untilClose) -> size_t {
    size_t done = 0;
    while (untilClose || done < want) {
      const size_t left = untilClose ? SIZE_MAX : want - done;
      const size_t room = cap > res.length + 1 ? cap - 1 - res.length : 0;
      size_t got;
      if (room) {
        got = r.some(reinterpret_cast<uint8_t *>(buf) + res.length, left < room ? left : room);
        res.length += got;
      } else {
        got = r.some(sinkBuf, left < sizeof(sinkBuf) ? left : sizeof(sinkBuf));
        if (got)
          res.truncated = true;
      }
      if (!got)
        break;
      done += got;
    }
    return done;
  };

  // HEAD não existe aqui, mas 1xx/204/304 não têm corpo por definição.
  const bool noBody = status < 200 || status == 204 || status == 304;
  if (noBody) {
    // nada
  } else if (chunked) {
    for (;;) {
      if (!r.line(lineBuf, sizeof(lineBuf)))
        return fail(HTTP_ERR_PROTOCOL);
      char *end = nullptr;
      const unsigned long size = strtoul(lineBuf, &end, 16);
      if (end == lineBuf)
        return fail(HTTP_ERR_PROTOCOL);
      if (!size) {
        // Trailers opcionais até a linha vazia; conexão fechada aqui também serve.
        while (r.line(lineBuf, sizeof(lineBuf)) && lineBuf[0]) {
        }
        break;
      }
      if (consume(size, false) != size)
        return fail(HTTP_ERR_PROTOCOL);
      if (!r.line(lineBuf, sizeof(lineBuf))) // CRLF depois do chunk
        return fail(HTTP_ERR_PROTOCOL);
    }
  } else if (haveLength) {
    if (consume(contentLength, false) != contentLength)
      return fail(HTTP_ERR_PROTOCOL);
  } else {
    // Sem tamanho e sem chunked: o corpo vai até o servidor fechar.
    consume(0, true);
    if (r.timedOut)
      return fail(HTTP_ERR_TIMEOUT);
  }

  c.stop();
  if (cap)
    buf[res.length] = 0;
  res.status = status;
  return res;
}

// ---- hora ------------------------------------------------------------------------
uint32_t ntpEpoch() {
  Lock l;
  if (!ready)
    return 0;
  // Mesma sequência do WiFiDrv::getTime(), com destino grande o bastante para a
  // resposta de 8 bytes do firmware do ESP32-C6.
  uint8_t len = 0;
  memset(timeReply, 0, 8);
  WAIT_FOR_SLAVE_SELECT();
  SpiDrv::sendCmd(GET_TIME_CMD, PARAM_NUMS_0);
  SpiDrv::spiSlaveDeselect();
  SpiDrv::waitForSlaveReady();
  SpiDrv::spiSlaveSelect();
  if (!SpiDrv::waitResponseCmd(GET_TIME_CMD, PARAM_NUMS_1, timeReply, &len))
    len = 0;
  SpiDrv::spiSlaveDeselect();
  if (len != 4 && len != 8)
    return 0;
  // Little-endian nos dois chips. Com 8 bytes, a parte alta só deixa de ser zero
  // em 2106.
  uint64_t t = 0;
  for (int i = len - 1; i >= 0; --i)
    t = (t << 8) | timeReply[i];
  return t > 0xFFFFFFFFull ? 0 : (uint32_t)t;
}

} // namespace net
