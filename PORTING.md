# PORTING.md — contrato do port M5Stack Core2 → Adafruit Fruit Jam

Este arquivo é o contrato entre as partes do port. Quem mexe numa parte segue
as regras daqui; quem precisa mudar uma regra muda **este arquivo** junto.

Nada neste port foi testado no aparelho ainda. Tudo o que for escrito aqui
entra como **não testado no Fruit Jam** até alguém gravar e conferir.

---

## 1. O que muda de hardware

| Função | Core2 + RCA (original) | Fruit Jam (este fork) |
|---|---|---|
| CPU | ESP32 240 MHz, 2 núcleos Xtensa | RP2350B, 2× Cortex-M33, **264 MHz** (o DVHSTX sobe o relógio) |
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

---

## 6. Emuladores

O dono quer emuladores (Genesis, Master System/Game Gear, SNES, Apple IIe,
Macintosh) no mesmo Fruit Jam que a TV. A solução é um **lançador residente**
que mora nos primeiros 512 KB da flash (0x10000000 — é o que o bootrom do
RP2350 sempre executa primeiro) e decide, a cada boot, para onde saltar: a TV,
o menu de emuladores, ou de volta para o que já estava rodando.

O lançador é um **fork separado** de [fhoedemakers/pico-bootLoader](https://github.com/fhoedemakers/pico-bootLoader), GPLv3, em repositório
próprio (`eliel9012/pico-bootloader`, branch `fruitjam-retro-tv`). **Nada do
código dele foi copiado para cá** — o que segue é só a interface numérica
(dois watchdog scratch registers) que ele já publica para qualquer app que
rode sob ele; essa interface foi **reimplementada** neste repositório a partir
do zero, com comentários próprios.

### 6.1 Mapa de flash (Fruit Jam, 16 MB)

```
0x10000000  +-----------------------------+  <- bootrom sempre boota isto
            |   pico-bootLoader (fork)    |     512 KB, nunca apagado
            |                             |     pelo caminho de gravar
0x10080000  +-----------------------------+     um emulador.
            |   Partição de app           |
            |   (emuladores) — 11,5 MB    |
0x10C00000  +-----------------------------+  <- esta TV, env "fruitjam-launcher"
            |   Fruit Jam retro-TV        |     4 MB, nunca apagados pelo
            |                             |     caminho de gravar um emulador
0x11000000  +-----------------------------+
```

A reserva dos 4 MB é feita **inteiramente do lado do lançador**
(`CMakeLists.txt` dele, que reduz `FRENS_APP_SIZE` só para `HW_CONFIG==8`;
todo outro board continua com a partição de app original de 15,5 MB). Este
repositório não precisa saber desse número em tempo de build além do que já
está no seu próprio linker script (`boards/fruitjam_launcher_memmap.ld`) —
mas se algum dia a reserva mudar de tamanho, os dois lados têm de mudar juntos
(a TV nunca deve ultrapassar o que o lançador reservou, ou a próxima gravação
de um emulador corromperia o firmware da TV).

### 6.2 Dois builds, um firmware

| Env | Origem da flash | Quando usar |
|---|---|---|
| `fruitjam` (padrão) | `0x10000000` | Sem lançador. `pio run --target upload` grava e roda sozinho, como hoje. |
| `fruitjam-launcher` | `0x10C00000` | Com o lançador já gravado. `board_build.ldscript` aponta para `boards/fruitjam_launcher_memmap.ld` (uma cópia estática de `lib/rp2350/memmap_default.ld` do arduino-pico com `ORIGIN` trocado — ver comentário no topo do arquivo para o porquê de não dar para usar `board_build.*` para isso). |

O código-fonte é o mesmo nos dois; a diferença de comportamento (item
EMULADORES real vs. só explicativo) é `#ifdef FRUITJAM_LAUNCHER_BUILD`
(`-DFRUITJAM_LAUNCHER_BUILD`, só no env do lançador) em `src/main.cpp`, perto
de `enterEmulators()`.

Como o lançador salta por VTOR (desliga IRQs, aponta `SCB->VTOR` e pula para o
reset vector — nunca passa pelo bootrom do RP2350), a imagem da TV **não
precisa de um `IMAGE_DEF`/bloco de boot válido para o RP2350** nessa região; só
precisa que o `.uf2` grave nos endereços certos, o que o `ORIGIN` do linker
script já garante. `picotool uf2 convert -t elf` (usado pelo
`platform-raspberrypi`) lê os endereços do ELF, não valida metadado de boot —
grave o `.uf2` do env `fruitjam-launcher` do mesmo jeito que qualquer outro,
por BOOTSEL/drag-and-drop.

Esta TV **não usa** `EEPROM.h` nem `LittleFS` (configurações ficam no cartão
SD, ver `SecretsManager.cpp`), então os símbolos `_FS_start`/`_FS_end`/
`_EEPROM_start` do linker script do lançador ficam inertes, apontando para o
fim da região — não há filesystem interno para reservar espaço.

### 6.3 Protocolo TV ↔ lançador

Dois registradores scratch do watchdog (sobrevivem a qualquer reset que não
seja desligar e ligar a placa de verdade — POR; um `watchdog_reboot()` ou o
botão RUN não os apagam):

| Registrador | Quem escreve | Significado |
|---|---|---|
| `scratch[6] = 0xB007ED01` | lançador, antes de saltar para **qualquer** região (TV ou emulador) | "você foi lançado por mim" |
| `scratch[7] = 0xB007BACE` | quem quer voltar ao menu (um emulador retornando, OU esta TV pedindo o menu) | "mostre o menu de emuladores no próximo boot" |

`enterEmulators()` (`src/main.cpp`) escreve `scratch[7]` e chama
`rp2040.reboot()` (que por baixo é `watchdog_reboot(0, 0, 10)` — ver
`RP2040Support.h`). É **exatamente** o mesmo par que um emulador escreve para
"voltar ao menu": o lançador não precisa saber quem pediu, então nenhuma
mudança foi necessária do lado dele além de saber saltar para esta região.

Antes de reiniciar, `enterEmulators()` encerra tudo que disputa áudio/cartão/
rede, na mesma ordem de `requestPowerOff()` (rádio/tom/música do Weather
primeiro — AGENTS.md, armadilha 3 do repositório principal: eles seguram
`audioIdle`), espera o decode de vídeo (`waitVideoDecodeIdle()`), derruba o
portal e a rede, e muda a rota de áudio para mudo antes do reboot.

**O que não foi implementado:** pedir um emulador específico direto (só o
menu de emuladores). O lançador sabe, em tese, flashar e saltar direto para um
emulador escolhido por um arquivo no cartão (`/emu/launch.txt` foi cogitado),
mas isso exigiria integrar com a lógica de flash/seleção do lançador
(`g_emus[]`, `flashAndLaunch()`) de um jeito que não dava para validar sem uma
placa — ficou como próximo passo. Hoje EMULADORES sempre abre o menu; escolher
o emulador é manual, com os três botões, na tela do lançador.

**Voltar da tela de emuladores para a TV**: o menu do lançador (SELECT →
"Return to TV") ou um reset normal da placa voltam para a TV — o lançador, num
boot a frio, salta direto para ela em vez de mostrar o menu (é assim que ele
decide "reset normal = TV, não picker"). Ver o repositório do lançador para
os detalhes do lado dele.

### 6.4 Instalação

1. Grave o `.uf2` do lançador (`pico-bootLoader_AdafruitFruitJam_arm_piousb.uf2`,
   compilado do fork `fruitjam-retro-tv`) por BOOTSEL, uma vez.
2. Grave o `.uf2` desta TV compilado com `pio run -e fruitjam-launcher`
   (**não** o do env `fruitjam` — aquele espera rodar sozinho em
   `0x10000000` e seria apagado pelo lançador). O drag-and-drop do BOOTSEL
   grava nos endereços certos porque o linker script já os embute no `.uf2`.
3. Copie os `.uf2` de cada emulador para `/emu/8/` no cartão SD (placa 8 =
   Fruit Jam no lançador). O bundle pronto do PicoPlus para a placa 8 já serve
   para Genesis, Master System/Game Gear e SNES.
4. Mac e Apple IIe não vêm no bundle: compile
   [adafruit/pico-mac](https://github.com/adafruit/pico-mac) e
   [adafruit/reload-emulator](https://github.com/adafruit/reload-emulator)
   localmente, como apps do lançador (`-DBUILD_FOR_BOOTLOADER=ON`/`BootPartition.cmake`, que os
   relinka para `0x10080000` do jeito que o lançador espera). **Não baixe nem
   grave ROMs de terceiros aqui**: os scripts de build desses dois projetos
   baixam a ROM sozinhos durante a compilação — compile localmente com a
   própria ROM, na sua máquina, e copie só o `.uf2` resultante para
   `/emu/8/`.

### 6.5 O que já foi conferido, e o que só a placa responde

Conferido nesta sessão, sem placa: os dois envs (`fruitjam` e
`fruitjam-launcher`) compilam e linkam de ponta a ponta com o PlatformIO
local; `readelf -l` no ELF do `fruitjam-launcher` mostra o segmento de
código carregado em `0x10c00000` e o entry point em `0x10c03139` (bit Thumb
setado), batendo com `TV_BASE_ADDR` do lançador; `picotool info` no `.uf2`
final mostra a família `rp2350-arm-s` correta (só depois do
`tools/pio_fruitjam_launcher_uf2.py` — sem ele o `picotool uf2 convert`
padrão do `platform-raspberrypi` recusa o segmento de RAM, ver o próprio
arquivo para o porquê). `./tests/run.sh` e o simulador (`rca_1_INICIO.png`)
conferem a UI nova. Nada disso é o mesmo que rodar num Fruit Jam de verdade:

- Se o salto por VTOR do lançador para `0x10C00000` realmente entrega um
  estado limpo o bastante para o `setup()` do Arduino rodar (o lançador
  desliga IRQs e SysTick antes de saltar, mas não reinicializa periféricos
  como o DAC/PSRAM/DVI — o `board::begin()`/`display::begin()` desta TV
  precisam reconfigurar tudo do zero de qualquer forma, mas isso nunca foi
  testado depois de um salto por VTOR em vez de um boot normal).
- O protocolo de scratch registers em si: nunca houve um lançador rodando de
  verdade para escrever `scratch[6]` antes de saltar, nem para ler
  `scratch[7]` depois do reboot desta TV.
- Se 4 MB é suficiente para esta TV (o firmware atual, no env `fruitjam`
  padrão em 16 MB de flash, nunca foi medido de perto do limite; o uso
  observado no build local, ~400 KB de flash, sugere folga grande, mas isso
  não conta bibliotecas carregadas condicionalmente nem crescimento futuro).
- Se o BOOTSEL do RP2350 de fato aceita gravar um `.uf2` cujos blocos miram
  `0x10C00000` (a família `rp2350-arm-s` está correta no arquivo, mas
  ninguém testou o drag-and-drop numa placa real).
- **Mac e Apple IIe como apps do lançador**: não foi possível compilar
  `adafruit/pico-mac` nem `adafruit/reload-emulator` nesta sessão. O
  `fetch-rom-dsk.sh` do `pico-mac` baixa a ROM de `archive.org`, e o proxy
  de rede desta sessão recusou a conexão (HTTP 403) — o que, por acaso,
  é exatamente o comportamento certo dado que este repositório não deveria
  baixar ROMs de qualquer forma. A receita de integração (seção 6.4, item 4,
  copiada do próprio README do lançador) não foi validada compilando; só a
  leitura do `CMakeLists.txt` de cada projeto (que não tem `BUILD_FOR_BOOTLOADER`
  nativo — precisa ser adicionado à mão, como o README do lançador descreve
  para qualquer app RP2350 genérico).
