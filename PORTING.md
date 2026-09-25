# PORTING.md — contrato do port M5Stack Core2 → Adafruit Fruit Jam

Este arquivo é o contrato entre as partes do port. Quem mexe numa parte segue
as regras daqui; quem precisa mudar uma regra muda **este arquivo** junto.

Nada neste port foi testado no aparelho ainda. Tudo o que for escrito aqui
entra como **não testado no Fruit Jam** até alguém gravar e conferir.

---

## 1. O que muda de hardware

| Função | Core2 + RCA (original) | Fruit Jam (este fork) |
|---|---|---|
| CPU | ESP32 240 MHz, 2 núcleos Xtensa | RP2350B, 2× Cortex-M33, **126 MHz** em regime (o DVHSTX sobe o relógio duas vezes -- 240 MHz num preinit, depois o valor exato do modo de vídeo; ver §3.11) |
| SRAM | ~320 KB | 520 KB |
| PSRAM | 4,5 MB | 8 MB (QSPI, CS 47) |
| Vídeo | CVBS NTSC 320×240 (M5ModuleRCA) | **DVI 640×480@60** pelo HSTX, quadro lógico 320×240 dobrado |
| Tela local | LCD 320×240 + touch | **nenhuma** |
| Áudio | I2S1 → módulo RCA, ou alto-falante interno | TLV320DAC3100: **fone P2** ou **alto-falante** da placa |
| Cartão | SPI (VSPI, dividido com o LCD) | **SDIO** próprio |
| Wi-Fi | ESP32 nativo (lwIP + mbedTLS no próprio chip) | **ESP32-C6 com firmware NINA**, por SPI1 (WiFiNINA); TLS roda no C6 |
| Botões | BtnA/B/C (touch) + PWR (AXP192) | 3 botões físicos (GPIO 0, 4, 5) |
| RTC | BM8563 com bateria | **nenhum**: hora só via NTP do ESP32-C6 |
| Energia | AXP192 (desligar de verdade) | sem PMIC: "desligar" = tela preta + dormir até um botão |
| LEDs | — | 5 NeoPixels (GPIO 32) |
| USB host | — (nenhuma porta) | teclado/gamepad, D+ GPIO 1 (D- é D+ +1), 5V_EN GPIO 11 (ver §3.11) |

Pinos: `include/fj/Board.h`.

---

## 2. Camada de plataforma (`include/fj/`, `src/fj/`)

| Arquivo | O que dá |
|---|---|
| `fj/Platform.h` | `ps_malloc`, `heap_caps_*`, `esp_timer_get_time`, `esp_random`, `ESP.getFreeHeap()` etc., `xTaskCreatePinnedToCore` (pilha em **bytes**, como no ESP-IDF). Inclui FreeRTOS. |
| `fj/Gfx.h` | LovyanGFX + `using GfxTarget = lgfx::LovyanGFX`. **Único** include gráfico permitido nos headers compartilhados. |
| `fj/Display.h` | `display::begin()`, `display::waitVsync()`, e o canvas global **`LGFX_Sprite tv`** (320×240 RGB565, é o próprio framebuffer do DVI). |
| `fj/Board.h` | pinos, `board::begin()`, `board::buttonDown(i)`, `board::cardInserted()`. |
| `fj/Storage.h` | `storage::begin()` — SD em SDIO. |
| `fj/AudioOut.h` | `audioout::begin/setRate/setRoute/setVolume/write/flushSilence`. |
| `fj/Net.h` | `net::beginRadio()`, `net::Lock`, `net::httpGet()`, `net::ntpEpoch()`. |

### 2.1 Ordem de boot — fixa

```
board::begin()        reset limpo do DAC e do ESP32-C6 (GPIO 22), botões
display::begin()      DVI no ar; interrupção de linha fica no núcleo 0
storage::begin()      SD
net::beginRadio()     acorda o ESP32-C6 — PULSA O GPIO 22, QUE ZERA O DAC
audioout::begin()     só agora configura o TLV320
xTaskCreate...        tarefas (áudio, rede)
```

Configurar o DAC antes do rádio = áudio mudo sem erro nenhum.

---

## 3. Regras do port

### 3.1 Renomeações já feitas

- `rca` → **`tv`** (o canvas da TV). Tudo o que era `rca.` agora é `tv.`.
- `M5GFX *` → **`GfxTarget *`**.
- Headers compartilhados incluem `"fj/Gfx.h"`, não `<M5GFX.h>`.

### 3.2 LCD, touch, AXP192, RTC: removidos

- Todo laço `for (auto *d : {&tv, &M5.Display})` vira desenho **só no `tv`**.
- Código que só desenhava no LCD (HUD de 1 Hz, pôster, botão de voltar do LCD,
  barra de toque) é **removido**, não emulado. Não crie um `M5.Display` falso.
- `handleTouch()` e `InputSource::BOTTOM_TOUCH` saem.
- `setBacklight()` sai. O `BurnIn.h` passa a proteger a TV/monitor (escurecer o
  canvas), não mais o backlight.
- `M5.Power.powerOff()` → "DESLIGAR" vira *soft-off*: apaga a tela, cala o
  áudio, apaga os NeoPixels e dorme até um botão; o botão reinicia
  (`rp2040.reboot()`). O item do menu continua se chamando DESLIGAR.
- `M5.Rtc`/`esp_sntp` → `RtcClock.h` passa a usar `net::ntpEpoch()` +
  `settimeofday()`. Sem Wi-Fi a hora é desconhecida — a interface mostra `--:--`.

### 3.3 Botões

Três botões, mesma semântica do Core2 (A = esquerda/anterior, B = selecionar,
C = direita/próximo; segurar B = voltar; segurar mais = início). O
`InputManager` lê `board::buttonDown(0..2)`. O que era o botão PWR do Core2
(atalho para voltar ao início) vira **segurar A+C juntos**.

### 3.4 Áudio

- O enum `AudioOutput { RCA, INTERNAL, MUTED }` e os valores de JSON
  (`"rca"`, `"interno"`, `"mudo"`) **ficam**, por compatibilidade com o
  `settings.json` de quem já usa o firmware. O significado muda:
  `RCA` → `audioout::Route::HEADPHONE` (fone P2, que vai à TV),
  `INTERNAL` → `Route::SPEAKER`.
- Na tela: "TV (P2)" e "ALTO-FALANTE" em vez de "RCA" e "INTERNO".
- Toda a API `i2s_*` do ESP-IDF e o `M5.Speaker` saem; a tarefa de áudio chama
  `audioout::write()`. Trocar de rota não desmonta driver nenhum.
- O DVI não leva áudio.

### 3.5 Vídeo

- Painel sempre RGB565. O ajuste `CONFIGURAÇÕES → CORES` e o
  `applyColorDepth()` saem; `settings.color16` continua sendo lido do JSON e é
  ignorado (compatibilidade).
- `jpegDraw()` escreve direto no `tv` (é o framebuffer). JPEGDEC deve emitir
  `RGB565_BIG_ENDIAN`, que é a ordem que o `LGFX_Sprite` guarda — ver
  `fj/Display.cpp` para o porquê.
- O vídeo continua centralizado em 320×240. Vídeo 320×240 (tela cheia) passa a
  ser aceito; o padrão do `prepare_video.py` sobe para 320×240.
- Não há mais disputa SD × LCD no barramento. O `sdMutex` continua, para loop()
  × tarefa de áudio.

### 3.6 Rede

- `#include <WiFiNINA.h>`. Não existem `WiFi.mode()`, `WiFi.setSleep()`,
  eventos de Wi-Fi, `HTTPClient`, `WebServer`, `DNSServer`,
  `WiFiClientSecure::setCACert`. `WiFiClientSecure` → `WiFiSSLClient`.
- **Toda** chamada WiFiNINA roda com `net::Lock` tomado. Um laço de leitura
  toma o lock por bloco lido, não pela duração inteira de um stream.
- GET de API (previsão, radar): `net::httpGet(url, buf, cap, ...)`. Buffer de
  resposta na PSRAM (`ps_malloc`), nunca na pilha.
- Portal de configuração: servidor HTTP mínimo sobre `WiFiServer` +
  `WiFi.beginAP()`; o "captive DNS" vira resposta DNS feita à mão em `WiFiUDP`
  (se o NINA não permitir, o portal funciona só pelo IP, e a tela diz qual).

### 3.7 Cartão SD — armadilha

**No arduino-pico, `FILE_WRITE` é `O_RDWR | O_CREAT | O_APPEND`.** No ESP32 era
`"w"` (trunca). Gravar um JSON com `FILE_WRITE` aqui **anexa** ao arquivo velho
e produz JSON inválido. Para sobrescrever use `SD.open(path, "w")`.

### 3.8 Memória

| | Onde | Tamanho |
|---|---|---|
| framebuffer do DVI | SRAM (`malloc` no `display::begin`) | 153.600 B |
| `jpegBuffer` (`MAX_JPEG`) | PSRAM | 128 KiB |
| buffers de HTTP, capas, sprites grandes | PSRAM | — |
| pilhas das tarefas | SRAM | ver `xTaskCreatePinnedToCore` |

Sobra ~300 KB de SRAM. Ainda assim: **nada de quadro inteiro extra na SRAM** sem
o chamador pedir, e nada de buffer de KB na pilha.

### 3.9 Pastas no cartão

Continuam em `/M5RETRO/...`, para que o mesmo cartão sirva nos dois aparelhos.

### 3.10 C++

O arduino-pico compila em **gnu++17**. O teste `make -C sim cxx11` continua
existindo para os headers que o upstream compartilha — mantenha-os em C++11
enquanto for razoável, para o fork poder puxar correções do m5-retro-tv.

### 3.11 USB host (teclado/gamepad)

Novo no fork: o Core2 não tinha porta USB host nenhuma. Pilha
[Adafruit_TinyUSB](https://github.com/adafruit/Adafruit_TinyUSB_Arduino) (já
vem dentro do `framework-arduinopico`) + `-DUSE_TINYUSB` no `platformio.ini` +
[Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) (lib_dep à
parte) para bit-bangar um segundo controlador USB num PIO. O contrato e o
porquê de cada decisão estão comentados por extenso em `include/fj/UsbHost.h`
(headers-fonte, não repita aqui) — resumo:

- **Stack `USE_TINYUSB`, nunca `USE_TINYUSB_HOST`.** A segunda vira o
  controlador *nativo* em host e mata o Serial/CDC da gravação. Conferido lendo
  `framework-arduinopico/tools/platformio-build.py` (`configure_usb_flags`) e
  `SerialUSB.cpp` (`#if !defined(USE_TINYUSB)`); `Serial` vira
  `Adafruit_USBD_CDC` por `#define`, e continua funcionando pela mesma USB-C.
- **PIO2, forçado.** O Pico-PIO-USB carrega seu programa sempre no offset 0 do
  PIO escolhido, sem checar o que já está lá — diferente do SDIO/I2S, que usam
  o alocador dinâmico do arduino-pico (`PIOProgram`, tenta PIO0 primeiro).
  Reservar o PIO2 evita a colisão enquanto os outros dois couberem em PIO0/1.
- **Núcleo 1.** O Pico-PIO-USB instala um alarme de hardware de 1 ms
  (independente do FreeRTOS) no núcleo que chama `USBHost.begin()`. O núcleo 0
  tem a interrupção de linha do DVI (~31,5 kHz) e não pode ganhar mais nenhum
  IRQ periódico; o núcleo 1 já absorve RCA_PCM e VIDEO_DEC.
- **Clock: NÃO é 240 nem 264 MHz em regime.** `display::begin()` reprograma o
  PLL do sistema para o valor exato que o modo de vídeo pede — 126 MHz para
  640×480@60 — e isso NÃO é múltiplo de 12 MHz, a regra que os próprios
  exemplos da Adafruit tratam como obrigatória (e travam o boot se não bater).
  Este port não trava o boot por isso; só mede e loga (`diag usb` mostra o
  clk_sys real). NÃO TESTADO: é o primeiro suspeito se o teclado/gamepad
  piscar, perder relatório ou travar no aparelho.
- **Dispositivos**: teclado HID boot, gamepad HID genérico (parser do report
  descriptor em `include/fj/UsbHidMap.h`, funções puras testadas em
  `tests/test_core.cpp`), DualShock4/DualSense por VID/PID com layout fixo.
  XInput (Xbox) fica de fora — precisaria do driver `tusb_xinput` (Ryzee119,
  MIT) integrado à parte; ver o item pendente no `PLANO_E_REVISAO.md`.
- Entra pela mesma fila do `InputManager` (`InputSource::USB`), pelo mesmo
  desenho do controle web: `fj/UsbHost.cpp` não inclui `InputManager.h` nem
  conhece `input` — recebe um `ActionHandler` (function pointer) que
  `appSetup()` fornece, do jeito que `FileTransfer` recebe `webCommand`.

---

## 4. Compilar

```sh
python3 -m venv .venv && .venv/bin/pip install platformio==6.1.19
.venv/bin/pio run                    # firmware
.venv/bin/pio run -d weather         # previsão autônoma
.venv/bin/pio run --target upload    # segure BOOT, aperte RESET: vira drive RP2350
```

Sem acesso ao registro do PlatformIO: clone as bibliotecas de `lib_deps` numa
pasta e crie `platformio_local.ini` (ignorado pelo git):

```ini
[env:fruitjam]
lib_deps =
lib_extra_dirs = /caminho/das/libs
```

---

## 5. Divisão do trabalho do port

Cada parte é dona dos arquivos listados. Mexer fora deles só com necessidade real,
e com o mínimo.

| Parte | Dono de |
|---|---|
| hal-audio | `src/fj/AudioOut.cpp`, `include/fj/AudioOut.h` |
| hal-net | `src/fj/Net.cpp`, `include/fj/Net.h`, `NetworkManager.*`, `ConfigurationPortal.*`, `SecretsManager.*`, `tests/test_portal.cpp`, `tests/portal_stubs/` |
| main-core | `src/main.cpp`: includes/globais do topo até `videoTick()`, HUD/OSD do player, `setAudioOutput`, energia, `drawControllerLabels`, `togglePlayerAudio`, `handleTouch`, diagnóstico, `setup()`, `loop()`; `InputManager.*`, `UiLogic.h`, `Config.h`, `LocalizationPTBR.h` |
| main-net-screens | `src/main.cpp`: radar (`stringAlias` … `pollAircraft`, `drawAirplane`, `drawRadar`), previsão (`weekdayFromIso` … `stopWeather`), `drawSettings`, `drawInfo` |
| main-media-screens | `src/main.cpp`: biblioteca (`normalizeSdPath` … `libraryProgramAt`), música (`isMusicTrack` … `musicTick`), transferência, `drawHomeClock`, `drawHome`, `drawLibrary`, fotos, padrão de teste, rádio, `handleNavigation` |
| net-headers | `RadioStream.h`, `FileTransfer.h`, `RtcClock.h` |
| gfx-headers | demais `include/*.h`, `sim/`, `tests/` (menos portal) |
| weather | `weather/` |
| docs | `README.md`, `AGENTS.md`, `PINOUT.md`, `PLANO_E_REVISAO.md`, `docs/`, `tools/`, `schematik-project.json` |

O `schematik-project.json` é ressincronizado **uma vez, no fim**, por quem
integra. Não rode `tools/sync_schematik.py` numa parte.
