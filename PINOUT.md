# Ligações usadas pelo firmware

Alvo: **Adafruit Fruit Jam** (RP2350B). Não há nada empilhado nem fio externo:
tudo o que o firmware usa já está na placa.

Fontes: `include/fj/Board.h` (os nomes que o código usa) e o variant
`adafruit_fruitjam` do arduino-pico
(`~/.platformio/packages/framework-arduinopico/variants/adafruit_fruitjam/pins_arduino.h`).
Os números abaixo foram copiados de lá; **nenhum foi conferido com multímetro na
placa**. Se o variant mudar, ele vence esta tabela.

## Vídeo — DVI pelo HSTX

| Sinal | GPIO | Observação |
|---|---:|---|
| Clock − / + | 12 / 13 | par diferencial TMDS |
| D0 − / + | 14 / 15 | pista 0 do TMDS (azul, no DVI) |
| D1 − / + | 16 / 17 | pista 1 (verde) |
| D2 − / + | 18 / 19 | pista 2 (vermelho) |

Os oito pinos são o bloco HSTX do RP2350 (GPIO 12..19); só eles servem para o
DVI. O conector tem o formato de HDMI, mas o sinal é **DVI**: não leva áudio. De
quais bits do pixel cada pista tira a cor é decidido em software, em
`src/fj/Display.cpp` (ver `AGENTS.md`, "Ordem de bytes do RGB565").

## Áudio — TLV320DAC3100

| Sinal | GPIO | Observação |
|---|---:|---|
| I2S DOUT | 24 | dados PCM, gerados por PIO |
| I2S MCLK | 25 | relógio mestre do DAC |
| I2S BCLK | 26 | |
| I2S WS (LRCK) | 27 | |
| I2C SDA | 20 | `Wire` (i2c0); DAC no endereço **0x18** |
| I2C SCL | 21 | |

O DAC tem as duas saídas no mesmo chip: o **fone P2** (3,5 mm) e o
**amplificador do alto-falante**. Trocar de uma para a outra é escrever
registrador por I2C, não trocar de periférico — ao contrário do Core2, onde RCA e
alto-falante disputavam o I2S1.

## microSD — SDIO

| Sinal | GPIO | Observação |
|---|---:|---|
| CLK | 34 | também `PIN_SPI0_SCK` |
| CMD | 35 | também `PIN_SPI0_MOSI` |
| D0 | 36 | também `PIN_SPI0_MISO` |
| D1 | 37 | |
| D2 | 38 | |
| D3 | 39 | também `PIN_SPI0_SS` |
| Detecção do cartão | 33 | chave do soquete; nível baixo = cartão presente (pull-up interno) |

O `SD.begin(CLK, CMD, D0)` do arduino-pico deduz D1..D3 como os três pinos
seguintes ao D0, que é como a placa está ligada. O barramento é **só do
cartão**: não há mais LCD dividindo o SPI, como havia no Core2.

## Wi-Fi — ESP32-C6 (firmware NINA)

| Sinal | GPIO | Observação |
|---|---:|---|
| SPI1 SCK | 30 | `SPIWIFI` = `SPI1` |
| SPI1 MOSI | 31 | |
| SPI1 MISO | 28 | |
| CS | 46 | `SPIWIFI_SS` |
| BUSY / READY | 3 | `SPIWIFI_ACK` |
| GPIO0 do ESP32-C6 | 23 | `ESP32_GPIO0` (no variant o mesmo pino também se chama `PIN_I2S_IRQ`) |
| UART TX / RX | 8 / 9 | `Serial1` = `SerialESP32`; ponte para regravar o NINA, o firmware não usa |

O SPI1 é **exclusivo do coprocessador**. Toda chamada WiFiNINA passa pelo
`net::Lock` (ver `include/fj/Net.h`).

## Reset compartilhado

| Sinal | GPIO | Observação |
|---|---:|---|
| `PIN_PERIPHERAL_RESET` | **22** | zera o **TLV320 e o ESP32-C6 juntos** |

O WiFiNINA pulsa este pino ao acordar o ESP32-C6, e o mesmo pulso apaga a
configuração do DAC. Por isso a ordem do boot é fixa:
`board::begin()` → `net::beginRadio()` → `audioout::begin()`.

## Botões, LEDs e o resto

| Uso | GPIO | Observação |
|---|---:|---|
| Botão 1 — "A" (esquerda / anterior) | 0 | ativo em nível baixo; **é também o BOOT** |
| Botão 2 — "B" (selecionar) | 4 | ativo em nível baixo |
| Botão 3 — "C" (direita / próximo) | 5 | ativo em nível baixo |
| NeoPixels (5 LEDs) | 32 | apagados no *soft-off* |
| LED vermelho | 29 | não usado |
| USB host D+ / D− | 1 / 2 | não usado |
| 5V do USB host (enable) | 11 | não usado |
| A0..A5 | 40..45 | não usados |
| PSRAM CS | 47 | 8 MB QSPI, mapeada em memória |

O botão 1 dividir o pino com o BOOT tem uma consequência prática: segurá-lo
durante um reset põe a placa em modo de gravação (drive `RP2350`) em vez de
rodar o firmware.

## O que deixou de existir em relação ao Core2

GPIO 26 do CVBS, o I2S1 do módulo RCA (BCK 19, DATA 2, LRCK 0), o alto-falante
NS4168, o LCD ILI9342C, o touch FT6336U, o RTC BM8563 e o AXP192. O
`PINOUT.md` do upstream (<https://github.com/eliel9012/m5-retro-tv>) continua
sendo a referência para aquele hardware.
