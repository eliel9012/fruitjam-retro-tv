#include "ConfigurationPortal.h"
#include "fj/Net.h"
#include "fj/Platform.h"
#include <WiFiUdp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const uint32_t IDLE_LIMIT = 600000UL;
static const uint16_t HTTP_PORT = 80;
static const uint16_t DNS_PORT = 53;
// Limites da requisição. O formulário inteiro dá ~500 bytes; o que passa disso
// é engano ou abuso, e cada conexão guarda o que recebeu na SRAM.
static const size_t MAX_HEAD = 2048;
static const size_t MAX_BODY = 2048;
// Prazo para a requisição chegar inteira depois do accept.
static const uint32_t REQUEST_MS = 3000;

// Um portal só por aparelho: o socket UDP e os buffers do DNS ficam aqui, e
// não no objeto, para o cabeçalho não depender do WiFiUdp.h.
static WiFiUDP dnsSocket;
static uint8_t dnsIn[320], dnsOut[340];

// ---- DNS cativo --------------------------------------------------------------
size_t captiveDnsReply(const uint8_t *q, size_t n, uint8_t *out, size_t cap, const uint8_t ip[4]) {
  if (n < 12)
    return 0;
  const bool isQuery = !(q[2] & 0x80);
  const uint8_t opcode = (q[2] >> 3) & 0x0F;
  const unsigned questions = (unsigned)q[4] << 8 | q[5];
  if (!isQuery || opcode != 0 || questions != 1)
    return 0;
  // QNAME: rótulos até o zero. Ponteiro de compressão não aparece em pergunta.
  size_t p = 12, nameLen = 0;
  for (;;) {
    if (p >= n)
      return 0;
    const uint8_t label = q[p];
    if (!label) {
      ++p;
      break;
    }
    if (label & 0xC0)
      return 0;
    nameLen += label + 1;
    if (nameLen > 255)
      return 0;
    p += 1 + label;
  }
  if (p + 4 > n)
    return 0;
  const unsigned qtype = (unsigned)q[p] << 8 | q[p + 1];
  const unsigned qclass = (unsigned)q[p + 2] << 8 | q[p + 3];
  p += 4;
  const bool answer = (qtype == 1 || qtype == 255) && (qclass == 1 || qclass == 255);
  const size_t len = p + (answer ? 16 : 0);
  if (len > cap)
    return 0;
  // Cabeçalho e pergunta de volta; o que vinha depois (EDNS) fica de fora.
  memcpy(out, q, p);
  out[2] = 0x80 | 0x04 | (q[2] & 0x01); // resposta, autoritativa, mantém o RD
  out[3] = 0x00;                        // sem recursão, sem erro
  out[6] = 0;
  out[7] = answer ? 1 : 0;
  memset(out + 8, 0, 4);
  if (answer) {
    // Nome por ponteiro para a pergunta (offset 12), tipo A, classe IN, TTL 60 s:
    // curto para o celular não guardar o IP do portal depois de sair dele.
    static const uint8_t rr[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04};
    memcpy(out + p, rr, sizeof(rr));
    memcpy(out + p + sizeof(rr), ip, 4);
  }
  return len;
}

// ---- formulário --------------------------------------------------------------
static int hexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// application/x-www-form-urlencoded: '+' é espaço, %XX é um byte. Sequência %
// inválida fica como veio.
static String urlDecode(const char *p, const char *end) {
  String r;
  r.reserve(end - p);
  while (p < end) {
    const char c = *p;
    if (c == '+') {
      r += ' ';
      ++p;
    } else if (c == '%' && end - p >= 3 && hexValue(p[1]) >= 0 && hexValue(p[2]) >= 0) {
      r += (char)(hexValue(p[1]) * 16 + hexValue(p[2]));
      p += 3;
    } else {
      r += c;
      ++p;
    }
  }
  return r;
}

static bool findArg(const String &src, const char *name, String &out) {
  const size_t nameLen = strlen(name);
  const char *p = src.c_str();
  while (*p) {
    const char *amp = strchr(p, '&');
    const char *end = amp ? amp : p + strlen(p);
    const char *eq = static_cast<const char *>(memchr(p, '=', end - p));
    const char *keyEnd = eq ? eq : end;
    if ((size_t)(keyEnd - p) == nameLen && !strncmp(p, name, nameLen)) {
      out = urlDecode(eq ? eq + 1 : end, end);
      return true;
    }
    p = amp ? amp + 1 : end;
  }
  return false;
}

static String span(const char *p, size_t n) {
  String s;
  s.reserve(n);
  s.concat(p, n);
  return s;
}

static const char *reason(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 302:
    return "Found";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 413:
    return "Payload Too Large";
  case 500:
    return "Internal Server Error";
  default:
    return "Status";
  }
}

// ---- portal ------------------------------------------------------------------
void ConfigurationPortal::markActivity() {
  _lastActivity = millis();
}
String ConfigurationPortal::escapeHtml(const String &v) const {
  String r;
  for (size_t i = 0; i < v.length(); ++i) {
    char c = v[i];
    if (c == '&')
      r += "&amp;";
    else if (c == '<')
      r += "&lt;";
    else if (c == '>')
      r += "&gt;";
    else if (c == '\"')
      r += "&quot;";
    else
      r += c;
  }
  return r;
}
void ConfigurationPortal::begin(SecretsManager &s, const SecretsConfig &sec, const RadarConfig &set,
                                PortalSavedCallback cb) {
  if (_active)
    return;
  _testPending = false;
  _callbackPending = false;
  _storage = &s;
  _formToken = String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX) +
               String(esp_random(), HEX);
  _savedSecrets = sec;
  _savedSettings = set;
  _callback = cb;
  _status = PortalStatus::PARADO;
  if (!net::radioReady()) {
    _status = PortalStatus::FALHA;
    return;
  }
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  {
    net::Lock l;
    WiFi.macAddress(mac);
    // Sai de qualquer rede em que a estação estivesse: com o AP no ar, uma
    // reconexão em outro canal arrastaria o AP junto.
    WiFi.disconnect();
  }
  // A WiFiNINA devolve o MAC em ordem que varia entre versões do firmware; o XOR
  // das duas metades dá um sufixo estável seja qual for a ordem.
  char x[5], p[16];
  snprintf(x, sizeof(x), "%02X%02X", mac[0] ^ mac[2] ^ mac[4], mac[1] ^ mac[3] ^ mac[5]);
  snprintf(p, sizeof(p), "M5TV-%06lu", (unsigned long)(esp_random() % 1000000UL));
  _apSsid = String("M5-RETRO-TV-") + x;
  _apPassword = p;
  if (!net::startAccessPoint(_apSsid.c_str(), _apPassword.c_str()) || !net::listen(HTTP_PORT)) {
    _status = PortalStatus::FALHA;
    return;
  }
  {
    net::Lock l;
    _dnsUp = dnsSocket.begin(DNS_PORT) != 0;
  }
  _active = true;
  _status = PortalStatus::AGUARDANDO;
  _pageOpened = false;
  markActivity();
}
void ConfigurationPortal::close(Connection &c) {
  {
    net::Lock l;
    c.client.stop();
  }
  c.client = WiFiClient();
  c.data = String();
  c.used = false;
}
void ConfigurationPortal::stop() {
  if (!_active)
    return;
  for (auto &c : _conns)
    if (c.used)
      close(c);
  if (_dnsUp) {
    net::Lock l;
    dnsSocket.stop();
  }
  _dnsUp = false;
  _active = false;
  _testPending = false;
  _callbackPending = false;
  _status = PortalStatus::PARADO;
}
bool ConfigurationPortal::timedOutWarning() const {
  return _active && millis() - _lastActivity >= IDLE_LIMIT - 60000UL;
}
void ConfigurationPortal::extendSession() {
  markActivity();
}

void ConfigurationPortal::serviceDns() {
  if (!_dnsUp)
    return;
  const IPAddress apIp = net::accessPointIp();
  const uint8_t ip[4] = {apIp[0], apIp[1], apIp[2], apIp[3]};
  // Poucos pacotes por volta: o celular dispara várias consultas ao entrar na
  // rede, mas o loop() não pode ficar preso nelas.
  for (int i = 0; i < 4; ++i) {
    net::Lock l;
    const int n = dnsSocket.parsePacket();
    if (n <= 0)
      return;
    // Maior que o buffer não é consulta de nome; o próximo parsePacket descarta.
    if ((size_t)n > sizeof(dnsIn))
      continue;
    const int got = dnsSocket.read(dnsIn, sizeof(dnsIn));
    const size_t len = got > 0 ? captiveDnsReply(dnsIn, (size_t)got, dnsOut, sizeof(dnsOut), ip) : 0;
    if (!len)
      continue;
    if (dnsSocket.beginPacket(dnsSocket.remoteIP(), dnsSocket.remotePort())) {
      dnsSocket.write(dnsOut, len);
      dnsSocket.endPacket();
    }
    if (_status == PortalStatus::AGUARDANDO)
      _status = PortalStatus::CELULAR_CONECTADO;
  }
}

void ConfigurationPortal::acceptConnections() {
  for (auto &slot : _conns) {
    if (slot.used)
      continue;
    // Lotado, as conexões seguintes esperam na fila do firmware.
    WiFiClient c = net::accept(HTTP_PORT);
    if (!c)
      return;
    slot.client = c;
    slot.data = String();
    slot.data.reserve(512);
    slot.openedAt = millis();
    slot.used = true;
    if (_status == PortalStatus::AGUARDANDO)
      _status = PortalStatus::CELULAR_CONECTADO;
  }
}

// Lê o que chegou e, com a requisição inteira, atende e fecha.
void ConfigurationPortal::pump(Connection &c) {
  bool peerClosed = false;
  {
    net::Lock l;
    char chunk[128];
    for (;;) {
      const int avail = c.client.available();
      if (avail <= 0) {
        peerClosed = !c.client.connected();
        break;
      }
      const int got = c.client.read(reinterpret_cast<uint8_t *>(chunk),
                                    (size_t)avail < sizeof(chunk) ? (size_t)avail : sizeof(chunk));
      if (got <= 0)
        break;
      c.data.concat(chunk, (unsigned)got);
      if (c.data.length() > MAX_HEAD + MAX_BODY)
        break;
    }
  }
  const char *s = c.data.c_str();
  const char *headEnd = strstr(s, "\r\n\r\n");
  const bool late = millis() - c.openedAt >= REQUEST_MS;
  if (!headEnd) {
    if (c.data.length() > MAX_HEAD) {
      _client = &c.client;
      send(413, "text/plain", "REQUISICAO GRANDE DEMAIS");
      _client = nullptr;
      close(c);
    } else if (late || peerClosed) {
      close(c);
    }
    return;
  }
  const size_t headLen = headEnd - s + 4;
  size_t bodyLen = 0;
  // Content-Length em qualquer caixa, só dentro do cabeçalho.
  for (const char *line = strstr(s, "\r\n"); line && line < headEnd; line = strstr(line + 2, "\r\n")) {
    static const char name[] = "content-length:";
    if (!strncasecmp(line + 2, name, sizeof(name) - 1)) {
      bodyLen = strtoul(line + 2 + sizeof(name) - 1, nullptr, 10);
      break;
    }
  }
  if (bodyLen > MAX_BODY) {
    _client = &c.client;
    send(413, "text/plain", "REQUISICAO GRANDE DEMAIS");
    _client = nullptr;
    close(c);
    return;
  }
  if (c.data.length() < headLen + bodyLen) {
    if (late || peerClosed)
      close(c);
    return;
  }
  dispatch(c, headLen, bodyLen);
  close(c);
}

void ConfigurationPortal::dispatch(Connection &c, size_t headLen, size_t bodyLen) {
  // "GET /caminho?consulta HTTP/1.1"
  const char *s = c.data.c_str();
  const char *sp1 = strchr(s, ' ');
  const char *lineEnd = strstr(s, "\r\n");
  const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : nullptr;
  _client = &c.client;
  if (!sp1 || !sp2 || sp2 > lineEnd) {
    send(400, "text/plain", "REQUISICAO INVALIDA");
    _client = nullptr;
    return;
  }
  _method = span(s, sp1 - s);
  const char *target = sp1 + 1;
  const char *q = static_cast<const char *>(memchr(target, '?', sp2 - target));
  _path = span(target, (q ? q : sp2) - target);
  _query = q ? span(q + 1, sp2 - q - 1) : String();
  _body = span(s + headLen, bodyLen);

  const bool get = _method == "GET", post = _method == "POST";
  if (get && _path == "/")
    handleRoot();
  else if (get && _path == "/redes")
    handleNetworks();
  else if (post && _path == "/salvar")
    handleSave();
  else if (post && _path == "/testar")
    handleTest();
  else if (get && _path == "/teste-status")
    handleTestStatus();
  else
    // Inclui as sondas de portal cativo (/generate_204, /hotspot-detect.html,
    // /ncsi.txt, /connecttest.txt): qualquer coisa que não seja o portal volta
    // para ele, e é isso que faz o celular abrir a página sozinho.
    redirectPortal();
  _client = nullptr;
  _method = _path = _query = _body = String();
}

void ConfigurationPortal::send(int code, const char *type, const String &body, const char *location) {
  if (!_client)
    return;
  String h;
  h.reserve(192);
  h += "HTTP/1.1 ";
  h += String(code);
  h += ' ';
  h += reason(code);
  h += "\r\nContent-Type: ";
  h += type;
  h += "\r\nContent-Length: ";
  h += String((unsigned long)body.length());
  h += "\r\nCache-Control: no-store\r\nConnection: close\r\n";
  if (location) {
    h += "Location: ";
    h += location;
    h += "\r\n";
  }
  h += "\r\n";
  if (net::writeAll(*_client, h.c_str(), h.length()) == h.length() && body.length())
    net::writeAll(*_client, body.c_str(), body.length());
}

String ConfigurationPortal::arg(const char *name) const {
  String v;
  if (findArg(_query, name, v) || findArg(_body, name, v))
    return v;
  return String();
}

void ConfigurationPortal::redirectPortal() {
  String location = "http://";
  location += net::accessPointIp().toString();
  location += "/";
  send(302, "text/plain", "", location.c_str());
}
String ConfigurationPortal::page() {
  String s = escapeHtml(_savedSecrets.ssid),
         b = escapeHtml(_savedSecrets.baseUrl.length() ? _savedSecrets.baseUrl : "https://app.meulab.fun"),
         e = escapeHtml(_savedSecrets.endpoint), hdr = escapeHtml(_savedSecrets.authHeader),
         pre = escapeHtml(_savedSecrets.authPrefix);
  String h =
      R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>M5 RETRO TV</title><style>body{margin:0;background:#06164b;color:#fff;font:16px Arial}main{max-width:620px;margin:auto;padding:18px}h1{color:#ffe34d}h2{color:#4eeaff;border-bottom:2px solid #4eeaff;font-size:18px}.b{background:#0b2872;padding:14px;margin:14px 0;border-radius:6px}label{display:block;margin-top:10px}input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}button{padding:12px;margin:15px 8px 0 0;background:#ffe34d;border:0;border-radius:4px;font-weight:bold}.m{color:#bfefff}</style><main><h1>M5 RETRO TV</h1><p class=m>CONFIGURACAO DO SISTEMA</p><form id=config method=post action=/salvar><input type=hidden name=csrf value=")HTML" +
      _formToken +
      R"HTML("><div class=b><h2>REDE WI-FI</h2><a style="color:#4eeaff" href=/redes target=_blank rel=noopener>PROCURAR REDES DISPONIVEIS</a><label>REDE WI-FI</label><input name=ssid value=")HTML" +
      s +
      R"HTML("><label>SENHA DO WI-FI</label><input name=senha type=password placeholder="SENHA NAO E EXIBIDA"><p class=m>Deixe vazio para manter a senha da mesma rede. Para rede aberta, marque abaixo.<label><input style="width:auto" name=aberta type=checkbox value=1> REDE SEM SENHA</label></p></div><div class=b><h2>API DE TRAFEGO AEREO</h2><label>URL BASE DA API</label><input name=base value=")HTML" +
      b + R"HTML("><label>ENDPOINT DE AERONAVES</label><input name=endpoint value=")HTML" + e +
      R"HTML("><label>MODO DE AUTENTICACAO</label><select name=modo><option value=bearer>Bearer Token</option><option value=x-api-key>X-API-Key</option><option value=custom-header>Cabecalho personalizado</option><option value=none>Sem autenticacao</option></select><label>TOKEN DA API</label><input name=token type=password placeholder="TOKEN CONFIGURADO - DIGITE SOMENTE PARA ALTERAR"><label>CABECALHO</label><input name=cabecalho value=")HTML" +
      hdr + R"HTML("><label>PREFIXO</label><input name=prefixo value=")HTML" + pre +
      R"HTML("></div><div class=b><h2>CONFIGURACAO DO RADAR</h2><label>LATITUDE CENTRAL</label><input name=lat type=number min=-90 max=90 step=any required value=")HTML" +
      String(_savedSettings.latitude, 6) +
      R"HTML("><label>LONGITUDE CENTRAL</label><input name=lon type=number min=-180 max=180 step=any required value=")HTML" +
      String(_savedSettings.longitude, 6) +
      R"HTML("><label>ALCANCE</label><select name=alcance><option>50</option><option>100</option><option selected>)HTML" +
      String(_savedSettings.rangeKm) +
      R"HTML(</option><option>250</option><option>500</option></select><label>ATUALIZACAO</label><select name=atualizacao><option>5</option><option>10</option><option>30</option></select></div><button id=testar type=button>TESTAR CONEXAO</button><button>SALVAR E CONECTAR</button></form><div id=resultado role=status aria-live=polite></div><script>
const form=document.getElementById('config'),test=document.getElementById('testar'),result=document.getElementById('resultado');
test.onclick=async()=>{
 test.disabled=true;result.textContent='TESTANDO WI-FI...';
 try{
  const response=await fetch('/testar',{method:'POST',body:new URLSearchParams(new FormData(form))});
  if(!response.ok){result.textContent=await response.text();return;}
  let status;
  do{
   await new Promise(resolve=>setTimeout(resolve,1000));
   status=await (await fetch('/teste-status?json=1')).json();
   result.innerHTML=status.message;
  }while(status.pending);
 }catch(error){result.textContent='CONEXAO COM O PORTAL INTERROMPIDA';}
 finally{test.disabled=false;}
};
</script></main></html>)HTML";
  h.replace("value=" + _savedSecrets.authMode + ">", "value=" + _savedSecrets.authMode + " selected>");
  h.replace("<option>" + String(_savedSettings.refreshSeconds) + "</option>",
            "<option selected>" + String(_savedSettings.refreshSeconds) + "</option>");
  return h;
}
void ConfigurationPortal::handleRoot() {
  markActivity();
  _pageOpened = true;
  _status = PortalStatus::CONFIGURANDO;
  send(200, "text/html; charset=utf-8", page());
}
SecretsConfig ConfigurationPortal::postedSecrets(bool keep) {
  SecretsConfig s = _savedSecrets;
  s.ssid = arg("ssid");
  String pw = arg("senha");
  if (arg("aberta") == "1")
    s.password = "";
  else if (pw.length() || s.ssid != _savedSecrets.ssid)
    s.password = pw;
  s.baseUrl = arg("base");
  s.endpoint = arg("endpoint");
  s.authMode = arg("modo");
  s.authHeader = arg("cabecalho");
  s.authPrefix = arg("prefixo");
  String t = arg("token");
  if (!keep || t.length())
    s.token = t;
  if (s.authMode == "x-api-key")
    s.authHeader = "X-API-Key";
  if (s.authMode == "bearer") {
    s.authHeader = "Authorization";
    s.authPrefix = "Bearer ";
  }
  return s;
}
RadarConfig ConfigurationPortal::postedRadar() {
  RadarConfig r = _savedSettings;
  r.latitude = arg("lat").toDouble();
  r.longitude = arg("lon").toDouble();
  r.rangeKm = arg("alcance").toInt();
  r.refreshSeconds = arg("atualizacao").toInt();
  if (r.rangeKm != 50 && r.rangeKm != 100 && r.rangeKm != 250 && r.rangeKm != 500)
    r.rangeKm = 250;
  if (r.refreshSeconds != 5 && r.refreshSeconds != 10 && r.refreshSeconds != 30)
    r.refreshSeconds = 5;
  return r;
}
void ConfigurationPortal::handleSave() {
  markActivity();
  SecretsConfig s = postedSecrets(true);
  RadarConfig r = postedRadar();
  if (!validatePosted(s, r))
    return;
  _testPending = false;
  if (!_storage->save(s, r)) {
    _status = PortalStatus::FALHA;
    send(500, "text/html; charset=utf-8", "<h1>FALHA AO SALVAR CONFIGURACAO</h1>");
    return;
  }
  _savedSecrets = s;
  _savedSettings = r;
  _status = PortalStatus::SALVO;
  send(200, "text/html; charset=utf-8",
       "<meta charset=utf-8><h1>CONFIGURACAO SALVA!</h1><p>O M5 RETRO TV ESTA SE CONECTANDO A SUA "
       "REDE.</p><p>VOCE JA PODE FECHAR ESTA PAGINA.</p>");
  // Só depois que esta conexão fechar (ver update()).
  _callbackPending = true;
}
void ConfigurationPortal::handleTest() {
  markActivity();
  _testSecrets = postedSecrets(true);
  if (!validatePosted(_testSecrets, postedRadar()))
    return;
  if (!_testSecrets.valid()) {
    send(400, "text/html; charset=utf-8",
         "<meta charset=utf-8><h1>TESTE DE CONEXAO</h1><p>ERRO INFORME A REDE WI-FI</p><p><a "
         "href='/'>VOLTAR</a></p>");
    return;
  }
  _status = PortalStatus::TESTANDO;
  _testPending = true;
  _testStarted = millis();
  _testResult = "CONECTANDO AO WI-FI...";
  // Não testado no Fruit Jam: se o firmware do ESP32-C6 mudar de canal para
  // seguir o roteador, o AP vai junto e o celular pode cair do portal. O
  // original no ESP32 tinha o mesmo risco (AP e estação num rádio só).
  net::startStation(_testSecrets.ssid.c_str(), _testSecrets.password.c_str());
  send(200, "text/html; charset=utf-8",
       "<meta charset=utf-8><meta http-equiv=refresh content='2;url=/teste-status'><h1>TESTE DE "
       "CONEXAO</h1><p>TESTANDO SEM INTERROMPER O M5 RETRO TV...</p>");
}
void ConfigurationPortal::handleTestStatus() {
  markActivity();
  if (arg("json") == "1") {
    send(200, "application/json",
         String("{\"pending\":") + (_testPending ? "true" : "false") + ",\"message\":\"" + _testResult +
             "\"}");
    return;
  }
  String h = "<meta charset=utf-8><h1>TESTE DE CONEXAO</h1><p>" + _testResult + "</p>";
  if (_testPending)
    h += "<meta http-equiv=refresh content='2'>";
  else
    h += "<p><a href='/'>VOLTAR</a></p>";
  send(200, "text/html; charset=utf-8", h);
}
void ConfigurationPortal::handleNetworks() {
  markActivity();
  String h = "<meta charset=utf-8><title>REDES DISPONIVEIS</title><h1>REDES DISPONIVEIS</h1>";
  // Síncrono: o firmware NINA não tem varredura em segundo plano. O loop()
  // para ~2 a 4 s, só quando o usuário pede esta página.
  const int n = net::scanNetworks();
  if (n < 0)
    h += "<p>FALHA NA BUSCA. Atualize esta pagina.</p>";
  else if (n == 0)
    h += "<p>NENHUMA REDE ENCONTRADA. Atualize esta pagina.</p>";
  else {
    net::Lock l;
    for (int i = 0; i < n; i++) {
      const uint8_t item = (uint8_t)i;
      h += "<p>" + escapeHtml(String(WiFi.SSID(item))) + " - " + String(WiFi.RSSI(item)) + " dBm " +
           (net::isOpenNetwork(WiFi.encryptionType(item)) ? "" : "LOCK") + "</p>";
    }
  }
  h += "<p><a href='/'>VOLTAR</a></p>";
  send(200, "text/html; charset=utf-8", h);
}
void ConfigurationPortal::update() {
  if (!_active)
    return;
  serviceDns();
  acceptConnections();
  for (auto &c : _conns)
    if (c.used)
      pump(c);
  if (_callbackPending) {
    _callbackPending = false;
    if (_callback)
      _callback(_savedSecrets, _savedSettings);
  }
  if (_testPending) {
    // Rádio ocupado por outra tarefa: confere na próxima volta.
    net::TryLock l;
    if (l) {
      if (WiFi.status() == WL_CONNECTED && String(WiFi.SSID()) == _testSecrets.ssid) {
        _testPending = false;
        _testResult = "OK WI-FI CONECTADO<br>INTERNET E API AINDA NAO VERIFICADAS";
        if (_testSecrets.baseUrl.length())
          _testResult += "<br>O SERVIDOR SERA VERIFICADO NA TELA DE TRAFEGO AEREO";
        _status = PortalStatus::CONFIGURANDO;
      } else if (millis() - _testStarted >= 10000) {
        _testPending = false;
        _testResult = "ERRO NAO FOI POSSIVEL CONECTAR AO WI-FI";
        _status = PortalStatus::FALHA;
      }
    }
  }
  if (millis() - _lastActivity >= IDLE_LIMIT)
    stop();
}

bool ConfigurationPortal::validatePosted(const SecretsConfig &s, const RadarConfig &r) {
  (void)r;
  String error;
  if (arg("csrf") != _formToken) {
    send(403, "text/plain", "SESSAO EXPIRADA. REABRA O PORTAL.");
    return false;
  }
  if (!s.valid() || s.ssid.length() > 32)
    error = "INFORME UMA REDE WI-FI VALIDA";
  if (s.password.length() && (s.password.length() < 8 || s.password.length() > 63))
    error = "SENHA WI-FI DEVE TER 8 A 63 CARACTERES";
  if (s.baseUrl.length() && !s.baseUrl.startsWith("https://"))
    error = "A API DEVE USAR HTTPS";
  if (s.endpoint.length() && !s.endpoint.startsWith("/"))
    error = "ENDPOINT DEVE COMECAR COM /";
  if (s.authMode != "bearer" && s.authMode != "x-api-key" && s.authMode != "custom-header" &&
      s.authMode != "none")
    error = "MODO DE AUTENTICACAO INVALIDO";
  for (const String *field : {&s.baseUrl, &s.endpoint, &s.authHeader, &s.authPrefix, &s.token}) {
    if (field->indexOf('\r') >= 0 || field->indexOf('\n') >= 0)
      error = "CONFIGURACAO INVALIDA";
  }
  if (s.authMode == "custom-header" && !s.authHeader.length())
    error = "INFORME O CABECALHO";
  const char *fields[] = {"lat", "lon"};
  const double limits[] = {90, 180};
  for (int i = 0; i < 2; ++i) {
    String text = arg(fields[i]);
    char *end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (!text.length() || end == text.c_str() || *end || !isfinite(value) || value < -limits[i] ||
        value > limits[i])
      error = "COORDENADAS INVALIDAS";
  }
  if (error.length()) {
    send(400, "text/plain; charset=utf-8", error);
    return false;
  }
  return true;
}
