// STUB — implementação pendente (ver PORTING.md, trabalho "hal-net").
#include "fj/Net.h"
namespace net {
bool beginRadio() { return false; }
bool radioReady() { return false; }
String firmwareVersion() { return String(); }
Lock::Lock() {}
Lock::~Lock() {}
HttpResult httpGet(const char *, char *buf, size_t cap, uint32_t, const char *) {
  if (cap)
    buf[0] = 0;
  HttpResult r;
  r.status = -1;
  return r;
}
uint32_t ntpEpoch() { return 0; }
} // namespace net
