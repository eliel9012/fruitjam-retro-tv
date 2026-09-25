# AGENTS.md — como este firmware funciona

Guia para um assistente que vá trabalhar neste repositório sem contexto prévio.
Descreve a arquitetura, as restrições de hardware que não são negociáveis e as
armadilhas que já causaram bug aqui — ou no upstream, de onde este código veio. O
`README.md` documenta o **uso**; este arquivo documenta o **código**;
`PORTING.md` é o contrato do port e vence este arquivo quando os dois discordarem.

Idioma: comentários, strings de interface e mensagens de commit em português do
Brasil. Strings desenhadas na tela são ASCII sem acento (ver "Texto" abaixo).

> **Estado:** nada deste fork foi testado no aparelho ainda. Tudo o que está
> escrito aqui sobre o Fruit Jam é o que o código **pretende** fazer. Os números
> medidos que aparecem neste arquivo vêm do Core2 e estão marcados como tal. O
> roteiro de teste físico está em `PLANO_E_REVISAO.md`.

---

## 1. O que é

Fork do [m5-retro-tv](https://github.com/eliel9012/m5-retro-tv) (M5Stack Core2 +
módulo RCA) para o **Adafruit Fruit Jam** (RP2350B). Transforma a placa numa "TV
retrô": reproduz vídeo e som do cartão microSD pela saída **DVI** (640×480 a
60 Hz, com o quadro lógico de 320×240 dobrado nos dois eixos), e tem mais telas —
radar de tráfego aéreo, player de música, fotos, rádio pela internet, padrão de
teste e um clone do *Local Forecast* do Weather Channel dos anos 80.

Para ver numa TV de tubo, o caminho é um conversor HDMI→AV externo; a placa em si
só fala DVI.

| | Core2 + RCA (upstream) | Fruit Jam (este fork) |
|---|---|---|
| CPU | ESP32, 2 núcleos Xtensa, 240 MHz | RP2350B, 2× Cortex-M33, 240 MHz (o DVHSTX sobe o relógio; ver 2.2) |
| SRAM / PSRAM | ~320 KB / 4,5 MB | 520 KB / 8 MB (QSPI) |
| Vídeo | CVBS NTSC 320×240 | DVI 640×480@60, quadro lógico 320×240 |
| Tela local | LCD 320×240 + touch | **nenhuma** |
| Áudio | I2S1 → RCA, ou alto-falante interno | TLV320DAC3100 → fone P2 ou alto-falante |
| Cartão | SPI dividido com o LCD | SDIO próprio |
| Wi-Fi | nativo (lwIP + mbedTLS no ESP32) | ESP32-C6 com firmware NINA, por SPI1 |
| RTC / PMIC | BM8563 / AXP192 | **nenhum** / **nenhum** |
| Botões | 3 zonas de toque + PWR | 3 botões físicos |

Dois projetos PlatformIO independentes:

| Projeto | Raiz | O que é |
|---|---|---|
| principal | `/` | firmware completo, todas as telas |
| weather | `weather/` | só o Weather Channel, autocontido, para gravar sozinho |

O `weather/` **duplica de propósito** rotinas do principal (WAV, parse do tempo,
ticker). Ao mexer numa, verifique a outra — divergência silenciosa entre as duas
cópias já produziu bug no upstream.

---

## 2. Restrições de hardware — leia antes de propor qualquer coisa

Estas não são preferências de estilo. As que vieram do upstream já quebraram o
firmware lá; as novas são as que o port descobriu lendo o código das bibliotecas.

### 2.1 C++: o firmware é C++17, os headers compartilhados continuam C++11

O arduino-pico compila em **gnu++17**, então o firmware em si não tem mais a
trava do C++11 que o ESP32 tinha. Mas os headers que o upstream também usa
(`SafeArea.h`, `VcrFont.h`, `VcrOsd.h`, `ScreenFx.h`, `WeatherIcons.h`,
`PlaybackIO.h`, `Ascii.h`, `Id3.h` e companhia) **continuam em C++11** enquanto
for razoável: é isso que deixa este fork puxar correções do m5-retro-tv sem
reescrever nada. `constexpr` com laço ou variável local passa aqui e quebra lá.

Antes de entregar qualquer header compartilhado:

```sh
make -C sim cxx11
```

Código que só existe no fork (`include/fj/`, `src/fj/`) pode usar C++17 à vontade.

### 2.2 Memória: a SRAM ainda é o recurso que importa

| Recurso | Uso | Total |
|---|---|---|
| **SRAM** | **153.600 B do framebuffer do DVI** (320×240 RGB565), pilhas das tarefas | 520 KB |
| PSRAM | `MAX_JPEG` 128 KiB, buffers de HTTP, capas, sprites grandes, anel do rádio | 8 MB |

O framebuffer é alocado pelo DVHSTX no `display::begin()` e **é** o buffer do
canvas `tv` (um `LGFX_Sprite` com `setBuffer`). Desenhar no `tv` é desenhar na
tela: não há cópia nem flush. Buffer **simples**: dois quadros levariam 60% da
SRAM, e o preço é um eventual rasgo horizontal no vídeo (`display::waitVsync()`
existe para quem precisar trocar a tela inteira sem rasgo).

Sobram ~300 KB de SRAM, bem mais que no Core2. Ainda assim:

- Nada de alocar quadro inteiro extra (153.600 B, ou 76.800 B em RGB332) sem o
  chamador pedir. É por isso que o `crossfade` do `ScreenFx.h` exige um sprite
  **fornecido pelo chamador** e nunca aloca sozinho.
- Buffer grande vai para a PSRAM (`ps_malloc`), nunca para a pilha. Ver 2.3.
- **Não mova o framebuffer para a PSRAM.** O HSTX lê cada linha por DMA no prazo
  da varredura, e a PSRAM fica atrás de um cache XIP de 16 KB dividido com o
  código. Leitura sequencial lá é boa; acesso concorrente com prazo, não. Não foi
  medido aqui — é a mesma lição do CVBS do upstream, onde mover o quadro inteiro
  para a PSRAM quebrava o vídeo.

`include/fj/Platform.h` traduz a API de memória do ESP-IDF: `ps_malloc` →
`pmalloc`, `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` → PSRAM e qualquer outro
`MALLOC_CAP_*` → SRAM. Não existe "SRAM que serve ao DMA" separada: toda a SRAM
do RP2350 serve. Dois números do shim **não são verdade**, e quem imprime
diagnóstico precisa saber:

- `heap_caps_get_largest_free_block()` devolve **metade do livre** — o
  arduino-pico não expõe o maior bloco. É um palpite conservador.
- `ESP.getMinFreeHeap()` devolve o livre **atual**, não o mínimo histórico.

### 2.3 Pilha das tarefas: bytes no código, palavras no FreeRTOS

No ESP-IDF o tamanho de pilha do `xTaskCreate*` é em **bytes**. No FreeRTOS "de
verdade", que é o do arduino-pico, é em **palavras de 32 bits**. O
`xTaskCreatePinnedToCore` do `fj/Platform.h` recebe **bytes**, como o código
original, e divide por 4. Consequências:

- Use sempre o `xTaskCreatePinnedToCore` do shim, com o tamanho em bytes.
- Um `xTaskCreate` cru com `8192` reserva **32 KB**, não 8 KB. Com `2048` ele dá
  8 KB — e quem copiar esse número para o shim ganha 2 KB e estoura.
- `uxTaskGetStackHighWaterMark` devolve **palavras** aqui (no ESP-IDF, bytes).
  Multiplique por 4 antes de imprimir.
- `core` 0 ou 1 vira `xTaskCreateAffinitySet`; `tskNO_AFFINITY` (−1) deixa o
  escalonador escolher.

| Tarefa | Pilha (bytes) | Observação |
|---|---:|---|
| `APP` (`setup()`/`loop()`) | 16384 | a `CORE0` do arduino-pico tem 4 KB fixos; o `setup()` dela só cria a `APP`, no núcleo 0 |
| `VIDEO_DEC` | 8192 | decode MJPEG no núcleo 1, prioridade 2 (abaixo do áudio) |
| `WEATHER_HTTP` | 8192 | uma consulta, publica e morre |
| `RADAR_HTTPS` | 8192 | idem |
| `RADIO_ICY` | 8192 | leitura do stream do rádio |
| `RCA_PCM` (áudio) | 4096 | o nome ficou do upstream; alimenta o `audioout::write()` |

O TLS agora roda **dentro do ESP32-C6**: o RP2350 não faz handshake, e some o
problema de pilha do mbedTLS que o upstream teve duas vezes (`char buf[4096]` no
`weatherFetch`, `MjpegReader` local de 4104 bytes no benchmark). A regra continua
de pé por outro motivo — pilha é SRAM: **array local de KB é bug**. Buffers
grandes vão para a PSRAM ou reusam um global.

A interrupção de linha do DVI fica no núcleo que chamou `display::begin()` (o do
`setup()`); por isso `display::begin()` vem **antes** de criar qualquer tarefa.

### 2.4 Pinos e barramentos

Ver `PINOUT.md`. O que mais importa:

- **GPIO 22 é o reset do DAC *e* do ESP32-C6, juntos.** O WiFiNINA pulsa esse
  pino ao acordar o rádio, e o pulso apaga a configuração do TLV320. A ordem do
  boot é **fixa**:

  ```
  board::begin()        reset limpo dos dois periféricos, botões
  display::begin()      DVI no ar
  storage::begin()      SD
  net::beginRadio()     acorda o ESP32-C6 — pulsa o GPIO 22
  audioout::begin()     só agora configura o DAC
  xTaskCreate...        tarefas
  ```

  Configurar o DAC antes do rádio dá áudio mudo **sem erro nenhum**. Qualquer
  código que reinicie o rádio depois do boot (reconectar, reabrir o portal) tem
  de reconfigurar o DAC em seguida.

- **O SPI1 é do ESP32-C6, e só dele.** `loop()`, `WEATHER_HTTP`, `RADAR_HTTPS` e
  `RADIO_ICY` disputam o mesmo coprocessador. **Toda** chamada WiFiNINA roda com
  `net::Lock` tomado (mutex recursivo, RAII). Uma transação cortada no meio trava
  o NINA até o próximo reset. Laço de leitura toma o lock **por bloco lido**,
  nunca pela duração inteira de um stream — senão o rádio mata de fome o resto.
- **O cartão tem barramento próprio** (SDIO, GPIO 34..39). Não há mais disputa
  SD × LCD. O `sdMutex` continua, para `loop()` × tarefa de áudio.
- **O áudio não troca de dono.** No Core2, RCA e alto-falante dividiam o I2S1 e
  o firmware desmontava um driver para montar o outro. Aqui o DAC tem as duas
  saídas: `audioout::setRoute()` muda registrador, e só a tarefa de áudio chama
  `audioout::write()`.
- **Botão 1 (GPIO 0) é também o BOOT.** Segurá-lo durante um reset entra no
  modo de gravação, não no firmware.

### 2.5 `FILE_WRITE` anexa

No arduino-pico, `FILE_WRITE` é `O_RDWR | O_CREAT | O_APPEND`. No ESP32 era `"w"`,
que trunca. Gravar um JSON com `FILE_WRITE` aqui **anexa** ao arquivo velho e
produz um JSON inválido — que só aparece no boot seguinte, quando o parse falha.
Para sobrescrever, `SD.open(path, "w")`. Vale para `settings.json`,
`secrets.json`, os `.bak` do `SafeStorage.h` e qualquer arquivo de estado.

### 2.6 Área segura: o monitor não corta, o tubo corta

Um monitor DVI mostra o raster inteiro: não há overscan. Uma TV de tubo, ligada
por um conversor HDMI→AV, esconde cerca de 7% de cada borda. O firmware tem de
ficar bem nos dois.

`include/SafeArea.h` define a geometria, compartilhada entre firmware, simulador
e upstream:

```
crt::W, crt::H              320x240
crt::SAFE_L/T/R/B           24, 18, 296, 222
crt::HEAD_Y/HEAD_RULE_Y     cabeçalho padrão das telas
crt::BAR_Y/BAR_H            barra de legendas dos três botões
crt::TICKER_Y/TICKER_H      faixa do ticker da previsão
```

Regra, a mesma do upstream: **texto, réguas e faixas ficam dentro da caixa
segura; só o fundo e os marquees sangram até a borda do raster.** Nada de tarja
preta em volta. Num monitor, a margem aparece como fundo da própria tela — não
como moldura —, e é por isso que o fundo precisa sangrar.

A barra de legendas dos botões (`crt::BAR_*`) ficou mais importante: sem LCD, é
a **única** indicação do que cada botão faz.

### 2.7 Sinal de vídeo: DVI, não CVBS

- **640×480 a 60 Hz**, gerado pelo HSTX do RP2350 com a biblioteca
  Adafruit-DVI-HSTX (driver `pimoroni::DVHSTX`, usado direto porque o wrapper
  esconde o `wait_for_vsync()`). O quadro lógico é **320×240 RGB565**, dobrado
  nos dois eixos pelo hardware. Todo o firmware (área segura, fontes bitmap, OSD,
  previsão) foi desenhado para 320×240; 640×480 em RGB565 seriam 614 KB, mais que
  a SRAM inteira.
- **Sem áudio no cabo.** É DVI, não HDMI com ilhas de dados. O som sai pelo fone
  P2 ou pelo alto-falante.
- **Painel sempre RGB565.** O ajuste `CONFIGURAÇÕES → CORES` do upstream (RGB332
  × RGB565) e o `applyColorDepth()` saíram. `settings.color16` continua sendo
  lido do JSON, e é ignorado, para o mesmo cartão servir nos dois aparelhos.
- O vídeo continua **centralizado** no 320×240 por `jpegDraw()`, sem escala.
  320×240 (tela cheia) agora é o padrão do `prepare_video.py`.
- **TV de tubo:** o conversor HDMI→AV é quem gera o composto. Configure-o em
  **NTSC** — TVs brasileiras PAL-M costumam aceitar NTSC; PAL puro sai em preto e
  branco ou rolando. A história do PAL-M do M5GFX (908 amostras por linha contra
  909,02) não se aplica mais: o firmware não gera composto.

#### Ordem de bytes do RGB565 — o maior risco do port

O `LGFX_Sprite` de 16 bits guarda RGB565 com os **bytes trocados** (big-endian,
herança do SPI dos painéis). O DVHSTX espera **little-endian**. Converter 76.800
pixels por quadro custaria tempo em todo quadro; em vez disso,
`configureSwappedRgb565()` em `src/fj/Display.cpp` reprograma o registrador
`expand_tmds` do HSTX para que cada pista TMDS leia os campos já trocados:

- vermelho já está nos bits 7..3 → rotação 0;
- azul está nos bits 12..8 → rotação 5;
- verde está partido em G5..G3 (bits 2..0) e G2..G0 (bits 15..13). O truque é
  que o DVHSTX, para dobrar a largura, manda cada pixel como `pixel * 0x10001` —
  a cópia na metade alta da palavra põe G2..G0 nos bits 31..29, e uma rotação de
  27 (à esquerda por 5) junta as duas partes.

Consequências:

- O truque do verde **depende da duplicação horizontal**. Vale só para o modo
  320×240 (`h_repeat_shift = 1`). Mudar o modo de vídeo quebra o verde.
- JPEGDEC tem de emitir **`RGB565_BIG_ENDIAN`**, que é a ordem que o sprite
  guarda. `jpegDraw()` escreve direto no `tv`.
- Cores passadas pela API do LovyanGFX saem certas; quem escreve **direto** no
  buffer (`tv.getBuffer()`) precisa escrever com os bytes trocados.
- **Nada disso foi visto numa tela ainda.** Se as cores saírem erradas, o
  primeiro suspeito é este registrador, e o `PLANO_E_REVISAO.md` descreve como
  diagnosticar pelo sintoma.

### 2.8 Texto é ASCII

As fontes bitmap (LovyanGFX `Font0/2/4` e a `VcrFont` própria) só têm ASCII. Em
UTF-8 uma letra acentuada são **dois bytes**, ambos sem glifo, então `"Não"` vira
`"N  o"` — dois buracos, não um.

**Todo texto que venha de fora — nome de pasta do cartão, `meta.json`, tag ID3,
resposta de API, título do stream do rádio — passa por `ascii::normalize` ou
`ascii::normalizeUpper` (`include/Ascii.h`) antes de ser desenhado.** O
`VcrOsd.h` normaliza sozinho; os outros caminhos normalizam no chamador.

### 2.9 O que não existe mais

- **LCD e touch.** Todo `for (auto *d : {&tv, &M5.Display})` virou desenho só no
  `tv`. HUD de 1 Hz, pôster, seta de voltar do LCD e barra de toque foram
  **removidos**, não emulados. Não crie um `M5.Display` falso.
- **AXP192.** "DESLIGAR" é *soft-off*: apaga a tela, cala o áudio, apaga os
  NeoPixels e dorme até um botão; o botão reinicia (`rp2040.reboot()`). O item do
  menu continua se chamando DESLIGAR. `setBacklight()` saiu; o `BurnIn.h`
  protege o monitor escurecendo o canvas.
- **RTC.** A hora vem só do NTP do ESP32-C6 (`net::ntpEpoch()` +
  `settimeofday()`, em `RtcClock.h`). Sem Wi-Fi a hora é desconhecida e a
  interface mostra `--:--`. Fuso `<-03>3`, como no upstream.
- **ca.pem.** O NINA valida TLS com o bundle de raízes dele; o
  `/M5RETRO/config/ca.pem` do cartão deixa de ser usado. Servidor com CA privada
  ou autoassinada não valida.

---

## 3. Arquitetura

### 3.1 Mapa dos arquivos

```
src/main.cpp          Máquina de estados da interface, player, radar, previsão,
                      player de música, fotos, rádio, console de diagnóstico.
                      É onde quase tudo acontece, e onde o merge com o upstream
                      mais conflita.
src/ConfigurationPortal.cpp   portal Wi-Fi em modo AP (WiFiServer, sem WebServer)
src/SecretsManager.cpp        leitura/escrita de settings.json e secrets.json
src/NetworkManager.cpp        conexão Wi-Fi
src/InputManager.cpp          três botões, com auto-repeat

include/fj/Platform.h   equivalências ESP-IDF -> RP2350 (memória, relógio, tarefas)
include/fj/Gfx.h        LovyanGFX + GfxTarget; ÚNICO include gráfico dos headers
include/fj/Display.h    display::begin/waitVsync e o canvas global `tv`
include/fj/Board.h      pinos, board::begin, botões, detecção do cartão
include/fj/Storage.h    SD em SDIO
include/fj/AudioOut.h   TLV320DAC3100: rota, volume, taxa, write()
include/fj/Net.h        ESP32-C6/WiFiNINA: net::Lock, httpGet, ntpEpoch
src/fj/*.cpp            implementações das acima

include/PlaybackIO.h    leitor de MJPEG concatenado e de WAV PCM
include/Id3.h           tags ID3v1/v2, inclusive capa embutida (APIC)
include/Ascii.h         normalização de acentos para ASCII
include/SafeStorage.h   gravação com .bak, recuperada no próximo boot
include/UiLogic.h       enum UiState e funções puras testáveis
include/SafeArea.h      geometria da tela (área segura)
include/LocalizationPTBR.h    strings da interface

include/VcrFont.h       fonte bitmap 12x16 do OSD
include/VcrOsd.h        OSD estilo videocassete Sony/Semp dos anos 90
include/WeatherIcons.h  ícones de condição estilo Weather Star 4000
include/ScreenFx.h      transições: fade, crossfade, slide, wipe
include/RadioStream.h   cliente ICY do rádio (anel na PSRAM)
include/FileTransfer.h  servidor HTTP de upload para o cartão
include/RtcClock.h      hora do sistema a partir do NTP

sim/                    simulador SDL das telas (ver seção 6)
tools/prepare_video.py  converte MP4 em MJPEG+WAV com FFmpeg
tools/prepare_photos.py converte fotos em JPEG baseline 320x240
tools/sync_schematik.py sincroniza schematik-project.json
tests/                  testes nativos com ASan/UBSan
```

Renomeações do port, para ler diffs contra o upstream: `rca` → **`tv`**,
`M5GFX *` → **`GfxTarget *`**, `#include <M5GFX.h>` → **`#include "fj/Gfx.h"`**.

### 3.2 Tarefas e quem roda onde

| Contexto | O que faz |
|---|---|
| `loop()` (tarefa `APP`, núcleo 0) | interface, leitura do quadro no cartão, desenho, diagnóstico serial |
| `VIDEO_DEC` (núcleo 1, prio 2) | decodifica o quadro MJPEG direto no framebuffer, a pedido do `videoTick()` |
| `RCA_PCM` | lê PCM do cartão e entrega ao `audioout::write()`, ritmado pelo relógio de amostras |
| `WEATHER_HTTP` / `RADAR_HTTPS` (prio 0) | uma consulta HTTP(S) via `net::httpGet`, publica e morre |
| `RADIO_ICY` | lê o stream do rádio, bloco a bloco, com `net::Lock` por bloco |
| interrupção do DVI | alimenta o HSTX linha a linha, no núcleo do `setup()` |

**Todo desenho acontece no `loop()`**, com uma exceção: a `VIDEO_DEC` escreve o
quadro do filme no framebuffer. Enquanto um decode está em voo o `loop()` não
desenha no `tv` nem usa o `jpeg`; quem precisa desenhar durante o vídeo chama
`waitVideoDecodeIdle()` antes (ver 3.5). As tarefas de rede nunca desenham: elas
escrevem num buffer e publicam por `std::atomic`.

### 3.3 Sincronização

**`sdMutex`** protege o cartão microSD, disputado entre o `loop()` e a tarefa de
áudio. Quem lê o cartão toma o mutex. Padrão: tomar e devolver **por operação**,
nunca segurar durante um trabalho longo — segurar mata o áudio de fome.

**`net::Lock`** protege o SPI1 do ESP32-C6. Mesmo padrão: por transação, nunca
por stream. `net::httpGet` segura o lock pela requisição inteira, e por isso
deve ser curta (resposta de API, não download).

**Buffer duplo** para os dados de rede (radar e previsão): a tarefa escreve no
índice não exibido e só então publica o índice novo por `store()`. O leitor nunca
vê estado pela metade.

**Handoff do radar**: a tarefa aloca um resultado e o entrega por
`radarResponse.store(ptr)`; o `loop()` drena com `exchange(nullptr)` e é dono da
liberação em todos os caminhos.

**`millis()` dá a volta em ~49 dias.** Comparação correta é `now - last >= K`
(aritmética sem sinal). Existe o helper `timeReached()`. Nunca compare
`millis() > prazo` cru.

### 3.4 Máquina de estados da interface

```
BOOT → HOME → { VIDEO_LIBRARY → VIDEO_PLAYBACK
                MUSIC_BROWSER  → MUSIC_NOW_PLAYING
                PHOTO_SHOW
                RADIO
                WEATHER
                AIRCRAFT_RADAR
                TEST_PATTERN
                FILE_TRANSFER
                SETTINGS → SETUP_PORTAL
                SYSTEM_INFO
                EMULATORS }
              ERROR_SCREEN (de qualquer lugar)
```

O menu inicial tem 12 itens em **duas colunas** de 6 e 6 (EMULADORES, entre
SISTEMA e DESLIGAR, é deste fork Fruit Jam — ver PORTING.md, seção
"Emuladores"). A ordem dos rótulos em `drawHome()` e o destino em
`homeTarget()` têm de andar juntos; um `static_assert` prende a lista ao
`HOME_ITEM_COUNT`. (No upstream havia um terceiro, `homeHit()`, a grade de
toque; sem touch ele não tem mais chamador.)

O enum está em `include/UiLogic.h`. O `loop()` despacha por `state`.

**Botões** (`InputManager`, lendo `board::buttonDown(0..2)`): A = esquerda /
anterior, B = selecionar, C = direita / próximo; segurar B 0,7 s = voltar, 1,5 s
= início. O que era o botão PWR do Core2 (atalho para o início) virou **segurar A
e C juntos**.

**Armadilha conhecida:** várias funções de desenho guardam estado em `static`
local. Ao entrar numa tela, zere o que precisa ser zerado — estado herdado da
visita anterior já deixou a tela da previsão em branco permanentemente.
`startWeather()` é o exemplo de como fazer certo.

### 3.5 Caminho do vídeo

```
cartão (SDIO) → MjpegReader.next() → jpegBuffer (PSRAM, 128 KiB)
              → [núcleo 1, VIDEO_DEC]
                JPEGDEC.decode()   → jpegDraw() por bloco de MCU, RGB565_BIG_ENDIAN
              → tv (LGFX_Sprite)   = framebuffer do DVI (SRAM)
              → HSTX, linha a linha → GPIO 12..19 → monitor
```

O relógio é o **PCM entregue**, não `millis()`: `videoTick()` calcula o quadro
alvo a partir de `samplesPlayed` e pula quadros atrasados **sem decodificar**
(passando `render = false`).

O `loop()` lê o quadro (sdMutex) e entrega o decode à `VIDEO_DEC` por
`submitVideoFrame()`; o `videoTick()` seguinte colhe o resultado
(`collectVideoFrame()`), publica `renderedSeq`/`lastJpegUsed` e **devolve a vez
antes de submeter o próximo**, para o OSD e a legenda pintarem sobre o quadro
limpo. Com um framebuffer só, a regra é: **nada desenha no `tv` com um decode em
voo** — `drawPlaybackOsd`, `redrawCurrentFrame`, `stopProgram`, a vinheta do modo
canal e o desligamento chamam `waitVideoDecodeIdle()`, que espera e já colhe. O
primeiro quadro e o descarte (`render = false`) continuam síncronos no `loop()`.
Estudo de desempenho: `docs/codecs-video-fruitjam.md`.

Por isso há dois contadores diferentes, e confundi-los já causou bug:

- `videoFrameIndex` — posição no fluxo. Avança **também** nos quadros descartados.
- `renderedSeq` — quadros efetivamente desenhados. É o que o OSD usa.

### 3.6 Caminho do áudio

```
cartão (WAV) ou libhelix (MP3/rádio) → tarefa RCA_PCM → audioout::write()
  → I2S por PIO (GPIO 24..27) → TLV320DAC3100 → fone P2 | alto-falante | mudo
```

Compatibilidade com o `settings.json` do upstream: o enum
`AudioOutput { RCA, INTERNAL, MUTED }` e os valores `"rca"`, `"interno"`,
`"mudo"` **ficam**. O significado mudou: `RCA` → `Route::HEADPHONE` (o fone P2,
que vai para a entrada de áudio da TV) e `INTERNAL` → `Route::SPEAKER`. Na tela:
"TV (P2)" e "ALTO-FALANTE". Toda a API `i2s_*` do ESP-IDF e o `M5.Speaker`
saíram.

### 3.7 OSD do player

`include/VcrOsd.h`. Imita o OSD de um videocassete Sony/Semp dos anos 90: texto
flutuando **sobre a imagem, sem tarja de fundo**, com contorno preto por glifo,
símbolo de transporte desenhado como forma (nunca a palavra "PLAY") e contador de
fita em dígitos grandes.

Como não há fundo próprio, **cada repintura precisa partir de imagem limpa**:

- com o filme rodando: logo depois de um quadro novo (o quadro apaga o OSD anterior);
- pausado: `redrawCurrentFrame()` redecodifica o último JPEG, que continua
  inteiro no `jpegBuffer` — é para isso que existe `lastJpegUsed`;
- ao esconder: `clearOsdLetterbox()` apaga **só o que fica fora do retângulo do
  vídeo**, para não piscar uma tarja preta sobre o filme.

O contorno vem da **dilatação** da máscara do glifo, num passe só: 2,15× os
pixels de um glifo simples, contra 9,00× de redesenhar a string deslocada oito
vezes (medido no upstream).

### 3.8 Tela da previsão

Duas páginas — condições atuais e previsão de três dias — alternadas a cada 10 s
com as transições do Weather Star 4000: **cortina vertical** quando chegam dados
novos, **deslizamento horizontal** na troca de página.

Os pintores têm assinatura `(dst, ox, oy, user)`: o deslizamento desenha as duas
páginas deslocadas no mesmo quadro. Com deslocamento zero é o desenho normal.

**Os esmaecimentos usam dithering ordenado de Bayer, não mistura por alfa.** O
framebuffer não tem canal alfa e não se aloca um segundo quadro inteiro sem o
chamador pedir. O resultado grosseiro é, por acaso, exatamente o que parece
autêntico para a época.

`fadeOut`, `slide` e `wipe` **não precisam de buffer**. `crossfade` e `fadeIn`
exigem um sprite 320×240 **fornecido pelo chamador** (a profundidade esperada
está no `ScreenFx.h`); passando `nullptr` eles degradam para corte seco e
devolvem `false`.

Fonte dos dados: **Open-Meteo** (`api.open-meteo.com`), pública e sem chave, com
resposta de ~800 bytes, buscada por `net::httpGet` num buffer na PSRAM. Os
ícones são escolhidos pelo **código WMO**, não por comparação de string.

---

## 4. Compilar e testar

```sh
python3 -m venv .venv && .venv/bin/pip install platformio==6.1.19

.venv/bin/pio run                 # firmware principal (env fruitjam)
.venv/bin/pio run -d weather      # projeto da previsão
.venv/bin/pio run --target upload # segure BOOT, aperte RESET: vira o drive RP2350

./tests/run.sh                    # testes nativos, ASan + UBSan
./tests/media.sh                  # exige FFmpeg/FFprobe

make -C sim                       # simulador de telas
make -C sim cxx11                 # headers compartilhados em C++11
make -C sim probes                # bancadas isoladas
./sim/build/m5sim --png <dir>     # grava as telas em PNG, headless

python3 tools/sync_schematik.py   # obrigatório após mudar fontes
python3 tools/sync_schematik.py --check
```

A plataforma é a comunitária do Max Gerhardt
(`maxgerhardt/platform-raspberrypi`) com o core do Earle Philhower
(arduino-pico): a plataforma oficial `raspberrypi` do PlatformIO não tem RP2350.
`board_build.f_cpu` fica em 150 MHz no `platformio.ini` porque a biblioteca do
DVHSTX recusa compilar com outro valor. Ela mesma sobe o relógio de verdade
depois: o `#error` de compilação dela fala em 264 MHz, mas a conta do
`clock_configure` no commit fixado dá **240 MHz** (PLL USB 480 MHz / 2) —
mensagem de erro desatualizada da própria lib, não do nosso código; ver AGENTS
2.2 abaixo. As bibliotecas estão fixadas por commit.

Sem acesso ao registro do PlatformIO: clone as bibliotecas de `lib_deps` numa
pasta e crie `platformio_local.ini` (ignorado pelo git):

```ini
[env:fruitjam]
lib_deps =
lib_extra_dirs = /caminho/das/libs
```

O artefato gravável é `.pio/build/fruitjam/firmware.uf2`: com a placa em modo de
gravação, basta copiá-lo para o drive `RP2350`.

`./tests/run.sh` falha se o `schematik-project.json` estiver dessincronizado.
Rode o `sync_schematik.py` antes de commitar.

---

## 5. Diagnóstico pelo serial (115200 baud, USB CDC)

```
diag status        heap, PSRAM, FPS, quadros descartados, tempos de decode
diag mem           memória em detalhe (lembre: maior bloco livre é palpite)
diag fb            profundidade e tamanho do framebuffer
diag cores         cores RGB565 distintas no framebuffer agora
diag scan          lê linhas de volta: onde a tinta caiu de fato
diag colors        confere o caminho RGB565 dos blocos JPEG
diag bench         mede leitura do cartão, decode e blit em microssegundos
diag time          origem e valor da hora do sistema
diag radio         estado do rádio pela internet
diag play/pause/resume/stop/home/back/next/previous/select/left/right
diag radar/weather/music/emulators
diag audio toggle|tv|internal|mute
```

`diag emulators` chama `enterEmulators()` direto (mesmo caminho do item
EMULADORES do menu inicial). No build `fruitjam-launcher` isso reinicia a
placa de verdade -- é o jeito mais rápido de testar o protocolo com o
lançador sem navegar até lá pelos três botões.

A lista exata é a do tratador serial em `src/main.cpp`; esta aqui é o que veio do
upstream menos o que dependia do LCD (`diag backlight`).

`diag bench` responde a pergunta que nenhum simulador de PC responde: até onde
**este** aparelho sustenta o vídeo. Aceita pasta e contagem de quadros:

```
diag bench /M5RETRO/videos/meu-filme 400
```

Os números de referência do upstream (Core2, 240×160: decode 25,3 ms, blit 12,3
ms, 23,5 fps em RGB565) **não valem aqui**: o blit no Fruit Jam é escrever no
próprio framebuffer, sem CVBS no meio. Nada foi medido no Fruit Jam ainda.

---

## 6. O simulador de telas

`sim/` roda o **LovyanGFX com o backend SDL** no desktop — a mesma biblioteca
gráfica do firmware, que desenha no `LGFX_Sprite` do DVI. Serve para conferir
diagramação — sobretudo se algo escapou da área segura — sem ligar a placa.

**O que ele NÃO simula:** Wi-Fi/NINA, cartão SD, DAC e I2S, o HSTX e a troca de
bytes do codificador TMDS, decodificação MJPEG, FreeRTOS. **E não simula
desempenho** — ele roda na CPU do seu computador, então qualquer FPS medido ali é
ficção. Para desempenho, `diag bench` no aparelho.

Em particular: **o simulador não prova que as cores estão certas no monitor.**
Ele desenha RGB565 do jeito que o SDL entende; a reinterpretação do HSTX só
existe no aparelho.

O modo `--png` grava as telas sem abrir janela, o que permite conferir área
segura pixel a pixel em CI. As imagens de `docs/screens/` saem daí.

**Cuidado:** `sim/src/main.cpp` é um **espelho manual** das telas do firmware,
não o mesmo código. Divergência entre os dois é bug — o simulador passa a
esconder problemas reais. Onde dá, prefira o simulador **chamar o código de
verdade**: a tela do player já chama `vcr::draw()` diretamente, e é assim que
deve ser.

`sim/probes/` são bancadas isoladas: cada `.cpp` ali vira um binário próprio,
para desenvolver um componente gráfico sem mexer no simulador principal.

---

## 7. Armadilhas que já morderam

Lista curta do que já quebrou, para não repetir. As de 1 a 14 vieram do
upstream e continuam valendo; as de 15 em diante são do port.

1. **`constexpr` com laço num header compartilhado** — passa aqui (C++17),
   quebra no upstream (C++11). `make -C sim cxx11` pega.
2. **Array de KB na pilha de uma tarefa** — estouro de pilha. No upstream foi
   por causa do mbedTLS; aqui a pilha é SRAM e o motivo continua.
3. **`stopProgram()` esperando `audioIdle`** — a música em loop do Weather segura
   o áudio; se a flag não cair antes da espera, o aparelho **congela para sempre**.
4. **Retentativa sem backoff** — um portão que só avança no sucesso vira laço
   apertado criando tarefas HTTP quando a rede cai.
5. **Confundir `videoFrameIndex` com quadros renderizados** — o OSD acredita ter
   imagem nova embaixo e pinta texto sobre texto.
6. **Estado `static` de função não zerado ao entrar na tela** — a tela herda a
   visita anterior.
7. **Desenhar só uma vez algo que as transições repintam** — a régua do ticker
   sumia na primeira transição.
8. **Duas constantes descrevendo a mesma geometria** — `SafeArea.h` dizia que o
   OSD tinha 38 px quando ele tem 74.
9. **Texto externo sem normalizar** — acento vira buraco duplo.
10. **Sprite com profundidade diferente do painel** — cor errada em silêncio, e
    memória dobrada.
11. **Passar cor como `uint32_t` para o LovyanGFX** — o formato é escolhido pelo
    **tipo do argumento**, não pelo valor (`lgfx/v1/misc/colortype.hpp`):
    `uint8_t` vira RGB332, `uint16_t`/`int16_t`/`int32_t` viram RGB565 e
    `uint32_t` vira **RGB888**. Um `(uint32_t)0xBDF7` é lido como `0x00BDF7`, ou
    seja R=0, G=189, B=247: o cinza das barras SMPTE saía esverdeado. Como
    literal `int` funciona (é `int32_t`), o erro só aparece em quem escreve o
    cast — e não dá aviso nenhum. Passe `uint16_t`.
12. **`>> 8` para escalar amostra com sinal** — o deslocamento arredonda para
    −infinito, a divisão trunca em direção ao zero. No gerador do tom de 1 kHz,
    220 das 441 amostras são negativas e cada uma perdia 1 LSB: −10900 de offset
    DC em um segundo, mandado para o amplificador. Em ponto fixo com sinal, use
    `/ 256` ou arredonde de propósito.
13. **Binário de teste sobrevivendo ao fonte apagado** — um `probe_*` em
    `sim/build/` continua executável depois que o `.cpp` sai, e acusa bugs já
    corrigidos como se fossem regressões. Confira se o fonte ainda existe.
14. **Probe que escreve PNG rodado da raiz do repositório** — o caminho é
    relativo a `sim/`, o `fopen` devolve NULL, e um `fprintf` em cima disso é
    segfault sem nenhuma saída. Rode de dentro de `sim/`, e confira o `fopen`.
15. **`FILE_WRITE` anexa** — no arduino-pico é `O_APPEND`. Sobrescrever JSON com
    ele gera arquivo inválido que só estoura no boot seguinte. Use `"w"`.
16. **Configurar o DAC antes de acordar o rádio** — o pulso do GPIO 22 apaga o
    TLV320. Áudio mudo, nenhum erro. Ordem: rádio, depois DAC.
17. **Tamanho de pilha em palavras × bytes** — o shim recebe bytes; o
    `xTaskCreate` cru e o `uxTaskGetStackHighWaterMark` falam palavras. Um fator
    4 para mais desperdiça SRAM; para menos, estoura.
18. **Chamada WiFiNINA sem `net::Lock`** — duas tarefas no SPI1 ao mesmo tempo
    cortam uma transação no meio, e o NINA trava até o reset. O sintoma aparece
    longe da causa (a tela da previsão para de atualizar horas depois).
19. **Segurar `net::Lock` por um stream inteiro** — o rádio, ou um download,
    deixa o resto da rede sem ar. Lock por bloco.
20. **Pixel escrito direto no buffer com a ordem de bytes errada** — o sprite e o
    HSTX combinam RGB565 big-endian. JPEGDEC em `RGB565_LITTLE_ENDIAN`, ou um
    `memcpy` de pixels nativos no `tv.getBuffer()`, sai com cor trocada.
21. **Mudar o modo do DVHSTX** — o verde depende da duplicação horizontal do
    320×240. Outro modo, outro `expand_tmds`.
22. **Confiar em `heap_caps_get_largest_free_block()`** — é metade do livre, não
    o maior bloco. Trate a falha do `malloc` sempre.
23. **Renomear os valores do `settings.json`** — `"rca"`, `"interno"`, `"mudo"`
    e `color16` ficam como estão, para o mesmo cartão servir no Core2 e no Fruit
    Jam. Muda o texto da tela, não o JSON. Pela mesma razão as pastas continuam
    em `/M5RETRO/`.
24. **Emular o LCD** — código que só desenhava no LCD sai. Um `M5.Display` falso
    esconde a remoção e reaparece como conflito em todo merge do upstream.

---

## 8. Sincronizar com o upstream

O fork partiu do commit `21aa210` do m5-retro-tv. Para puxar correções de lá:

```sh
git remote add upstream https://github.com/eliel9012/m5-retro-tv
git fetch upstream
git log --oneline HEAD..upstream/main      # o que chegou
git merge upstream/main                    # ou: git cherry-pick <commit>
```

Prefira `cherry-pick` para correções isoladas; `merge` quando vier muita coisa.

O que costuma conflitar, e como resolver:

| Arquivo | Por quê | Como |
|---|---|---|
| `src/main.cpp` | `rca` → `tv`, LCD/touch/AXP192/RTC removidos, rede em WiFiNINA | reaplique a correção à mão; troque `rca.` por `tv.` e descarte o que só desenhava no LCD |
| headers com `#include "fj/Gfx.h"` | o upstream inclui `<M5GFX.h>` e usa `M5GFX *` | mantenha `fj/Gfx.h` e `GfxTarget *`; o resto do diff costuma entrar limpo |
| `RadioStream.h`, `FileTransfer.h`, `RtcClock.h` | rede e relógio reescritos para NINA/NTP | leia a correção e reimplemente; raramente entra por merge |
| `platformio.ini` | outra plataforma, outro ambiente | fique com o do fork; bibliotecas novas do upstream entram à mão, fixadas por commit |
| `weather/` | idem, no projeto autônomo | idem |
| `README.md`, `AGENTS.md`, `PINOUT.md`, `PLANO_E_REVISAO.md` | documentam outro hardware | fique com o do fork; traga só o que for de uso (formato de mídia, telas novas) |
| `schematik-project.json` | gerado | não resolva à mão: rode `tools/sync_schematik.py` |

Depois de qualquer sincronização:

```sh
make -C sim cxx11 && ./tests/run.sh && .venv/bin/pio run && .venv/bin/pio run -d weather
```

Correção feita aqui que também vale lá (um header compartilhado, um bug de
lógica) deve ir para o upstream como PR próprio, sem nada de `fj/` — é isso que
mantém os headers compartilhados de fato compartilhados.

---

## 9. Ao propor mudanças

- Comentários em português do Brasil, explicando **o porquê**, não o óbvio. O
  repositório inteiro segue esse tom.
- `.clang-format` define o estilo; não gaste revisão com formatação.
- Nada é considerado pronto por compilar. **O que não foi testado no aparelho
  tem que ser declarado como não testado**, inclusive na mensagem de commit. Hoje
  isso é o fork inteiro.
- Ao mexer na previsão do tempo, no WAV ou no ticker, verifique as **duas
  cópias** (`src/` e `weather/`).
- Ao mexer em layout, gere os PNGs do simulador e confira a área segura.
- Ao mudar uma regra do port, mude o `PORTING.md` junto.
- `PLANO_E_REVISAO.md` tem o roteiro de teste físico do Fruit Jam e, abaixo, o
  histórico do upstream. Leia antes de reportar algo como novo.
