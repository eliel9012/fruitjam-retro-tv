#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
using String = std::string;
extern uint32_t fakeMillis;
inline uint32_t millis() {
  return fakeMillis;
}
