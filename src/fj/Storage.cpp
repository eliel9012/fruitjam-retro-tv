#include "fj/Storage.h"

namespace storage {
namespace {
bool isMounted = false;
}

bool begin() {
  if (isMounted)
    return true;
  // SDIO: o arduino-pico deduz D1..D3 como os pinos seguintes ao D0, que é
  // exatamente o que o Fruit Jam faz (36, 37, 38, 39).
  isMounted = SD.begin(PIN_SD_CLK, PIN_SD_CMD_MOSI, PIN_SD_DAT0_MISO);
  return isMounted;
}

bool mounted() { return isMounted; }
} // namespace storage
