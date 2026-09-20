#pragma once
#include "Arduino.h"
#include <map>
#include <functional>
constexpr int HTTP_GET = 0, HTTP_POST = 1, HTTP_ANY = 2;
class WebServer {
public:
  static WebServer *last;
  std::map<String, std::function<void()>> routes;
  std::map<String, String> args;
  int code = 0;
  String body;
  explicit WebServer(int) { last = this; }
  void on(const char *p, int, std::function<void()> f) { routes[p] = f; }
  void onNotFound(std::function<void()>) {}
  void send(int status, const char *, const String &text) {
    code = status;
    body = text;
  }
  void sendHeader(const char *, const char *, bool) {}
  String arg(const char *name) { return args[name]; }
  void begin() {}
  void stop() {}
  void handleClient() {}
};
