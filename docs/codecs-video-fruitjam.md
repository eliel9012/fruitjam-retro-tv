# Codecs de vídeo no Fruit Jam (RP2350B)

Pergunta: com o Fruit Jam dá para trocar o MJPEG (JPEGDEC + WAV) por outro codec
de vídeo? Este documento responde com números e separa o que é **medido**, o que
é **publicado por terceiros** e o que é **estimativa**. Nada aqui foi rodado no
Fruit Jam: o port inteiro continua **não testado no aparelho** (ver `PORTING.md`).

---

## 0. Resposta curta

**Fique no MJPEG baseline 4:2:0 com o JPEGDEC.** Nenhum outro codec disponível
ganha dele nas três coisas que importam aqui ao mesmo tempo — CPU, SRAM e
simplicidade do player — e o gargalo que sobra (decodificar 320x240 a 30 quadros/s
num núcleo só) se resolve usando o **segundo núcleo**, não trocando de codec.

Os números que sustentam isso:

| | valor | de onde vem |
|---|---|---|
| decode MJPEG q5 320x240 no M33 a 264 MHz | **31–37 ms/quadro** (27–32 fps, um núcleo) | emulado + calibrado; bate com o Core2 medido (129 ciclos/px) |
| MPEG-1 (pl_mpeg) precisa de | **3 × 115.200 B = 345.600 B** de quadros YCbCr | código do pl_mpeg; heap medido na emulação: 380–448 KiB |
| SRAM livre depois do framebuffer | ~300 KB (`PORTING.md` §3.8) | — |
| custo do MPEG-1 por quadro (decode + conversão) | típico **13% mais barato** que o MJPEG, mas **11,5% dos quadros custam mais que o pior MJPEG** e o pior custa **2,9×** | emulado, §3 |
| vídeo a 30 fps ocupa do cartão | MJPEG q5: **~0,3 MB/s**; o SDIO de 4 bits entrega **15–27 MB/s** | medido no PC / publicado (arduino-pico) |

E há folga sobrando: durante o vídeo o `loop()`, o áudio e a interrupção do DVI
dividem o **núcleo 0**, e o **núcleo 1 fica ocioso** (§6). Levar o decode para lá
é o primeiro passo; dividir cada quadro entre os dois núcleos é o segundo (§7).

O MPEG-1 só compensa em **espaço no cartão** (mesma qualidade com ~3,5× menos
bytes), e isso não é problema com cartões de 32 GB: uma hora de MJPEG q5 a 24 fps
dá ~0,9 GB de vídeo, ~1,2 GB com o WAV. Ele custa SRAM que não existe, tira o
descarte de quadros sem decodificar e piora o pior caso. Cinepak decodifica 14×
mais rápido mas perde em qualidade por byte, tem codificador 300× mais lento e
quebra o OSD. H.264 não é viável hoje.

---

## 1. Correção de premissas

**O cartão não está em SDIO de 1 bit.** O Fruit Jam liga D0..D3 (GPIO 36..39,
`include/fj/Board.h`) e o `SD.begin(clk, cmd, dat0)` do arduino-pico sobe o
`PioSdioCard` do SdFat, que manda `ACMD6` com argumento 2 — barramento de
**4 bits** (`libraries/SdFat/src/SdCard/PioSdio/PioSdioCard.cpp:312`).

O relógio do cartão sai do PIO: o programa `rd_clk` gasta 5 ciclos de sistema por
ciclo de SD, com `clkDiv = 1`. A 150 MHz isso dá 30 MHz (15 MB/s teóricos, e o
arduino-pico mediu 15,5 MB/s num Pico 2); **a 264 MHz, que é o relógio que o DVI
impõe, dá 52,8 MHz**. O driver não troca o cartão para *high speed* (`CMD6`) antes
de subir o relógio, e o modo padrão do SD é especificado até 25 MHz. Funciona na
maioria dos cartões — o arduino-pico mediu 27 MB/s a 250 MHz — mas é um risco
real de erro de CRC em cartão ruim, e vale para qualquer codec. Ver §8.

**Não há blit.** O `jpegDraw()` escreve direto no `tv`, que é o próprio
framebuffer do DVI. O custo que no Core2 era "blit CVBS" (7,5–12,3 ms) some.

---

## 2. Metodologia

**Clipe de teste.** Trailer do *Big Buck Bunny* (60 s, 640x360 H.264),
reamostrado para 320x240 (escala + recorte, sem tarja) a 24 fps: 1.442 quadros.
É animação, que comprime melhor que filme com grão; isso **favorece** os codecs
com predição entre quadros (MPEG-1, H.264). Com filme real a vantagem deles cai.

**Qualidade × taxa (medido no PC).** FFmpeg 7.0.2; PSNR e SSIM contra a fonte
320x240 sem perdas, quadro a quadro.

**Custo de decodificação (emulado, depois calibrado).** Cada decodificador foi
compilado com o mesmo GCC 16.1 do arduino-pico
(`toolchain-rp2040-earlephilhower`), `-mcpu=cortex-m33 -mthumb -Os` (o `-Os` é o
padrão do `platformio-build.py` do arduino-pico), e executado no emulador
**Unicorn 2.1.4 com o modelo Cortex-M33**, contando instruções por quadro. As
instruções viram ciclos por um modelo simples de M4/M33 (1 ciclo por instrução,
+1 por `LDR`, +1 por registrador de `LDM/STM/PUSH/POP`, +2 por desvio tomado).
Os quadros de saída foram conferidos: JPEG contra o PIL (diferença média de
1 nível por canal), MPEG-1 e Cinepak contra a fonte (33–34 dB em RGB565).

O modelo foi **calibrado contra medições publicadas do próprio JPEGDEC**, com a
imagem `tulips` (640x480, 56 KB) que vem na biblioteca:

| | ciclos | fonte |
|---|---|---|
| ATSAMD51 (Cortex-M4F) 120 MHz | 301.521 µs = **36,2 M** | gráfico `perf.jpg` do JPEGDEC |
| Cortex-M4F 64 MHz | 651.855 µs = **41,7 M** | idem |
| ESP32 240 MHz | 154.884 µs = **37,2 M** | idem |
| modelo, mesmo código compilado para M4 `-Os` | **34,0 M** | emulado |

Fator de correção: **1,06 a 1,23**. Todo tempo em ms abaixo é
`ciclos do modelo × (1,06…1,23) / 264 MHz`.

Conferência independente: o `diag bench` do Core2 mediu **20,6 ms** de JPEGDEC
puro (blit descontado) em 240x160 a 240 MHz = 129 ciclos/px. Aplicado a 320x240 a
264 MHz dá 37,5 ms, dentro da faixa acima.

**O que o modelo não vê:** falta no cache XIP de 16 KB (compartilhado entre o
flash e a PSRAM), a interrupção de linha do DVI roubando o núcleo 0, disputa de
barramento com o DMA do HSTX. Por isso os números valem para **um núcleo livre,
código e dados quentes na SRAM ou no cache**. O código quente de cada
decodificador medido na emulação é pequeno (~6 KiB para o JPEGDEC e para o
pl_mpeg), então cabe no cache — mas divide o cache com a PSRAM.

---

## 3. Tabela comparativa

Resolução 320x240. "fps" = quanto um núcleo decodifica, sem contar leitura do
cartão e áudio. Taxas de bits a 24 fps; para 30 fps multiplique por 1,25.

| Codec | taxa (kbit/s) | PSNR Y / SSIM Y | decode no M33 264 MHz (médio / pior) | memória extra na SRAM | biblioteca / licença | conversão no PC |
|---|---:|---|---|---|---|---|
| **MJPEG q5 4:2:0** (atual) | 2.013 | 38,4 dB / 0,969 | **31–37 ms / 37–43 ms** (27–32 fps) | ~18 KB (objeto JPEGDEC) | JPEGDEC, Apache-2.0, madura | FFmpeg, 0,03× tempo real |
| MJPEG q8 4:2:0 | 1.378 | 35,6 / 0,945 | 29–33 / 33–39 ms | idem | idem | idem |
| MJPEG q3 4:2:0 | 2.858 | 41,5 / 0,983 | 35–40 / 41–48 ms | idem | idem | idem |
| MJPEG 4:4:4 (libjpeg q75) | +22% sobre 4:2:0 | croma melhor | **+39% de ciclos**: 45–52 ms | idem | idem | **não com FFmpeg** (ver §4.1) |
| MPEG-1 I/P (pl_mpeg) 570 kbit/s | 569 | 37,7 / 0,965 | **29–33 ms / 108–125 ms** (decode + conversão) | **345.600 B** (3 quadros) ou 230.400 B com patch | pl_mpeg, MIT, arquivo único | FFmpeg `mpeg1video`, 0,03× tempo real |
| MPEG-1 I/P 1000 kbit/s | 893 | 40,4 / 0,980 | 36–42 ms / 116–134 ms | idem | idem | idem |
| Cinepak | 2.266 | 37,3 / 0,969 (média YUV 37,9) | **2,3–2,7 / 4,3–5,0 ms** | 0 (decodifica no framebuffer) | decodificador do ScummVM, **GPL-3.0** | FFmpeg `cinepak`, **8× mais lento que tempo real** |
| H.264 baseline 300 kbit/s | 303 | 38,4 / 0,973 | **~45 ms + 8–9,5 ms de conversão** (extrapolado de medição publicada) | ~230 KB (2 quadros) | TinyH264, **GPL-3.0**, nova | FFmpeg `libx264`, 0,13× tempo real |
| QOI por quadro (sem perdas) | ~16.400 | ∞ | 15–18 / 21–24 ms | 0 | qoi.h, MIT | FFmpeg `qoi` |
| RGB565 cru | 29.491 (30 fps: 4,6 MB/s) | — | ~0 (só cópia) | 0 | — | FFmpeg `rawvideo` |

Detalhes do MPEG-1 (emulado, `-Os` salvo indicação; conversão com a faixa de
vídeo BT.601 16–235 e saída conferida contra a fonte):

| fluxo MPEG-1 (sem B, GOP 24) | kbit/s | decode médio | pior quadro | + conversão | total médio a 264 MHz | pior total |
|---|---:|---:|---:|---:|---:|---:|
| 600 kbit/s (≈ qualidade do MJPEG q5) | 569 | 5,06 M ciclos | 24,88 M | 2,04 M | **29–33 ms** | **108–125 ms** |
| 1000 kbit/s | 893 | 6,89 M | 26,73 M | 2,04 M | 36–42 ms | 116–134 ms |
| 1500 kbit/s | 1.204 | 8,50 M | 28,93 M | 2,04 M | 42–49 ms | 124–144 ms |
| 1000 kbit/s com quadros B | 946 | 7,47 M | 30,06 M | 2,04 M | 38–44 ms | 129–150 ms |
| 1000 kbit/s, `-O2` | 893 | 5,75 M | 20,82 M | ~1,83 M | 30–35 ms | 91–106 ms |

Distribuição quadro a quadro, decode + conversão, ciclos do modelo:

| | p50 | p90 | p99 | máximo |
|---|---:|---:|---:|---:|
| MJPEG q5 (361 quadros amostrados) | 7,87 M | 9,07 M | 9,19 M | 9,23 M |
| MPEG-1 600 kbit/s (1.441 quadros) | 6,81 M | 9,65 M | 14,85 M | 26,92 M |

O MPEG-1 é **13% mais barato no quadro típico**, empata no p90 e perde feio na
cauda: **166 dos 1.441 quadros (11,5%) custam mais que o pior quadro do MJPEG**,
e o pior custa 2,9× — são os quadros P de cena com muito movimento, que não se
pode pular.

A conversão YCbCr 4:2:0 → RGB565 que o MPEG-1 e o H.264 precisam e o JPEG faz por
dentro custa **2,04 M ciclos por quadro** (26,6 ciclos/px, laço inteiro em C
puro, 2x2 pixels por vez): **8,2–9,5 ms**. Dá para cortar pela metade com as
instruções SIMD do M33 (`UQADD8`, `SSAT16`), mas é trabalho a mais que o MJPEG
não tem.

---

## 4. Codec por codec

### 4.1 MJPEG com JPEGDEC (linha de base)

- **Otimização para Cortex-M: nenhuma que valha no M33.** O caminho SIMD do
  JPEGDEC para M4/M7 existe mas está comentado (`//#define HAS_SIMD` em
  `jpeg.inl`), e só o ESP32-S3 (instruções PIE), x86 (SSE) e ARM64 (NEON) têm
  SIMD ativo. O RP2350 compila o C puro. Definir `ARM_MATH_CM4` para ligar o
  `ALLOWS_UNALIGNED` não mudou uma instrução do caminho quente (emulado). O
  Core2 também rodava C puro (ESP32 comum, sem PIE), então a comparação com o
  Core2 é justa.
- **Custo por quadro escala com os bytes**, como diz o próprio JPEGDEC:
  q12 → q3 (5,1 → 14,5 KB por quadro) vai de 87 para 112 ciclos/px.
- **`-O2` em vez de `-Os`:** 7,32 M contra 7,83 M ciclos (−6,5%). Pouco.
- **4:2:0 contra 4:4:4** (quadros codificados pelo libjpeg do PIL, mesma
  qualidade 75): 4:4:4 tem **+22% de bytes e +39% de ciclos** (145 contra 104
  ciclos/px) — o dobro de blocos de croma para IDCT e conversão. Num tubo de TV,
  onde a croma do NTSC já tinha ~1/4 da banda da luminância, não compensava; no
  DVI se vê, mas não paga 39% de CPU.
- **Armadilha encontrada: o 4:4:4 do FFmpeg não decodifica no JPEGDEC.** O
  `mjpeg` do FFmpeg com `-pix_fmt yuvj444p` grava fatores de amostragem **1x2**
  em todos os componentes (SOF `...011200021200031200`), em vez de 1x1. O
  JPEGDEC devolve `JPEG_DECODE_ERROR` em **1.417 de 1.442 quadros** (os 25 que
  passam são quadros quase pretos). `yuvj422p` falha igual. O mesmo quadro em
  4:4:4 pelo libjpeg (fatores 1x1) decodifica sem erro. Testado no JPEGDEC fixado
  em `platformio.ini` (86282979) e no `master` atual (é o mesmo commit). O
  `prepare_video.py` fixa `yuvj420p`, então hoje não morde; morde quem tentar
  "melhorar a cor" trocando para `yuvj444p`.
- **Tamanho de quadro:** q5 dá 10,2 KB de média e 16,7 KB no pior quadro. O
  `MAX_JPEG` de 128 KiB na PSRAM sobra com folga; a leitura do buffer é
  sequencial (o JPEGDEC copia em blocos de 2 KB), então a PSRAM não pesa.
- **Descarte de quadros:** cada quadro é independente. O `videoTick()` pula
  quadros atrasados sem decodificar (`render = false`). Isso só existe porque o
  codec é intra.

### 4.2 MPEG-1 com pl_mpeg

- **Qualidade por byte: ótima.** 570 kbit/s dá a mesma qualidade do MJPEG q5 a
  2.013 kbit/s — **3,5× menos**. A 893 kbit/s já passa o q3.
- **Memória: o problema.** O pl_mpeg aloca três quadros de uma vez
  (`plm_video_decode_sequence_header`): Y 320x240 + Cb/Cr 160x120 = 115.200 B por
  quadro, **345.600 B**. O heap medido na emulação fecha em **380 KiB** logo no
  começo e em **448 KiB** no fim do clipe: o pl_mpeg só decodifica uma imagem
  que esteja inteira no buffer, e o buffer (32 KiB no início) cresce com
  `realloc` até caber a maior. Somado ao framebuffer de 153.600 B dá ~530 KB:
  **não cabe nos 520 KB de SRAM**, nem de longe com o resto do firmware.
  - Sem quadros B (`-bf 0` no FFmpeg) só dois quadros são necessários, mas o
    pl_mpeg aloca três mesmo assim: exige **patch** na biblioteca.
    2 × 115.200 = 230.400 B. Dos ~300 KB livres sobram ~70 KB para pilhas,
    WiFiNINA, MP3, legendas. Apertado demais para um firmware que também tem
    rádio, previsão e radar.
  - Quadros de referência na PSRAM: a compensação de movimento lê blocos 16x16
    (17x17 com meio pixel) em posição arbitrária, e o cache XIP tem linhas de
    **8 bytes** e 16 KB para flash e PSRAM juntos. Cada linha de 17 px toca 3
    linhas de cache; cada falta custa ~30 ciclos de QSPI (comando, endereço,
    espera, 8 bytes) ≈ 60–70 ciclos de CPU. **Estimativa:** ~85 faltas por
    macrobloco × 300 macroblocos ≈ **1,5–2 M ciclos por quadro** só de leitura,
    mais a escrita do quadro novo na PSRAM (115 KB; leitura em cache publicada:
    24–42 MB/s a 150 MHz) ≈ **3–5 ms**. Soma **+10–15 ms por quadro** e
    ainda derruba do cache o código que roda do flash. Não medido.
- **CPU:** ver §3. O quadro típico sai 13% mais barato que o MJPEG q5 (decode +
  conversão), mas a cauda é longa: p99 1,6× e pior quadro 2,9× o pior do MJPEG.
  É quadro P de muito movimento — justamente o que não se pode pular.
- **Descarte de quadros acaba.** Quadro P depende do anterior: atrasou, tem de
  decodificar do mesmo jeito, ou pular até o próximo I (no nosso GOP de 1 s, até
  um segundo congelado). O `videoTick()` e o OSD (`videoFrameIndex` ×
  `renderedSeq`) teriam de ser repensados.
- **Publicado:** `button-video` (RP2040, pl_mpeg, 128x128) roda "abaixo de 30
  fps" a 125 MHz e exige overclock a 275 MHz. O *Macchiato DX* (RP2040, 240x240
  a 30 fps) chegou lá **reescrevendo o decodificador**, e registra que o pl_mpeg
  passa o estado num struct grande por ponteiro e perde tempo em indireções. O
  *espflix* (ESP32, NTSC composto, 352x192) também reescreveu a compensação de
  movimento, eliminou os quadros B e picotou os dois quadros em faixas para caber
  na RAM.

### 4.3 Cinepak

- Decodificação **barata de verdade: 7,4 ciclos/px**, 2,3–2,7 ms por quadro
  (emulado, decodificador do `aviPlayer` do moononournation, derivado do ScummVM).
  O livro de códigos já sai em RGB565 e o quadro é escrito direto no framebuffer:
  **zero SRAM extra**.
- Mas **perde em qualidade por byte**: 2.266 kbit/s para 37,3 dB de luminância
  (37,9 dB na média Y/Cb/Cr), pior que o MJPEG q5 com menos bytes (2.013 kbit/s,
  38,4 dB e 41,0 dB).
- O codificador do FFmpeg é **84 s para 10 s de vídeo** (contra 0,28 s do MJPEG).
  Um filme de 2 h leva ~17 h para converter.
- **Quebra o OSD.** O quadro inter do Cinepak só repinta os blocos que mudaram.
  O `VcrOsd`, as legendas e o filtro VHS desenham no mesmo framebuffer e
  contam com "o próximo quadro apaga o OSD" (`AGENTS.md` §3.6): com Cinepak o
  texto fica grudado nos blocos parados. Consertar exige um segundo quadro de
  153.600 B (não há) ou forçar todo quadro como chave (aí a taxa explode).
- Licença: o decodificador disponível é **GPL-3.0**. O do FFmpeg é LGPL mas
  não é embarcável como está.

### 4.4 QOI por quadro, RLE/delta próprio, RGB565 cru

- **QOI** (sem perdas, MIT): 85,5 KB por quadro, 50 ciclos/px (emulado,
  decodificando direto para RGB565). Nem rápido o bastante para valer a taxa de
  2 MB/s nem compacto.
- **RGB565 cru**: 153.600 B por quadro, 4,6 MB/s a 30 fps, **276 MB por minuto**.
  O SDIO aguenta, a CPU fica livre, mas um filme de 2 h ocupa 33 GB.
- **RLE/delta próprio**: o `popcorn` da Raspberry Pi (pico-playground) toca
  320x240 a 30 fps num RP2040 a **48 MHz** com um formato próprio; o Big Buck Bunny
  de 10 min dá 1,6 GB (~2,7 MB/s). Prova que dá para trocar CPU por bytes — mas
  aqui a CPU não é o aperto, e formato próprio é conversor, validador e
  decodificador a manter. Um delta contra o quadro anterior herda o mesmo
  problema do OSD do Cinepak.

### 4.5 H.264 baseline

- Qualidade por byte imbatível: **300 kbit/s = MJPEG q5** (6,7× menos).
- **Publicado:** o TinyH264 (pschatzmann, GPL-3.0, header-only, "validado" com
  `arduino-cli` no RP2040) decodifica QCIF 176x144 no **RP2350 em 26,2 ms**
  médios (29,5 ms no pior), num clipe de teste próprio. O README não dá o relógio
  do teste de decode; ao comentar o encoder, o autor lembra que o padrão do
  arduino-pico para o RP2350 é 150 MHz. Supondo 150 MHz — **estimativa:**
  3,03× mais pixels → ~79 ms a 150 MHz → **~45 ms a 264 MHz**, mais 8–9,5 ms de
  conversão YCbCr → **~18 fps**, com o conteúdo leve do clipe deles. Memória: dois
  quadros YCbCr (~230 KB) mais estado por macrobloco — o mesmo problema do MPEG-1.
- O `esp_h264` da Espressif é binário pré-compilado para ESP32; não serve no
  RP2350.
- Veredito: não viável hoje. Biblioteca nova, GPL, sem otimização para M33.

### 4.6 Contêiner (AVI) em vez de pastas

O AVI juntaria vídeo e áudio num arquivo só, com índice (`idx1`) — o que
permitiria **pular para um ponto do filme** sem varrer o MJPEG. O FFmpeg grava
direto (`-f avi -c:v mjpeg`). Custa um parser de RIFF e ler o mesmo arquivo em
dois pontos (vídeo no `loop()`, áudio na tarefa), o que o `sdMutex` já resolve.
Não muda nada de desempenho. Vale se o avanço rápido/busca entrar no roteiro;
senão, as pastas atuais bastam e servem aos dois aparelhos (`PORTING.md` §3.9).

---

## 5. Áudio junto

Hoje: WAV PCM 16 bits, estéreo, 22.050 Hz = **705,6 kbit/s = 88 KB/s**. Ao lado do
MJPEG q5 (2.013 kbit/s) isso é **26% do arquivo**; ao lado de um MPEG-1 de
600 kbit/s seria mais da metade.

| Formato | taxa | CPU | ganho real |
|---|---:|---|---|
| WAV PCM (atual) | 705,6 kbit/s | zero | — |
| **IMA ADPCM 4 bits** (WAV `0x11`, FFmpeg `adpcm_ima_wav`) | 176 kbit/s | **emulado:** ~61 ciclos por amostra (laço C ingênuo, `-Os`) → 2,7 M ciclos/s = **~1% de um núcleo** | −530 kbit/s: **filme ~20% menor** |
| MP3 22.050 Hz (libhelix, já está no `lib_deps`) | 64–96 kbit/s | **medido no Core2:** 5,0 ms por quadro MP3 de 44,1 kHz estéreo com buffers na SRAM (commit `21aa210`) ≈ 19% de um núcleo; a 22.050 Hz, metade | −610 kbit/s, mas custa ~10% de núcleo e o relógio de amostras passa a depender do decodificador |

**A banda do cartão não é argumento para comprimir o áudio**: 88 KB/s contra
15–27 MB/s. O ganho é só espaço. O ADPCM é o meio-termo honesto: 4× menor, custo
de CPU desprezível, sincronia pelo `samplesPlayed` intacta.

---

## 6. Onde está o gargalo de verdade

Orçamento de um quadro a 30 fps: **33,3 ms**.

| etapa | Core2 (medido, 240x160) | Fruit Jam (estimado, 320x240) |
|---|---|---|
| leitura do cartão | ~1 ms | 10–17 KB a 15–27 MB/s: **< 1 ms** |
| decode JPEG | 20,6 ms | **31–37 ms** (pior quadro 37–43) |
| blit | 7,5–12,3 ms | ~0: o `jpegDraw` entrega cada bloco ao `pushImage` do LovyanGFX, que só troca os bytes (RGB565 → RGB565 invertido). **Estimativa:** 2–4 ciclos/px, ~1 ms. Não emulado. |
| filtro VHS (`vhsFilter.pushBlock`) | dentro do blit | não medido; desligado custa o mesmo `pushImage`; ligado soma o borrão de croma por bloco |

**E o núcleo que decodifica não está livre.** No arduino-pico com FreeRTOS o
`loop()` roda na tarefa `CORE0`, presa ao **núcleo 0**
(`cores/rp2040/freertos/freertos-main.cpp:149-150`), com prioridade 4 de 8. No
mesmo núcleo 0 estão a interrupção de linha do DVI (`PORTING.md` §2.1) e a
tarefa de áudio `RCA_PCM`, também com prioridade 4 (`src/main.cpp`, criação da
tarefa em `setup()`). O **núcleo 1 fica ocioso durante o vídeo** — lá só rodam as
tarefas de rede do radar e da previsão, que nem existem enquanto o filme toca.

Conclusão: **o decode é o gargalo e fica no limite dos 30 fps mesmo com um núcleo
inteiro**; dividindo o núcleo 0 com o DVI e o áudio, fica abaixo. A 24 ou 25 fps
(a maioria dos filmes) deve caber; a 30 fps com q5, não no pior quadro. O cartão
e o áudio não pesam. Trocar de codec não resolve: o único mais rápido (Cinepak)
perde em qualidade e quebra o OSD, e os mais compactos (MPEG-1, H.264) custam o
mesmo ou mais de CPU e não cabem na SRAM. **O recurso sobrando é o núcleo 1.**

---

## 7. Recomendação e o que muda

### 7.1 Agora — nada no formato

1. Gravar e rodar `diag bench /M5RETRO/videos/<pasta> 400` com um vídeo 320x240
   q5. A previsão é **31–37 ms de decode** num núcleo livre; com o `loop()`
   dividindo o núcleo 0, um pouco mais. Se vier muito acima, o suspeito é o
   cache XIP (código do JPEGDEC no flash disputando com a PSRAM), não o codec.
2. Se o bench confirmar e 24/25 fps bastarem, o padrão do `prepare_video.py`
   (`--size 320x240`, que o `PORTING.md` §3.5 já pede) com `--fps` até 25 fica.

### 7.2 Passo 1 — levar o decode para o núcleo 1 (sem mudar o formato)

Uma tarefa `VIDEO_DEC` presa ao núcleo 1 roda o `readAndShowOneFrame()`; o
`loop()` continua no núcleo 0 cuidando de interface, OSD e botões, e só pede
"quadro N" e espera o aviso de pronto. Isso sozinho põe o decode num núcleo sem
DVI e sem áudio, que é a condição dos 27–32 fps do §3, e tira o áudio da disputa
com o JPEG. Não muda arquivo nenhum no cartão nem o `prepare_video.py`.

Cuidados: todo desenho no `tv` tem de continuar num lugar só por vez (`AGENTS.md`
§3.2: "todo desenho acontece no `loop()`"). O OSD, as legendas e o
`drawOverlay()` do filtro precisam esperar o quadro terminar — a mesma ordem de
hoje, só que atravessando um semáforo. O objeto `jpeg` é global (17.884 B,
medido com `nm`), então a pilha da tarefa só carrega as chamadas; 4–8 KB bastam.

### 7.3 Passo 2 — se 30 fps a 320x240 for requisito: os dois núcleos no mesmo quadro

Cada quadro vira **dois JPEGs**, metade de cima e metade de baixo (320x120). Cada
núcleo decodifica uma metade; as metades escrevem linhas disjuntas do
framebuffer, sem trava no `jpegDraw`. Custo medido no PC: **+6,2% de bytes**
(cabeçalho e tabelas de Huffman em dobro). Capacidade estimada: ~50 fps de
decode (o núcleo 0 continua pagando DVI e áudio), ou seja, 30 fps com folga
mesmo no pior quadro.

Por que não marcadores de reinício (DRI) num JPEG só: daria o mesmo efeito sem
os 6%, mas o JPEGDEC decodifica o quadro de ponta a ponta e não tem API para
começar num RST do meio; seria um fork da biblioteca. Dois JPEGs usam a
biblioteca como ela é.

Mudanças:

- **`tools/prepare_video.py`**: opção `--split` que roda o FFmpeg duas vezes
  (`crop=320:120:0:0` e `crop=320:120:0:120`, mesmo `-q:v`) e intercala os
  quadros no `video.mjpeg` (topo, base, topo, base…). `meta.json` ganha
  `"split": 2`. A validação de tamanho passa a contar pares. Manter o
  `-pix_fmt yuvj420p` e **recusar `yuvj444p`/`yuvj422p`** (ou verificar os
  fatores de amostragem no SOF) por causa do §4.1.
- **`src/main.cpp`** (dono: main-core): segundo objeto `JPEGDEC` **na SRAM** (é
  o estado quente da decodificação) e segundo buffer de entrada;
  `readAndShowOneFrame()` lê os dois quadros com o `sdMutex`, entrega a base para
  a tarefa do outro núcleo e espera por um semáforo. `videoFrameIndex` e
  `renderedSeq` não mudam de significado. O `lastJpegUsed`/`redrawCurrentFrame()`
  do quadro pausado passam a guardar os dois pedaços.
- **`vhsFilter`** (`include/VhsFx.h`): o `beginFrame()` sorteia o quadro inteiro
  antes; o `pushBlock()` precisa ser seguro com dois chamadores simultâneos em
  linhas disjuntas (contadores como `pushes_` e o borrão de croma — conferir).
- A tarefa de decode do núcleo 0 fica com prioridade **abaixo** da do áudio.
- Pastas antigas (`split` ausente) continuam tocando no caminho de um núcleo.

### 7.4 Ajustes baratos, independentes

- Colocar as funções quentes do JPEGDEC na SRAM (`__not_in_flash_func`, ~6 KiB
  de código quente medido) para tirar o decode da disputa do cache XIP com a
  PSRAM.
- `-O2` só ganha 6,5%; não vale o aumento de flash no firmware inteiro.
- Áudio em **IMA ADPCM** (opcional, §5): `readWav()` em `include/PlaybackIO.h`
  passa a aceitar formato `0x11` (hoje recusa tudo que não seja PCM 16 bits
  estéreo 22.050 Hz) e a tarefa de áudio decodifica blocos de 1.024 bytes antes
  do `audioout::write()`. No `prepare_video.py`, `--audio adpcm` troca
  `pcm_s16le` por `adpcm_ima_wav`. **Mexe no WAV: conferir a cópia do
  `weather/`** (`AGENTS.md` §1) e os testes de `readWav` em `tests/test_core.cpp`.

### 7.5 O que **não** fazer agora

- **MPEG-1**: só se o espaço no cartão virar queixa real. Exigiria patch no
  pl_mpeg para dois quadros (230 KB de SRAM, liberados só enquanto o vídeo toca),
  reescrever o descarte de quadros, conversão YCbCr → RGB565 otimizada e
  medir no aparelho se a PSRAM aguenta. Conversão no PC seria trivial
  (`-c:v mpeg1video -bf 0 -g 24 -b:v 700k`, áudio em MP2 dentro de `.mpg`, que o
  pl_mpeg também demultiplexa).
- **Cinepak**, **H.264**, **QOI**, **formato próprio**: pelos motivos do §4.

---

## 8. Riscos

- **Tudo o que é "estimado" ou "emulado" aqui precisa do `diag bench` no Fruit
  Jam.** O modelo de ciclos não vê o cache XIP nem o DVI. O fator de calibração
  foi tirado de M4F, não de M33; o M33 é da mesma família (pipeline de 3
  estágios, Thumb-2 com DSP), mas não é idêntico.
- **Relógio do SDIO a 52,8 MHz sem modo *high speed*** (§1). Se aparecer erro de
  leitura com algum cartão, o conserto é passar `clkDiv` > 1 no `SdioConfig`
  (o `SDFSConfig` do arduino-pico não expõe; seria preciso montar o SdFat direto).
  A 26 MHz ainda sobram ~13 MB/s, 40× o que o vídeo pede.
- **Decode em dois núcleos**: o núcleo 0 já tem DVI e áudio. Se a tarefa de decode
  atrasar a de áudio, volta o arrasto que o rádio já teve. Medir `audioUnderruns`.
- **Rasgo**: o framebuffer é único (`fj/Display.cpp`). Decodificar mais rápido
  não muda o rasgo; esperar o `waitVsync()` antes de decodificar muda, mas custa
  latência.
- **Conteúdo**: o clipe de teste é animação. Filme com grão e movimento de câmera
  aumenta os bytes do MJPEG (e o custo, que escala com os bytes) e aumenta ainda
  mais o pior quadro P do MPEG-1.
- **Licenças**: JPEGDEC Apache-2.0, pl_mpeg MIT, qoi MIT. Cinepak (ScummVM) e
  TinyH264 são GPL-3.0 e contaminariam o firmware inteiro na distribuição.

---

## 9. Fontes

Medido/emulado neste trabalho (PC, 2026-09-25): FFmpeg 7.0.2 (`imageio-ffmpeg`),
Unicorn 2.1.4, Capstone, GCC 16.1 do `toolchain-rp2040-earlephilhower`, JPEGDEC
86282979, pl_mpeg e qoi `master`, Cinepak do `moononournation/aviPlayer`.

Publicado:

- JPEGDEC, gráfico de desempenho e código: <https://github.com/bitbank2/JPEGDEC>
- pl_mpeg: <https://github.com/phoboslab/pl_mpeg>
- button-video (pl_mpeg no RP2040): <https://github.com/unwiredben/button-video>
- Macchiato DX (MPEG-1 240x240 a 30 fps no RP2040): <https://hackaday.io/project/204450-macchiato-dx-practical-video-badge-with-rp2040>
- espflix (MPEG-1 no ESP32 para TV composta): <https://github.com/rossumur/espflix>
- popcorn (320x240x30 no RP2040): <https://github.com/raspberrypi/pico-playground/tree/master/apps/popcorn>
- TinyH264 (desempenho no RP2350): <https://github.com/pschatzmann/TinyH264>
- esp_h264 para Arduino (binário só ESP32): <https://github.com/pschatzmann/codec-h264-ESP32S3>
- aviPlayer (Cinepak/MJPEG no ESP32): <https://github.com/moononournation/aviPlayer>
- SDIO no arduino-pico, medições no Pico 2: <https://github.com/earlephilhower/arduino-pico/issues/2562>
- Vazão da PSRAM no RP2350 (24–42 MB/s em cache, 150 MHz): <https://forums.raspberrypi.com/viewtopic.php?t=386630>

O Macchiato DX (hackaday.io) e o fórum da Raspberry Pi estavam bloqueados para
leitura direta neste ambiente: os números atribuídos a eles vêm do resumo do
buscador, não da página. Todo o resto foi lido na fonte.

---

## Apêndice: como reproduzir

Fonte 320x240 e codificações (FFmpeg 7):

```sh
VF="fps=24,scale=320:240:force_original_aspect_ratio=increase,crop=320:240,setsar=1"
ffmpeg -i fonte.mp4 -an -vf "$VF" -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -f mjpeg q5_420.mjpeg
ffmpeg -i fonte.mp4 -an -vf "$VF" -c:v mpeg1video -b:v 600k -maxrate 600k -bufsize 600k \
       -bf 0 -g 24 -f mpeg1video m1_600.m1v
ffmpeg -i fonte.mp4 -an -vf "$VF" -c:v cinepak -f avi cvid.avi
ffmpeg -i fonte.mp4 -an -vf "$VF" -c:v libx264 -profile:v baseline -b:v 300k -g 24 -f h264 h264.264
# qualidade: [dec]format=yuv444p,settb=1/24,setpts=N contra a fonte crua, filtros ssim e psnr
```

Emulação: cada decodificador vira um ELF bare-metal
(`arm-none-eabi-g++ -mcpu=cortex-m33 -mthumb -mfloat-abi=soft -Os`, ligado em
0x10000000 sem startup), carregado no Unicorn (`UC_MODE_THUMB | UC_MODE_MCLASS`,
`UC_CPU_ARM_CORTEX_M33`) com um gancho por bloco básico que classifica as
instruções pelo Capstone. O pl_mpeg roda com `plm_buffer_create_with_capacity` e
um *callback* que entrega blocos de 8 KiB, como viria do cartão — com
`plm_buffer_create_with_memory` ele faz `memmove` do arquivo inteiro a cada
quadro e o custo sai 10× maior, o que não representa o aparelho.

