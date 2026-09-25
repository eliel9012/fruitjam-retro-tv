// Portal de configuração de ponta a ponta no host: requisições HTTP cruas
// entram por conexões falsas (tests/portal_stubs/fj/Net.h), passam pelo
// parser e pelos handlers de verdade, e a resposta crua é conferida.
#include "ConfigurationPortal.h"
#include "fj/Net.h"
#include <WiFiUdp.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
uint32_t fakeMillis = 1;
FakeWiFi WiFi;
SecretsConfig captured;
RadarConfig capturedRadar;
int saves = 0, callbacks = 0;
bool SecretsManager::save(const SecretsConfig &s, const RadarConfig &r) {
  captured = s;
  capturedRadar = r;
  ++saves;
  return true;
}
void saved(const SecretsConfig &, const RadarConfig &) {
  // O callback só pode rodar com a resposta já entregue e a conexão fechada.
  ++callbacks;
}

static std::string encode(const std::string &v) {
  static const char hex[] = "0123456789ABCDEF";
  std::string r;
  for (unsigned char c : v) {
    if (isalnum(c) || c == '-' || c == '.' || c == '_')
      r += char(c);
    else if (c == ' ')
      r += '+';
    else {
      r += '%';
      r += hex[c >> 4];
      r += hex[c & 15];
    }
  }
  return r;
}
static std::string form(const std::map<std::string, std::string> &args) {
  std::string r;
  for (const auto &kv : args) {
    if (!r.empty())
      r += '&';
    r += kv.first + "=" + encode(kv.second);
  }
  return r;
}
static std::string get(const std::string &target) {
  return "GET " + target + " HTTP/1.1\r\nHost: 192.168.4.1\r\nAccept: */*\r\n\r\n";
}
static std::string post(const std::string &path, const std::string &body) {
  return "POST " + path +
         " HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\n" +
         "content-length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
struct Response {
  int code = 0;
  std::string head, body;
};
static Response parse(const std::string &raw) {
  Response r;
  assert(raw.rfind("HTTP/1.1 ", 0) == 0);
  r.code = atoi(raw.c_str() + 9);
  const auto end = raw.find("\r\n\r\n");
  assert(end != std::string::npos);
  r.head = raw.substr(0, end + 2);
  r.body = raw.substr(end + 4);
  // Content-Length tem de bater: o navegador espera exatamente isso.
  const auto cl = r.head.find("Content-Length: ");
  assert(cl != std::string::npos && size_t(atoi(r.head.c_str() + cl + 16)) == r.body.size());
  assert(r.head.find("Connection: close\r\n") != std::string::npos);
  return r;
}
static Response request(ConfigurationPortal &portal, const std::string &raw) {
  auto c = net::Fake::connect(raw);
  portal.update();
  assert(c->stopped);
  return parse(c->out);
}

// Consulta DNS de um nome, tipo `qtype`, com um registro EDNS no fim (que a
// resposta deve largar).
static std::vector<uint8_t> dnsQuery(const std::string &name, uint16_t qtype) {
  std::vector<uint8_t> q = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 1};
  size_t start = 0;
  while (start <= name.size()) {
    size_t dot = name.find('.', start);
    if (dot == std::string::npos)
      dot = name.size();
    q.push_back(uint8_t(dot - start));
    q.insert(q.end(), name.begin() + start, name.begin() + dot);
    start = dot + 1;
  }
  q.push_back(0);
  q.insert(q.end(), {uint8_t(qtype >> 8), uint8_t(qtype), 0, 1});
  q.insert(q.end(), {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0}); // OPT
  return q;
}

static void testDnsBuilder() {
  const uint8_t ip[4] = {192, 168, 4, 1};
  uint8_t out[340];
  auto q = dnsQuery("connectivitycheck.gstatic.com", 1);
  const size_t questionEnd = q.size() - 11;
  size_t n = captiveDnsReply(q.data(), q.size(), out, sizeof(out), ip);
  assert(n == questionEnd + 16);
  assert(out[0] == 0x12 && out[1] == 0x34);           // mesmo ID
  assert(out[2] == 0x85 && out[3] == 0x00);           // resposta, AA, RD
  assert(out[7] == 1 && out[9] == 0 && out[11] == 0); // 1 resposta, sem EDNS
  assert(!memcmp(out + n - 4, ip, 4));
  q = dnsQuery("www.apple.com", 28); // AAAA: sem resposta, sem erro
  n = captiveDnsReply(q.data(), q.size(), out, sizeof(out), ip);
  assert(n == q.size() - 11 && out[7] == 0 && (out[3] & 0x0F) == 0);
  q[2] |= 0x80; // já é resposta
  assert(!captiveDnsReply(q.data(), q.size(), out, sizeof(out), ip));
  q = dnsQuery("a.b", 1);
  q[12] = 60; // rótulo passa do fim do pacote
  assert(!captiveDnsReply(q.data(), q.size(), out, sizeof(out), ip));
  assert(!captiveDnsReply(q.data(), 11, out, sizeof(out), ip));
}

int main() {
  testDnsBuilder();

  ConfigurationPortal portal;
  SecretsManager store;
  SecretsConfig s;
  RadarConfig r;
  s.ssid = "old-network";
  s.password = "existing-password";
  s.authMode = "x-api-key";
  s.token = "private-token";
  s.authHeader = "X-API-Key";
  r.refreshSeconds = 30;
  r.vhsOsd = false;
  r.audioOutput = "mudo";

  // Sem o ESP32-C6 o portal não sobe, e diz por quê.
  net::Fake::radio = false;
  portal.begin(store, s, r, saved);
  assert(!portal.active() && portal.status() == PortalStatus::FALHA && net::Fake::apStarts == 0);
  net::Fake::radio = true;

  portal.begin(store, s, r, saved);
  assert(portal.active() && portal.status() == PortalStatus::AGUARDANDO);
  assert(portal.apSsid() == "M5-RETRO-TV-7000" && net::Fake::apSsid == portal.apSsid());
  assert(portal.apPassword().size() == 11 && net::Fake::apPassword == portal.apPassword());

  // DNS cativo: o celular pergunta, o portal responde com o próprio IP.
  WiFiUDP::incoming.push_back(dnsQuery("captive.apple.com", 1));
  portal.update();
  assert(WiFiUDP::sent.size() == 1 && WiFiUDP::sent[0].back() == 1 && WiFiUDP::sent[0][7] == 1);
  assert(portal.status() == PortalStatus::CELULAR_CONECTADO);

  // Sonda de portal cativo: redireciona para a página.
  Response res = request(portal, get("/generate_204"));
  assert(res.code == 302 && res.head.find("Location: http://192.168.4.1/\r\n") != std::string::npos);

  // Requisição em dois pedaços: a primeira volta não pode responder nem fechar.
  auto slow = net::Fake::connect("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n");
  portal.update();
  assert(!slow->stopped && slow->out.empty());
  slow->in += "\r\n";
  portal.update();
  assert(slow->stopped);
  res = parse(slow->out);
  assert(res.code == 200 && portal.status() == PortalStatus::CONFIGURANDO);
  assert(res.body.find("existing-password") == std::string::npos &&
         res.body.find("private-token") == std::string::npos);
  assert(res.body.find("value=x-api-key selected>") != std::string::npos &&
         res.body.find("<option selected>30</option>") != std::string::npos);
  const std::string marker = "name=csrf value=\"";
  auto pos = res.body.find(marker);
  assert(pos != std::string::npos);
  pos += marker.size();
  const std::string csrf = res.body.substr(pos, res.body.find('"', pos) - pos);

  std::map<std::string, std::string> args = {{"ssid", "old-network"},
                                             {"senha", ""},
                                             {"modo", "x-api-key"},
                                             {"token", ""},
                                             {"base", "https://example.com"},
                                             {"endpoint", "/aircraft"},
                                             {"lat", "-23.5"},
                                             {"lon", "-46.6"},
                                             {"alcance", "250"},
                                             {"atualizacao", "30"}};
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 403 && saves == 0);
  args["csrf"] = csrf;
  args["lat"] = "nan";
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 400 && saves == 0);
  args["lat"] = "-23.5";
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 200 && saves == 1 && callbacks == 1 && portal.status() == PortalStatus::SALVO);
  assert(captured.password == "existing-password" && captured.token == "private-token" &&
         captured.authHeader == "X-API-Key" && captured.baseUrl == "https://example.com" &&
         captured.endpoint == "/aircraft");
  assert(capturedRadar.audioOutput == "mudo" && !capturedRadar.vhsOsd && capturedRadar.refreshSeconds == 30);
  args["aberta"] = "1";
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 200 && captured.password.empty());
  args.erase("aberta");
  args["ssid"] = "Rede do Joao & Cia";
  args["senha"] = "senha com espaco+mais";
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 200 && captured.ssid == "Rede do Joao & Cia" &&
         captured.password == "senha com espaco+mais");
  args["ssid"] = "old-network";
  args["senha"] = "";
  args["lat"] = "91";
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 400);
  args["lat"] = "-23.5";
  args["token"] = "bad\r\nHeader: injected";
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 400);
  args["token"] = "";

  // Corpo acima do limite: recusado sem ler o resto.
  res = request(portal, "POST /salvar HTTP/1.1\r\nContent-Length: 5000\r\n\r\n");
  assert(res.code == 413);

  // Conexão especulativa que nunca manda nada: fecha no prazo, sem resposta.
  auto idle = net::Fake::connect("");
  portal.update();
  assert(!idle->stopped);
  fakeMillis += 3001;
  portal.update();
  assert(idle->stopped && idle->out.empty());

  // Redes disponíveis: nome escapado, cadeado só na protegida.
  res = request(portal, get("/redes"));
  assert(res.code == 200 && res.body.find("Casa &lt;5G&gt; - -40 dBm LOCK") != std::string::npos &&
         res.body.find("Aberta - -40 dBm </p>") != std::string::npos);

  res = request(portal, post("/testar", form(args)));
  assert(res.code == 200 && WiFi.attempts == 1 && portal.status() == PortalStatus::TESTANDO);
  WiFi.statusValue = WL_CONNECTED;
  portal.update();
  res = request(portal, get("/teste-status"));
  assert(res.body.find("INTERNET E API AINDA NAO VERIFICADAS") != std::string::npos);
  res = request(portal, get("/teste-status?json=1"));
  assert(res.body.find("\"pending\":false") != std::string::npos);

  portal.stop();
  assert(!portal.active() && portal.status() == PortalStatus::PARADO);
  portal.begin(store, s, r, saved);
  res = request(portal, post("/salvar", form(args)));
  assert(res.code == 403); // token da sessão anterior
  fakeMillis += 600001;
  portal.update();
  assert(!portal.active());
  std::cout << "PASS: portal HTTP parser, captive DNS, routes, validation, secret preservation, form "
               "defaults, truthful Wi-Fi test, slow and oversized requests, expiry\n";
}
