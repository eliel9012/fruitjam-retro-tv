#pragma once
// Pinos e bring-up do Adafruit Fruit Jam (RP2350B). Os números vêm do variant
// `adafruit_fruitjam` do arduino-pico; aqui só ganham nome e comentário.
//
//   DVI (HSTX)      GPIO 12..19          -> fj/Display.h
//   microSD (SDIO)  CLK 34, CMD 35, D0..D3 36..39, detecção 33
//   I2S do DAC      DOUT 24, MCLK 25, BCLK 26, WS 27  (TLV320DAC3100)
//   I2C             SDA 20, SCL 21       (DAC em 0x18)
//   ESP32-C6        SPI1: SCK 30, MOSI 31, MISO 28, CS 46, BUSY 3, GPIO0 23
//   RESET           GPIO 22 — ZERA O DAC *E* O ESP32-C6 JUNTOS
//   Botões          1 = GPIO 0, 2 = GPIO 4, 3 = GPIO 5 (ativos em nível baixo)
//   NeoPixels       5 LEDs no GPIO 32
//   USB host        D+ 1, D- 2, 5V_EN 11
//   PSRAM           8 MB, CS 47
//
// A armadilha do GPIO 22: o WiFiNINA pulsa o reset do ESP32-C6 no WiFi.begin
// / primeira transação SPI, e esse mesmo pulso zera o TLV320. Por isso a ordem
// no boot é FIXA: board::begin() -> net::beginRadio() -> audioout::begin().
// Configurar o DAC antes do rádio faz o áudio morrer em silêncio no primeiro
// acesso à rede. Ver PORTING.md.
#include <Arduino.h>

namespace board {

constexpr uint8_t BTN1 = PIN_BUTTON1; // "A": esquerda / anterior
constexpr uint8_t BTN2 = PIN_BUTTON2; // "B": selecionar
constexpr uint8_t BTN3 = PIN_BUTTON3; // "C": direita / próximo
constexpr uint8_t NEOPIXEL = PIN_NEOPIXEL;
constexpr uint8_t NEOPIXEL_COUNT = NUM_NEOPIXEL;
constexpr uint8_t PERIPHERAL_RESET = PIN_PERIPHERAL_RESET;
constexpr uint8_t SD_DETECT = PIN_SD_DETECT;

// Solta o reset dos periféricos e configura os botões. Primeira coisa do setup().
void begin();

// true enquanto o botão está pressionado (já descontada a lógica invertida).
bool buttonDown(uint8_t index); // 0, 1, 2

// Cartão presente segundo a chave mecânica do soquete.
bool cardInserted();

} // namespace board
