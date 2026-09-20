#include "ConfigurationPortal.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdlib.h>
static const IPAddress AP_IP(192, 168, 4, 1);
static const uint32_t IDLE_LIMIT = 600000UL;
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
  _scanState = -2;
  _storage = &s;
  _formToken = String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX) +
               String(esp_random(), HEX);
  _savedSecrets = sec;
  _savedSettings = set;
  _callback = cb;
  char x[5], p[16];
  snprintf(x, sizeof(x), "%04X", (uint16_t)ESP.getEfuseMac());
  snprintf(p, sizeof(p), "M5TV-%06lu", (unsigned long)(esp_random() % 1000000UL));
  _apSsid = String("M5-RETRO-TV-") + x;
  _apPassword = p;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(_apSsid.c_str(), _apPassword.c_str())) {
    _status = PortalStatus::FALHA;
    return;
  }
  _dns.start(53, "*", AP_IP);
  registerRoutes();
  _server.begin();
  _active = true;
  _status = PortalStatus::AGUARDANDO;
  _pageOpened = false;
  _tokenChangeRequested = false;
  markActivity();
}
void ConfigurationPortal::stop() {
  if (!_active)
    return;
  _dns.stop();
  _server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _active = false;
  _testPending = false;
  WiFi.scanDelete();
  _scanState = -2;
  _status = PortalStatus::PARADO;
}
bool ConfigurationPortal::timedOutWarning() const {
  return _active && millis() - _lastActivity >= IDLE_LIMIT - 60000UL;
}
void ConfigurationPortal::extendSession() {
  markActivity();
}
void ConfigurationPortal::redirectPortal() {
  _server.sendHeader("Location", "http://192.168.4.1/", true);
  _server.send(302, "text/plain", "");
}
void ConfigurationPortal::registerRoutes() {
  if (_routesRegistered)
    return;
  _routesRegistered = true;
  _server.on("/", HTTP_GET, [this] { handleRoot(); });
  _server.on("/redes", HTTP_GET, [this] { handleNetworks(); });
  _server.on("/salvar", HTTP_POST, [this] { handleSave(); });
  _server.on("/testar", HTTP_POST, [this] { handleTest(); });
  _server.on("/teste-status", HTTP_GET, [this] { handleTestStatus(); });
  for (const char *p : {"/generate_204", "/hotspot-detect.html", "/ncsi.txt", "/connecttest.txt"})
    _server.on(p, HTTP_ANY, [this] { redirectPortal(); });
  _server.onNotFound([this] { redirectPortal(); });
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
  _server.send(200, "text/html; charset=utf-8", page());
}
SecretsConfig ConfigurationPortal::postedSecrets(bool keep) {
  SecretsConfig s = _savedSecrets;
  s.ssid = _server.arg("ssid");
  String pw = _server.arg("senha");
  if (_server.arg("aberta") == "1")
    s.password = "";
  else if (pw.length() || s.ssid != _savedSecrets.ssid)
    s.password = pw;
  s.baseUrl = _server.arg("base");
  s.endpoint = _server.arg("endpoint");
  s.authMode = _server.arg("modo");
  s.authHeader = _server.arg("cabecalho");
  s.authPrefix = _server.arg("prefixo");
  String t = _server.arg("token");
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
  r.latitude = _server.arg("lat").toDouble();
  r.longitude = _server.arg("lon").toDouble();
  r.rangeKm = _server.arg("alcance").toInt();
  r.refreshSeconds = _server.arg("atualizacao").toInt();
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
    _server.send(500, "text/html; charset=utf-8", "<h1>FALHA AO SALVAR CONFIGURACAO</h1>");
    return;
  }
  _savedSecrets = s;
  _savedSettings = r;
  _status = PortalStatus::SALVO;
  _server.send(200, "text/html; charset=utf-8",
               "<meta charset=utf-8><h1>CONFIGURACAO SALVA!</h1><p>O M5 RETRO TV ESTA SE CONECTANDO A SUA "
               "REDE.</p><p>VOCE JA PODE FECHAR ESTA PAGINA.</p>");
  if (_callback)
    _callback(s, r);
}
void ConfigurationPortal::handleTest() {
  markActivity();
  _testSecrets = postedSecrets(true);
  if (!validatePosted(_testSecrets, postedRadar()))
    return;
  if (!_testSecrets.valid()) {
    _server.send(400, "text/html; charset=utf-8",
                 "<meta charset=utf-8><h1>TESTE DE CONEXAO</h1><p>ERRO INFORME A REDE WI-FI</p><p><a "
                 "href='/'>VOLTAR</a></p>");
    return;
  }
  _status = PortalStatus::TESTANDO;
  _testPending = true;
  _testStarted = millis();
  _testResult = "CONECTANDO AO WI-FI...";
  WiFi.disconnect(false, false);
  WiFi.begin(_testSecrets.ssid.c_str(), _testSecrets.password.c_str());
  _server.send(200, "text/html; charset=utf-8",
               "<meta charset=utf-8><meta http-equiv=refresh content='2;url=/teste-status'><h1>TESTE DE "
               "CONEXAO</h1><p>TESTANDO SEM INTERROMPER O M5 RETRO TV...</p>");
}
void ConfigurationPortal::handleTestStatus() {
  markActivity();
  if (_server.arg("json") == "1") {
    _server.send(200, "application/json",
                 String("{\"pending\":") + (_testPending ? "true" : "false") + ",\"message\":\"" +
                     _testResult + "\"}");
    return;
  }
  String h = "<meta charset=utf-8><h1>TESTE DE CONEXAO</h1><p>" + _testResult + "</p>";
  if (_testPending)
    h += "<meta http-equiv=refresh content='2'>";
  else
    h += "<p><a href='/'>VOLTAR</a></p>";
  _server.send(200, "text/html; charset=utf-8", h);
}
void ConfigurationPortal::handleNetworks() {
  markActivity();
  if (_scanState == -2)
    _scanState = WiFi.scanNetworks(true, true);
  int n = WiFi.scanComplete();
  String h = "<meta charset=utf-8><title>REDES DISPONIVEIS</title><h1>REDES DISPONIVEIS</h1>";
  if (n < 0)
    h += "<p>PROCURANDO REDES... Atualize esta pagina.</p>";
  else {
    for (int i = 0; i < n; i++)
      h += "<p>" + escapeHtml(WiFi.SSID(i)) + " - " + String(WiFi.RSSI(i)) + " dBm " +
           (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : "LOCK") + "</p>";
    WiFi.scanDelete();
    _scanState = -2;
  }
  h += "<p><a href='/'>VOLTAR</a></p>";
  _server.send(200, "text/html; charset=utf-8", h);
}
void ConfigurationPortal::update() {
  if (!_active)
    return;
  _dns.processNextRequest();
  _server.handleClient();
  if (_testPending) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == _testSecrets.ssid) {
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
  if (WiFi.softAPgetStationNum() > 0 && !_pageOpened)
    _status = PortalStatus::CELULAR_CONECTADO;
  if (millis() - _lastActivity >= IDLE_LIMIT)
    stop();
}

bool ConfigurationPortal::validatePosted(const SecretsConfig &s, const RadarConfig &r) {
  (void)r;
  String error;
  if (_server.arg("csrf") != _formToken) {
    _server.send(403, "text/plain", "SESSAO EXPIRADA. REABRA O PORTAL.");
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
    String text = _server.arg(fields[i]);
    char *end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (!text.length() || end == text.c_str() || *end || !isfinite(value) || value < -limits[i] ||
        value > limits[i])
      error = "COORDENADAS INVALIDAS";
  }
  if (error.length()) {
    _server.send(400, "text/plain; charset=utf-8", error);
    return false;
  }
  return true;
}
