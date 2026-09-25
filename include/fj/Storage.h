#pragma once
// microSD do Fruit Jam em SDIO de 1 bit (CLK 34, CMD 35, D0 36) pela biblioteca
// SD do arduino-pico. Diferente do Core2, o cartão tem barramento próprio: não
// divide nada com o vídeo, então o sdMutex agora só serializa loop() contra a
// tarefa de áudio, não contra o LCD.
#include <Arduino.h>
#include <SD.h>

namespace storage {
bool begin();
bool mounted();
} // namespace storage
