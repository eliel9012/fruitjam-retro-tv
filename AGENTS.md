# AGENTS.md — como este firmware funciona

Guia para um assistente que vá trabalhar neste repositório sem contexto prévio.
Descreve a arquitetura, as restrições de hardware que não são negociáveis e as
armadilhas que já causaram bug aqui. O `README.md` documenta o **uso**; este
arquivo documenta o **código**.

Idioma: comentários, strings de interface e mensagens de commit em português do
Brasil. Strings desenhadas na tela são ASCII sem acento (ver "Texto" abaixo).

---

## 1. O que é

Firmware Arduino/ESP32 para um **M5Stack Core2** com o **Module13.2 RCA (M125)**
empilhado. Transforma o conjunto numa "TV retrô": reproduz vídeo e som do cartão
microSD numa TV de tubo pela saída de vídeo composto, e tem mais três telas —
radar de tráfego aéreo, player de música e um clone do *Local Forecast* do
Weather Channel dos anos 80.

Dois projetos PlatformIO independentes:

| Projeto | Raiz | O que é |
|---|---|---|
| principal | `/` | firmware completo, todas as telas |
| weather | `weather/` | só o Weather Channel, autocontido, para gravar sozinho |

O `weather/` **duplica de propósito** rotinas do principal (WAV, parse do tempo,
ticker). Ao mexer numa, verifique a outra — divergência silenciosa entre as duas
cópias já produziu bug. As divergências conhecidas e intencionais estão em
`PLANO_E_REVISAO.md`.

---

## 2. Restrições de hardware — leia antes de propor qualquer coisa

Estas não são preferências de estilo. Cada uma já quebrou o firmware.

### 2.1 C++11

O firmware compila em `-std=gnu++11` (arduino-esp32 2.x). O simulador de telas
compila em **C++17**. `constexpr` com laço ou variável local passa no simulador e
**quebra no firmware**. Já aconteceu com a `VcrFont.h`.

Antes de entregar qualquer header compartilhado:

```sh
make -C sim cxx11
```

### 2.2 Memória: a SRAM é o recurso escasso, não a PSRAM

| Recurso | Uso | Total |
|---|---|---|
| **SRAM interna** | **153.600 B só do framebuffer do CVBS** (320×240 × 16 bits) | ~320 KB |
| PSRAM | `MAX_JPEG` 128 KiB, buffers TLS, sprites de capa e HUD | 4,5 MB |
| Flash | ~1,44 MB | 6,5 MB |

O `M5ModuleRCA` é construído com `psram_no_use` **de propósito**: a PSRAM é lenta
demais para o prazo por linha de varredura do NTSC. Mover o framebuffer para lá
liberaria 150 KB de SRAM e quebraria o vídeo.

Consequências práticas:

- Nada de alocar quadro inteiro (76.800 ou 153.600 bytes) sem o chamador pedir.
  É por isso que o `crossfade` do `ScreenFx.h` exige um sprite **fornecido pelo
  chamador** e nunca aloca sozinho.
- Buffer grande vai para a PSRAM (`ps_malloc`), nunca para a pilha. Ver 2.3.
- Otimizar PSRAM não rende nada: ela está em menos de 3%.

### 2.3 Pilha das tarefas

| Tarefa | Pilha | Observação |
|---|---:|---|
| `loopTask` (Arduino) | 8192 | `CONFIG_ARDUINO_LOOP_STACK_SIZE` |
| `WEATHER_HTTP` | 8192 | ainda precisa dos quadros do HTTPClient e do mbedTLS |
| `RADAR_HTTPS` | 8192 | idem |
| `RCA_PCM` (áudio) | 4096 | core 0 |

Um handshake mbedTLS precisa de 4 a 6 KB. **Array local de alguns KB numa
função que faz HTTPS é estouro de pilha.** Já aconteceu duas vezes: um
`char buf[4096]` no `weatherFetch` e um `playback::MjpegReader` local (4104
bytes) no benchmark. Buffers grandes vão para a PSRAM ou reusam um global.

### 2.4 Pinos e barramentos

Ver `PINOUT.md`. O que mais importa:

- **GPIO 19 é o BCK do módulo RCA** e por isso **não pode entrar no SPI do
  cartão**. O MISO do microSD é o GPIO 38.
- **I2S0 é do vídeo composto.** O áudio usa I2S1.
- RCA e alto-falante interno usam o I2S1 **alternadamente**, nunca juntos: o
  firmware encerra o dono anterior antes de trocar.
- O microSD divide o barramento VSPI com o LCD. É por isso que o vídeo **não é
  espelhado no LCD** — o LCD mostra só um pôster estático mais um HUD de 1 Hz.

### 2.5 Área segura do tubo

Uma TV CRT esconde cerca de 7% de cada borda (overscan). O raster inteiro nunca
é visível.

`include/SafeArea.h` define a geometria, compartilhada entre firmware e
simulador:

```
crt::W, crt::H              320x240
crt::SAFE_L/T/R/B           24, 18, 296, 222
crt::HEAD_Y/HEAD_RULE_Y     cabeçalho padrão das telas
crt::BAR_Y/BAR_H            barra de legendas dos três botões
crt::TICKER_Y/TICKER_H      faixa do ticker da previsão
```

Regra: **texto, réguas e faixas ficam dentro da caixa segura; só o fundo e os
marquees sangram até a borda do raster.** Nada de tarja preta em volta — isso
deixaria a imagem parecendo pequena no tubo.

### 2.6 Sinal de vídeo: NTSC, não PAL-M

O aparelho está no Brasil, mas o firmware emite **NTSC**. Não é descuido.

A tabela `PAL_M` do M5GFX 0.2.29 monta a linha com **908 amostras**, mas a conta
correta é 4 × 3,57561149 MHz × 63,5556 µs = **909,02**. A linha sai ~0,11% curta,
a fase da burst de cor anda a cada linha e o resultado é uma faixa de cor
diagonal que caminha pela tela. A tabela NTSC usa **910**, que é exato para
4 × 3,579545 MHz. Como PAL-M e NTSC compartilham 525 linhas e 59,94 campos, a
geometria é a mesma; só o encode de cor muda.

**Teto de qualidade da RCA — não é limitação do aparelho:**

| Limite | Valor | Por quê |
|---|---|---|
| largura útil | ~320 px | a luminância NTSC tem ~4,2 MHz → ~330 pontos por linha |
| altura | 240 (240p) | 1 linha de framebuffer por linha de varredura, exato |
| taxa | 29,97 / 59,94 | taxa de campo do NTSC |

Acima de **320×240 a 30 quadros/s não há detalhe a ganhar num tubo.**

### 2.7 Texto é ASCII

As fontes bitmap (M5GFX `Font0/2/4` e a `VcrFont` própria) só têm ASCII. Em UTF-8
uma letra acentuada são **dois bytes**, ambos sem glifo, então `"Não"` vira
`"N  o"` — dois buracos, não um.

**Todo texto que venha de fora — nome de pasta do cartão, `meta.json`, tag ID3,
resposta de API — passa por `ascii::normalize` ou `ascii::normalizeUpper`
(`include/Ascii.h`) antes de ser desenhado.** O `VcrOsd.h` normaliza sozinho;
os outros caminhos normalizam no chamador.

---

## 3. Arquitetura

### 3.1 Mapa dos arquivos

```
src/main.cpp          ~3.700 linhas. Máquina de estados da interface, player,
                      radar, previsão, player de música, console de diagnóstico.
                      É onde quase tudo acontece.
src/ConfigurationPortal.cpp   portal Wi-Fi em modo ponto de acesso
src/SecretsManager.cpp        leitura/escrita de settings.json e secrets.json
src/NetworkManager.cpp        conexão Wi-Fi
src/InputManager.cpp          três botões, com auto-repeat

include/PlaybackIO.h    leitor de MJPEG concatenado e de WAV PCM
include/Id3.h           tags ID3v1/v2, inclusive capa embutida (APIC)
include/Ascii.h         normalização de acentos para ASCII
include/SafeStorage.h   gravação com .bak, recuperada no próximo boot
include/UiLogic.h       enum UiState e funções puras testáveis
include/SafeArea.h      geometria da saída composta
include/LocalizationPTBR.h    strings da interface

include/VcrFont.h       fonte bitmap 12x16 do OSD (1.440 B de flash)
include/VcrOsd.h        OSD estilo videocassete Sony/Semp dos anos 90
include/WeatherIcons.h  ícones de condição estilo Weather Star 4000
include/ScreenFx.h      transições: fade, crossfade, slide, wipe

sim/                    simulador SDL das telas (ver seção 6)
tools/prepare_video.py  converte MP4 em MJPEG+WAV com FFmpeg
tools/sync_schematik.py sincroniza schematik-project.json
tests/                  testes nativos com ASan/UBSan
```

### 3.2 Tarefas e quem roda onde

| Contexto | O que faz |
|---|---|
| `loop()` (core 1) | interface, decodificação JPEG, desenho, diagnóstico serial |
| `RCA_PCM` (core 0) | alimenta o I2S com PCM do cartão, ritmado pelo relógio de amostras |
| `WEATHER_HTTP` / `RADAR_HTTPS` (core 1, prio 0) | uma consulta HTTPS, publica e morre |

**Todo desenho acontece no `loop()`.** As tarefas de rede nunca desenham: elas
escrevem num buffer e publicam por `std::atomic`.

### 3.3 Sincronização

**`sdMutex`** protege o cartão microSD, disputado entre o `loop()` e a tarefa de
áudio. Quem lê o cartão toma o mutex. Padrão: tomar e devolver **por operação**,
nunca segurar durante um trabalho longo — segurar mata o áudio de fome.

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
                AIRCRAFT_RADAR
                SETTINGS → SETUP_PORTAL
                SYSTEM_INFO
                WEATHER }
              ERROR_SCREEN (de qualquer lugar)
```

O enum está em `include/UiLogic.h`. O `loop()` despacha por `state`.

**Armadilha conhecida:** várias funções de desenho guardam estado em `static`
local. Ao entrar numa tela, zere o que precisa ser zerado — estado herdado da
visita anterior já deixou a tela da previsão em branco permanentemente.
`startWeather()` é o exemplo de como fazer certo.

### 3.5 Caminho do vídeo

```
cartão → MjpegReader.next() → jpegBuffer (PSRAM, 128 KiB)
       → JPEGDEC.decode()   → jpegDraw() por bloco de MCU
       → rca.pushImage()    → framebuffer CVBS (SRAM)
       → DMA do I2S0        → GPIO 26 → TV
```

O vídeo é **centralizado** no quadro de 320×240 por `jpegDraw()`; não há escala.
O relógio é o **PCM entregue**, não `millis()`: `videoTick()` calcula o quadro
alvo a partir de `samplesPlayed` e pula quadros atrasados **sem decodificar**
(passando `render = false`).

Por isso há dois contadores diferentes, e confundi-los já causou bug:

- `videoFrameIndex` — posição no fluxo. Avança **também** nos quadros descartados.
- `renderedSeq` — quadros efetivamente desenhados. É o que o OSD usa.

### 3.6 OSD do player

`include/VcrOsd.h`. Imita o OSD de um videocassete Sony/Semp dos anos 90: texto
flutuando **sobre a imagem, sem tarja de fundo**, com contorno preto por glifo,
símbolo de transporte desenhado como forma (nunca a palavra "PLAY") e contador
de fita em dígitos grandes.

Como não há fundo próprio, **cada repintura precisa partir de imagem limpa**:

- com o filme rodando: logo depois de um quadro novo (o quadro apaga o OSD anterior);
- pausado: `redrawCurrentFrame()` redecodifica o último JPEG, que continua
  inteiro no `jpegBuffer` — é para isso que existe `lastJpegUsed`;
- ao esconder: `clearOsdLetterbox()` apaga **só o que fica fora do retângulo do
  vídeo**, para não piscar uma tarja preta sobre o filme.

O contorno vem da **dilatação** da máscara do glifo, num passe só: 2,15× os
pixels de um glifo simples, contra 9,00× de redesenhar a string deslocada oito
vezes.

### 3.7 Tela da previsão

Duas páginas — condições atuais e previsão de três dias — alternadas a cada 10 s
com as transições do Weather Star 4000: **cortina vertical** quando chegam dados
novos, **deslizamento horizontal** na troca de página.

Os pintores têm assinatura `(dst, ox, oy, user)`: o deslizamento desenha as duas
páginas deslocadas no mesmo quadro. Com deslocamento zero é o desenho normal.

**Os esmaecimentos usam dithering ordenado de Bayer, não mistura por alfa.** O
framebuffer composto não tem canal alfa e não há folga de SRAM para dois quadros
inteiros. O resultado grosseiro é, por acaso, exatamente o que parece autêntico
para a época.

`fadeOut`, `slide` e `wipe` **não precisam de buffer**. `crossfade` e `fadeIn`
exigem um sprite 320×240 **RGB332** fornecido pelo chamador; passando `nullptr`
eles degradam para corte seco e devolvem `false`.

Fonte dos dados: **Open-Meteo** (`api.open-meteo.com`), pública e sem chave, com
resposta de ~800 bytes. Substituiu o wttr.in, cujo JSON de ~39 KB não cabia no
buffer e chegava truncado. Os ícones são escolhidos pelo **código WMO**, não por
comparação de string.

---

## 4. Compilar e testar

```sh
python3 -m venv .venv && .venv/bin/pip install platformio==6.1.19

.venv/bin/pio run                 # firmware principal
.venv/bin/pio run -d weather      # projeto da previsão
.venv/bin/pio run --target upload # gravar via USB

./tests/run.sh                    # testes nativos, ASan + UBSan
./tests/media.sh                  # exige FFmpeg/FFprobe

make -C sim                       # simulador de telas
make -C sim cxx11                 # headers compartilhados em C++11
make -C sim probes                # bancadas isoladas
./sim/build/m5sim --png <dir>     # grava as 9 telas em PNG, headless

python3 tools/sync_schematik.py   # obrigatório após mudar fontes
python3 tools/sync_schematik.py --check
```

`./tests/run.sh` falha se o `schematik-project.json` estiver dessincronizado.
Rode o `sync_schematik.py` antes de commitar.

---

## 5. Diagnóstico pelo serial (115200 baud)

```
diag status        heap, PSRAM, FPS, quadros descartados, tempos de decode
diag colors        confere o caminho RGB565 dos blocos JPEG
diag bench         mede leitura do cartão, decode e blit em microssegundos
diag play/pause/resume/stop/home/back/radar/weather/music
diag audio toggle|rca|internal|mute
```

`diag bench` responde a pergunta que nenhum simulador de PC responde: até onde
**este** aparelho sustenta a saída composta. Aceita pasta e contagem de quadros:

```
diag bench /M5RETRO/videos/meu-filme 400
```

---

## 6. O simulador de telas

`sim/` roda o **mesmo rasterizador M5GFX** do aparelho, via backend SDL, no
desktop. Serve para conferir diagramação — sobretudo se algo escapou da área
segura — sem ligar o Core2 na TV.

**O que ele NÃO simula:** Wi-Fi, cartão SD, I2S, decodificação MJPEG, FreeRTOS,
touch, e nada do M5Unified. **E não simula desempenho** — ele roda na CPU do seu
computador, então qualquer FPS medido ali é ficção. Para desempenho, `diag bench`
no aparelho.

O modo `--png` grava as telas sem abrir janela, o que permite conferir área
segura pixel a pixel em CI.

**Cuidado:** `sim/src/main.cpp` é um **espelho manual** das telas do firmware,
não o mesmo código. Divergência entre os dois é bug — o simulador passa a
esconder problemas reais. Onde dá, prefira o simulador **chamar o código de
verdade**: a tela do player já chama `vcr::draw()` diretamente, e é assim que
deve ser.

`sim/probes/` são bancadas isoladas: cada `.cpp` ali vira um binário próprio,
para desenvolver um componente gráfico sem mexer no simulador principal.

---

## 7. Armadilhas que já morderam

Lista curta do que já quebrou, para não repetir.

1. **`constexpr` com laço** — passa no simulador (C++17), quebra no firmware (C++11).
2. **Array de KB na pilha de uma tarefa que faz HTTPS** — estouro de pilha.
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

---

## 8. Ao propor mudanças

- Comentários em português do Brasil, explicando **o porquê**, não o óbvio. O
  repositório inteiro segue esse tom.
- `.clang-format` define o estilo; não gaste revisão com formatação.
- Nada é considerado pronto por compilar. **O que não foi testado no aparelho
  tem que ser declarado como não testado**, inclusive na mensagem de commit.
- Ao mexer na previsão do tempo, no WAV ou no ticker, verifique as **duas
  cópias** (`src/` e `weather/`).
- Ao mexer em layout, gere os PNGs do simulador e confira a área segura.
- `PLANO_E_REVISAO.md` registra problemas já encontrados e o roteiro de teste
  físico. Leia antes de reportar algo como novo.
