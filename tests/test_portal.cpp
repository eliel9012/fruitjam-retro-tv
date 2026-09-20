#include "ConfigurationPortal.h"
#include <WiFi.h>
#include <cassert>
#include <iostream>
uint32_t fakeMillis = 1;
FakeESP ESP;
FakeWiFi WiFi;
WebServer *WebServer::last = nullptr;
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
  ++callbacks;
}
int main() {
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
  portal.begin(store, s, r, saved);
  assert(portal.active());
  auto &server = *WebServer::last;
  server.routes["/"]();
  assert(server.code == 200);
  assert(server.body.find("existing-password") == std::string::npos &&
         server.body.find("private-token") == std::string::npos);
  assert(server.body.find("value=x-api-key selected>") != std::string::npos &&
         server.body.find("<option selected>30</option>") != std::string::npos);
  const std::string marker = "name=csrf value=\"";
  auto pos = server.body.find(marker);
  assert(pos != std::string::npos);
  pos += marker.size();
  String csrf = server.body.substr(pos, server.body.find('"', pos) - pos);
  server.args = {{"ssid", "old-network"},
                 {"senha", ""},
                 {"modo", "x-api-key"},
                 {"token", ""},
                 {"base", "https://example.com"},
                 {"endpoint", "/aircraft"},
                 {"lat", "-23.5"},
                 {"lon", "-46.6"},
                 {"alcance", "250"},
                 {"atualizacao", "30"}};
  server.routes["/salvar"]();
  assert(server.code == 403 && saves == 0);
  server.args["csrf"] = csrf;
  server.args["lat"] = "nan";
  server.routes["/salvar"]();
  assert(server.code == 400 && saves == 0);
  server.args["lat"] = "-23.5";
  server.routes["/salvar"]();
  assert(server.code == 200 && saves == 1 && callbacks == 1);
  assert(captured.password == "existing-password" && captured.token == "private-token" &&
         captured.authHeader == "X-API-Key");
  assert(capturedRadar.audioOutput == "mudo" && !capturedRadar.vhsOsd && capturedRadar.refreshSeconds == 30);
  server.args["aberta"] = "1";
  server.routes["/salvar"]();
  assert(server.code == 200 && captured.password.empty());
  server.args["lat"] = "91";
  server.routes["/salvar"]();
  assert(server.code == 400);
  server.args["lat"] = "-23.5";
  server.args["token"] = "bad\r\nHeader: injected";
  server.routes["/salvar"]();
  assert(server.code == 400);
  server.args["token"] = "";
  server.routes["/testar"]();
  assert(server.code == 200 && WiFi.attempts == 1);
  WiFi.statusValue = WL_CONNECTED;
  portal.update();
  server.routes["/teste-status"]();
  assert(server.body.find("INTERNET E API AINDA NAO VERIFICADAS") != std::string::npos);
  server.args["json"] = "1";
  server.routes["/teste-status"]();
  assert(server.body.find("\"pending\":false") != std::string::npos);
  const auto routeCount = server.routes.size();
  portal.stop();
  portal.begin(store, s, r, saved);
  assert(server.routes.size() == routeCount);
  server.routes["/salvar"]();
  assert(server.code == 403); // token from the previous session
  fakeMillis += 600001;
  portal.update();
  assert(!portal.active());
  std::cout
      << "PASS: portal routes, validation, secret preservation, form defaults, truthful Wi-Fi test, expiry\n";
}
