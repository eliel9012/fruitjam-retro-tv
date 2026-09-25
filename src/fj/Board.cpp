#include "fj/Board.h"

#include "fj/Platform.h"

FjEspCompat ESP;

namespace board {

void begin() {
  // Pulso de reset limpo no DAC e no ESP32-C6: ninguém sabe em que estado eles
  // ficaram se o RP2350 reiniciou sozinho (watchdog, upload).
  pinMode(PERIPHERAL_RESET, OUTPUT);
  digitalWrite(PERIPHERAL_RESET, LOW);
  delay(10);
  digitalWrite(PERIPHERAL_RESET, HIGH);
  delay(10);

  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);
  pinMode(SD_DETECT, INPUT_PULLUP);
}

bool buttonDown(uint8_t index) {
  static const uint8_t pins[3] = {BTN1, BTN2, BTN3};
  return index < 3 && digitalRead(pins[index]) == LOW;
}

bool cardInserted() {
  // A chave do soquete fecha para o terra com cartão; o pull-up puxa para cima
  // sem cartão.
  return digitalRead(SD_DETECT) == LOW;
}

} // namespace board
