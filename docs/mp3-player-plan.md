# Plano de implementação — Player de Música (MP3/WAV) no M5 RETRO TV

> Documento de **planejamento** (sem código aplicado). Não altera `src/main.cpp`
> nem qualquer arquivo de código. Fundamenta-se na leitura do repositório em
> `/Users/eliel/Documents/TV` (revisão de `src/main.cpp`, `include/*.h`,
> `platformio.ini`, `weather/src/main.cpp`, `README.md`, `PINOUT.md` e
> `PLANO_E_REVISAO.md`).

---

## 1. Visão geral e decisões de arquitetura

O firmware atual já resolve MJPEG+WAV (vídeo) e WAV em loop (Weather Channel) com
um padrão de tarefas bem definido e testado. O objetivo é adicionar um **player de
música** como sexto item do menu principal, navegável por **listas hierárquicas
estilo iPod**, com tela de **now playing** mostrando **tags ID3** e **capa do
álbum**, e controles de **play/pause, anterior/próxima, volume, seek, shuffle e
repeat**.

### Decisões de arquitetura (com justificativa)

| # | Decisão | Justificativa |
|---|---------|---------------|
| D1 | **Manter WAV como cidadão de primeira classe** e adicionar MP3 **atrás de uma abstração de fonte PCM**. | O pipeline I2S atual (22050 Hz, PCM 16-bit, estéreo) já está validado; não reescrever o que funciona. WAV custa zero CPU; MP3 é um caminho adicional, não substituto. |
| D2 | **Fonte PCM abstrata (`PcmSource`)** produzindo **22050 Hz estéreo** para o `audioTask` existente. | Isola decodificação do ritmo de I2S. Reaproveita `scalePcm`, `samplesPlayed`, underrun e pacing por relógio PCM que já existem. |
| D3 | **Decodificador MP3 = libhelix** (16-bit inteiro, baixo consumo), **não** `ESP32-audioI2S`. | `ESP32-audioI2S` é pesado, orientado a ESP32-S3/P4, e **gerencia o próprio I2S/tarefa**, o que conflitaria com a posse exclusiva do I2S1 pelo `audioTask`. Detalhes na §2. |
| D4 | **Navegação por sistema de arquivos primeiro** (pastas = hierarquia Artista/Álbum), **tags ID3 lidas sob demanda** (linha selecionada / now playing). | Reusa o padrão já existente de `libraryProgramCount`/`libraryProgramAt` (iterar `SD.openNextFile`). Indexar tags de todo o cartão é lento no SD SPI e não cabe em RAM. |
| D5 | **Capa do álbum** decodificada com o **JPEGDEC já embutido** (bitbank2/JPEGDEC@1.8.2), **escalada**, em sprite **PSRAM RGB565** de 64×64 ou 96×96, cache de **uma** capa. | Zero dependência nova. Memória mínima (8–18 KB). Mesmo padrão de `drawStaticPoster` (sprite PSRAM + `pushRotateZoom`). |
| D6 | **Decodificação de áudio no core 0** (mesmo núcleo do `audioTask`), **UI no core 1** (`loop()`). | Segue o padrão FreeRTOS existente; não bloqueia `serviceWiFi`/`handleNavigation`/radar/weather. |
| D7 | **Estados novos no `UiState`** (`MUSIC_BROWSER`, `MUSIC_NOW_PLAYING`) e **`homeTarget` passa de 5 para 6 itens**. | Integração mínima e consistente com a máquina de estados atual. |

### Não-regressões observadas no código que o plano deve preservar

- A **posse exclusiva do I2S1** pelo `audioTask` e a troca coordenada de saída
  (`AudioOutput::RCA`/`INTERNAL`/`MUTED`) em `initExternalAudio()` e no próprio
  `audioTask` (linhas 434–530 de `src/main.cpp`). O MP3 **não** deve instalar outro
  driver I2S.
- O **`sdMutex`** serializa todo acesso ao SD entre UI e `audioTask`. Toda leitura
  de MP3/WAV/tags/capa deve respeitá-lo, como já fazem `startProgram`,
  `pollAircraft` (CA) e `saveSettings`.
- O `rca` é o único destino de vídeo; o LCD mostra pôster/HUD/placas simples. A
  tela de now playing desenha **nas duas saídas** (RCA + `M5.Display`), como
  `drawLibrary`/`drawRadar` já fazem.
- O `serviceWiFi()` retorna cedo quando `playing` é verdadeiro (linha 331), então a
  reprodução de música **automaticamente** pausa as rotinas de Wi-Fi, como no vídeo.

---

## 2. Escolha da biblioteca de decodificação MP3

O ESP32 **não tem codec de áudio em hardware**. A decodificação MP3 é software puro.

| Biblioteca | Prós | Contras | Veredito |
|---|---|---|---|
| **libhelix-mp3** (Helix MP3, RealNetworks) via **`arduino-libhelix`** (pschatzmann) ou **`esp-libhelix-mp3`** (chmorgan, MIT) | 16-bit inteiro (rápido no Xtensa, sem FPU overhead); RAM de trabalho ~4–8 KB; CPU < ~10% para 128 kbps a 240 MHz; API de baixo nível que **se integra ao nosso I2S**; cabe folgadamente no firmware de 1.34 MB | Precisa do nosso próprio resampler (44100→22050); frame-a-frame manual (sync, header, bit reservoir) | **Recomendado** |
| **`ESP32-audioI2S`** (schreibfaul1) | "Uma linha" para MP3/AAC/WAV de SD/HTTP; abstrai tudo | Pesado; recomendado para S3/P4; **instala e gerencia o próprio driver I2S e tarefa** → conflito direto com o `audioTask`/I2S1 atual; aloca internamente muita RAM; difícil de coexistir com CVBS/radar/weather | Rejeitado |
| **Converter tudo para WAV** (offline, via `tools/`) | Zero código de codec; reusa 100% do pipeline WAV atual | Perde o objetivo explícito "MP3"; duplica espaço em cartão; o usuário quer MP3 nativo | Fallback/alternativa documentada, não o caminho principal |

### Recomendação

Usar **libhelix** embrulhado para Arduino. Duas vias viáveis:

1. **`pschatzmann/arduino-libhelix`** — instalação direta via `lib_deps` do
   PlatformIO (mesmo estilo das deps atuais), API simples (`MP3DecoderHelix` com
   `write(bytes, len)` e callback de PCM, ou `decode()` frame-a-frame).
   Licenciamento: incorpora código Helix original sob **RPSL** (obrigações de
   redistribuição do código-fonte).
2. **`chmorgan/esp-libhelix-mp3`** — componente ESP-IDF, reimplementação **MIT**
   limpa. Exige trazê-lo via `lib_extra_dirs`/git no `platformio.ini` (projeto usa
   Arduino, não IDF puro), um pouco mais de fricção de integração.

Para um projeto de hobby, **`arduino-libhelix`** é o caminho de menor atrito; se a
licença RPSL incomodar, a reimplementação MIT é o substituto direto com a mesma
API de conceito (frame in → PCM out).

### Impacto estimado

- **Firmware**: +30–50 KB de código (pouco diante dos 1.34 MB atuais).
- **RAM**: ~4–8 KB de área de trabalho + 1 buffer de frame comprimido (~2–4 KB) +
  saída de um frame PCM (~4.6 KB p/ Layer III MPEG1 estéreo 1152 samples). Tudo
  cabe sem tocar na SRAM reservada a DMA/vídeo; usar PSRAM para o frame comprimido.
- **CPU**: decodificar 128–320 kbps em tempo real usa bem menos que um núcleo; roda
  no core 0 junto do `audioTask`, sem competir com o JPEG do vídeo (que não roda
  durante música) nem com a UI no core 1.

### Ponto crítico: resampling para 22050 Hz

O I2S1 está fixo em 22050 Hz (ver `initExternalAudio` e o `M5.Speaker.config`
com `sample_rate = 22050`). MP3 quase sempre está em 44100 Hz (ou 48000/32000).
A saída do libhelix vem na taxa nativa do arquivo. Logo é preciso **resample**:

- **44100 → 22050** (caso dominante): decimação 2:1 trivial (média de 2 amostras
  ou drop com filtro simples).
- **48000/32000 → 22050**: resampler **linear** genérico (interpolação de primeira
  ordem), qualidade aceitável para um aparelho "retro".
- Decisão: **manter o I2S sempre em 22050** e resample em software. Re-inicializar
  o I2S a cada faixa (para casar a taxa nativa) violaria D2, causaria cliques e
  quebraria o `audioTask` compartilhado.

---

## 3. Estrutura de estados / UI nova (menus iPod-style + now playing)

### 3.1 Estados novos no `include/UiLogic.h`

```cpp
enum UiState {
  BOOT, HOME,
  VIDEO_LIBRARY, VIDEO_PLAYBACK,
  MUSIC_BROWSER,       // <-- novo: lista hierárquica (estilo iPod)
  MUSIC_NOW_PLAYING,   // <-- novo: capa + tags + progresso + controles
  AIRCRAFT_RADAR, SETTINGS, SYSTEM_INFO,
  SETUP_PORTAL, WEATHER, ERROR_SCREEN
};
```

`homeTarget` passa a mapear 6 seleções (ver §8.1). Sugestão de ordem no menu HOME:
`VIDEOS, MUSICA, TRAFEGO AEREO, CONFIGURACOES, SISTEMA, TEMPO` (MUSICA logo após
VIDEOS).

### 3.2 Modelo de dados do navegador

Estrutura mínima mantida em RAM (pequena, por diretório corrente):

```cpp
enum class MusicEntryKind { FOLDER, TRACK };
struct MusicEntry {
  MusicEntryKind kind;
  String path;      // caminho SD absoluto
  String name;      // nome de arquivo (ou ".." para subir)
  // tags (preenchidas só quando necessárias):
  String title, artist, album, year, track;
  bool   hasCover;
};
```

### 3.3 Tela `MUSIC_BROWSER` — lista estilo iPod

- **Layout** (RCA e LCD, espelhados como `drawLibrary`):
  - Cabeçalho: `MUSICA` + trilha ciano (`drawFastHLine(8, 36, 304, TFT_CYAN)`).
  - Caminho atual (breadcrumb) pequeno, ex.: `Artista / Album`.
  - Lista rolável de até 4 linhas visíveis (como `drawLibrary` paga 4 por página).
  - **"Roda de destaque"**: o roundrect ciano + texto azul-marinho já usados em
    `drawHome`/`drawLibrary` (`fillRoundRect(12, y-2, 296, 28, 4, TFT_CYAN)` com
    `> ` de prefixo). Não há touch wheel física; o destaque se move com BtnA/BtnC.
  - Rodapé com `drawControllerLabels("ACIMA", "OK", "ABAIXO")` (ou
    `"ANTERIOR","PLAY","PROXIMO"` no nível de faixas).
- **Semântica de navegação**:
  - `LEFT`/`RIGHT` = mover seleção (com auto-repeat já habilitado pelo
    `InputManager`).
  - `SELECT` em `FOLDER` = descer um nível (ou `..` = subir).
  - `SELECT` em `TRACK` = abrir e tocar → `MUSIC_NOW_PLAYING`.
  - `BACK` = subir um nível; na raiz (`/M5RETRO/music`) volta ao HOME.
- **Comportamento "tocar álbum/pasta"**: ao tocar uma faixa, monta a **fila** como
  a lista de faixas (`.mp3`/`.wav`) do diretório corrente, na ordem do diretório;
  `currentTrackIndex` aponta a faixa escolhida. Isso permite "próxima/anterior"
  sem reabrir a lista.

### 3.4 Tela `MUSIC_NOW_PLAYING`

- **RCA (experiência principal, 320×240)**:
  - Fundo `TFT_NAVY` com moldura/trilhas ciano (estilo VHS das outras telas).
  - Capa do álbum (64×64 ou 96×96) centralizada/esquerda, com borda ciano; sem
    capa → "quadrado vazio" desenhado com primitivas.
  - `fonts::Font4` (amarelo) para o **título**; `fonts::Font2` (branco) para
    **ARTISTA / ALBUM / ANO / FAIXA**.
  - Barra de progresso proporcional (estilo `uiHudDraw`): moldura ciano + fill
    amarelo; relógio `MM:SS / MM:SS` (reaproveitar o formato de `playbackClock`).
  - Indicadores de estado: `PLAY`/`PAUSA`, `REP: UMA|TODAS|N`, `ALEAT: S|N`.
- **LCD do Core2 (placa simples, como `startWeather`)**:
  - Título `MUSICA` + texto "EXIBINDO NA TV" + dica dos botões + `drawBackButton()`.
  - (A arte "grande" fica na TV; o LCD pode repetir a capa em miniatura, opcional.)
- **Redesenho**: a tela estática só é redesenhada quando muda (faixa/tag/capa);
  **apenas a barra de progresso e o relógio** são redesenhados a ~4 Hz, no mesmo
  espírito do `drawPlaybackOsd` (linhas 1230–1265), nunca a cada passada do loop.

---

## 4. Esquema de arquivos no SD

```
/M5RETRO/
  music/                          (raiz da biblioteca de música — nova)
    Artista A/
      Album 1/
        folder.jpg  (ou cover.jpg / front.jpg)   <- capa do álbum (JPEG)
        01-faixa.mp3
        02-faixa.mp3
        ...
      Album 2/
        cover.jpg
        faixa.wav                    <- WAV PCM 16-bit 22050 Hz (já suportado)
    Artista B/
      faixa-avulsa.mp3
```

Regras:

- A raiz é **`/M5RETRO/music`** (constante nova `MUSIC_ROOT`, a exemplo de
  `VIDEOS = "/M5RETRO/videos"`). `makeDirectories()` cria a pasta no boot.
- Uma entrada é **pasta** se `isDirectory()`, **faixa** se o nome termina em
  `.mp3` (case-insensitive) ou `.wav`.
- **Capa** por álbum: procurar, nesta ordem, `cover.jpg`, `folder.jpg`,
  `front.jpg`; senão a capa embutida ID3v2 `APIC` da faixa tocando (§5.3).
- **Sem banco de dados**: a hierarquia vem da própria árvore de pastas (D4).
  Tags ID3 são lidas sob demanda para exibição; nunca como índice global.
- Opcional (fase avançada): `playlist.m3u` simples em um diretório — um arquivo de
  texto com caminhos relativos, para reproduzir ordens não-alfabéticas.

---

## 5. Leitura de ID3 (v1 e v2) e extração de capa (APIC / folder.jpg)

### 5.1 Onde ler

- **ID3v2**: no **início** do arquivo (cabeçalho de 10 bytes + frames).
- **ID3v1**: nos **últimos 128 bytes** (marcador `"TAG"`).
- Tudo sob `sdMutex`, com `File.seek()`/`read()` de trechos pequenos (nunca o
  arquivo inteiro). Novo módulo `include/Id3.h` (funções puras, testáveis no
  `tests/` com stubs, no padrão de `PlaybackIO.h`).

### 5.2 ID3v1 (trivial — implementar primeiro)

```
pos = file.size() - 128
ler 128 bytes; validar memcmp(bytes, "TAG", 3)
title  = bytes[3..32]   (30)
artist = bytes[33..62]  (30)
album  = bytes[63..92]  (30)
year   = bytes[93..96]  (4)
comment= bytes[97..126] (30)
faixa  = se comment[28]==0 && comment[29]!=0 -> comment[29]  (ID3v1.1)
gênero = bytes[127]
```

Normalizar: cortar em `\0`, remover espaços à direita, converter para ASCII
imprimível (o bitmap font é ASCII-only — ver `asciiCopy`/`asciiUpper` no
`weather/src/main.cpp`).

### 5.3 ID3v2 (início — título/artista/álbum/ano/faixa + APIC)

Passo a passo do parser (v2.3 e v2.4):

1. **Cabeçalho (10 bytes)**:
   - `"ID3"` + versão (`major`, `revision`) + flags + **tamanho syncsafe** (4 bytes,
     7 bits cada) = tamanho total da tag **excluindo** o cabeçalho.
   - Flags: `0x10` (footer de 10 bytes no final, v2.4), `0x40` (extended header de
     tamanho variável, pular), `0x80` (unsynchronisation, v2.3).
2. **Varrer frames** dentro de `[10, 10 + tagSize)`:
   - Cada frame: **ID (4 bytes)** + **size (4 bytes)** + **flags (2 bytes)** +
     payload de `size` bytes.
   - `size`: em v2.3 é **big-endian simples**; em v2.4 é **syncsafe**. Interpretar
     conforme a `major` (3 vs 4).
   - `size == 0` ou ID vazio → padding, parar.
   - IDs de texto relevantes:
     - `TIT2` título, `TPE1` artista, `TALB` álbum, `TRCK` faixa,
       `TYER` (v2.3) ou `TDRC` (v2.4) ano, `TCON` gênero.
     - `APIC` — figura anexada (capa).
   - Flags de compressão/encryption: `0x00C0` (v2.3) ou `0x000C` (v2.4) →
     **ignorar** o frame (não implementar descompressão).
3. **Decodificar frame de texto**:
   - Byte 0 = encoding: `0` = ISO-8859-1, `1` = UTF-16 (com BOM), `2` = UTF-16BE,
     `3` = UTF-8.
   - Implementar, no mínimo, **ISO-8859-1 e UTF-8** (casos dominantes). UTF-16:
     detectar BOM e converter (pode ser fase opcional — a maioria dos rippers usa
     ISO-8859-1 ou UTF-8 em tags latinas).
   - Resultado final sempre **ASCII normalizado** (acentos removidos) para as
     fontes bitmap, como o projeto já faz.
4. **`APIC` (capa embutida)**:
   - Payload = `encoding(1) + mime(zero-term) + picType(1) + desc(zero-term,
     no encoding) + dados da imagem`.
   - `mime` = `image/jpeg`/`image/jpg` (ou `image/png` — PNG **não** será suportado;
     só JPEG). `picType` = 3 (front cover) é o preferido; aceitar 0 (other) como
     fallback.
   - Salvar **offset + tamanho** dos dados da imagem dentro do arquivo (não
     carregar em RAM ainda); a renderização lê e decodifica na hora (§6).

### 5.4 Precedência de capa (na tela now playing)

1. `cover.jpg` / `folder.jpg` / `front.jpg` na pasta do álbum.
2. `APIC` embutido na faixa tocando (se JPEG).
3. Fallback: quadrado desenhado com primitivas (ícone de nota musical via
   `fillTriangle`/`drawCircle`), mantendo o estilo programático do projeto.

### 5.5 Quando ler as tags

- **Browser**: exibir o **nome do arquivo** nas linhas (rápido, sem abrir arquivo);
  ler tags **apenas** da linha destacada para o "painel" lateral/breadcrumb, se
  desejado (uma leitura por movimento — aceitável). Alternativa (fase avançada):
  uma **tarefa de indexação** em background (prioridade idle, core 1) preenche um
  cache em PSRAM do diretório corrente, limitado a ~64 entradas.
- **Now playing**: ler ID3v2 (início) e, se necessário, ID3v1 (fim) **uma vez** ao
  abrir a faixa; guardar em um struct `TrackMeta`.

---

## 6. Renderização da arte do álbum (JPEG → RGB565, cache em PSRAM)

Reutiliza o **JPEGDEC@1.8.2** já embutido e o mesmo padrão de `drawStaticPoster`
(linhas 721–779) e `posterDraw` (714–719).

### 6.1 Fluxo

1. **Localizar** a fonte da capa (§5.4) → obter `offset`/`size` (APIC) ou abrir
   `cover.jpg`.
2. **Limitar o tamanho** do JPEG de entrada a, ex., **128 KB** (`MAX_JPEG`, já
   existe). Capas maiores caem no fallback (evita buffer enorme em PSRAM).
3. **Ler o JPEG para PSRAM** com `ps_malloc(size)` (mesmo padrão do `jpegBuffer` em
   `setup`, linha 2323). Não usar SRAM/DMA.
4. **Decodificar escalado**:
   - `jpeg.openRAM(buf, size, coverDraw)`.
   - `jpeg.setPixelType(RGB565_LITTLE_ENDIAN)`.
   - Usar `jpeg.decode(x, y, JPEG_SCALE_QUARTER | JPEG_SCALE_EIGHTH)` conforme o
     tamanho nativo: capas ~300–600 px reduzem para ~64–96 px com 1/4 ou 1/8.
   - O callback `coverDraw(JPEGDRAW*)` escreve os blocos num `LGFX_Sprite` PSRAM
     de 64×64 (ou 84×84) RGB565, via `pushImage` (como `posterDraw`).
5. **Ajuste final de tamanho/aspecto**: `sprite.pushRotateZoom(&rca, ...)` para
   centralizar/escalar (técnica já usada em `drawStaticPoster`), ou desenhar direto
   no sprite com `fillRect` para letterbox.
6. **Cache**: manter **uma** capa decodificada em PSRAM, com chave = caminho do
   álbum; invalidar ao trocar de faixa/álbum. (64×64×2 = 8 KB; 96×96×2 = 18 KB —
   irrelevante diante dos 4 MB de PSRAM.)

### 6.2 Memória da arte

| Item | Onde | Tamanho |
|---|---|---|
| JPEG de entrada (cover.jpg/APIC) | PSRAM (`ps_malloc`) | ≤ 128 KB, temporário |
| Sprite RGB565 64×64 | PSRAM (`setPsram(true)`) | 8 KB |
| Sprite RGB565 96×96 | PSRAM | 18 KB |
| SRAM/DMA | — | **zero** (preservada para I2S/CVBS) |

> Nota de precisão: o código atual cria o `rca` com
> `M5ModuleRCA::use_psram_t::psram_no_use` e o `PLANO_E_REVISAO.md` registra que o
> buffer de vídeo composto foi **movido para SRAM** para eliminar cópia contínua de
> PSRAM. Independentemente dessa distinção, a regra do plano é: **SRAM é cara e
> DMA; PSRAM é farta e CPU-only** — usar PSRAM para capa/JPEG scratch e tags.

---

## 7. Modelo de tarefas FreeRTOS para streaming + UI responsiva

### 7.1 Abstração recomendada: um único `audioTask` generalizado

Mínimo de mudança no código comprovado. O `audioTask` (core 0, prioridade 4, hoje
em `src/main.cpp` 471–637) passa a consumir de uma **`PcmSource`** que devolve
blocos de **PCM 22050 Hz estéreo**, em vez de ler WAV diretamente:

```cpp
struct PcmSource {                 // conceito (interface)
  virtual ~PcmSource();
  virtual bool begin(const String &path) = 0;   // abre + prepara cabeçalho/frames
  virtual size_t read(int16_t *dst, size_t samples) = 0; // 0 => fim (ou loop)
  virtual bool seekTo(uint32_t second) = 0;     // opcional (fase de seek)
  virtual void close() = 0;
  virtual uint32_t totalSamples() const = 0;    // p/ progresso (estimado p/ MP3)
};
```

Implementações:

- **`WavSource`** — extrai a lógica atual de `openWavAndReadHeader` + leitura de
  `wavFile` (posição `wavDataStart..wavDataEnd`, `wavBlockAlign`). Comportamento
  idêntico ao atual, inclusive o loop do Weather (`weatherAudio`).
- **`Mp3Source`** — usa libhelix:
  1. Lê do `File` em bloco (4–8 KB) sob `sdMutex`.
  2. Sincroniza no **frame header** (0xFF + `(b & 0xE0) == 0xE0`, 11 bits set).
  3. Extrai taxa de bits/taxa de amostragem da tabela MPEG para saber o tamanho do
     frame; copia o frame completo para um buffer contíguo.
  4. `decode()` → PCM (taxa nativa, 1 ou 2 canais, 16-bit).
  5. **Resample** 44100→22050 (ou 48000/32000→22050), **duplicar mono→estéreo**,
     **`scalePcm`** com `playbackVolume`.
  6. Devolve até `AUDIO_CHUNK/4` amostras estéreo por chamada.

O `audioTask` então continua: `i2s_write`/`playRaw`, pacing por relógio PCM,
`audioUnderruns`, `samplesPlayed`, `audioIdle`. **As linhas de pacing e underrun
não mudam.** Troca-se apenas a origem dos dados.

### 7.2 Alternativa (se medir jitter de decodificação)

Separar em **produtor/consumidor** com anel em PSRAM:

- `mp3DecodeTask` (core 0, prioridade 3): decodifica MP3 → PCM 22050 → anel.
- `audioTask` (core 0, prioridade 4, inalterado no pacing): consome o anel e
  escreve I2S.

Usar apenas se o `PcmSource` em linha única mostrar underruns (improvável: libhelix
decodifica bem mais rápido que tempo real). Não é o caminho inicial.

### 7.3 Flags atômicas novas

Seguir o padrão `std::atomic` existente:

```cpp
std::atomic<bool> musicPlaying{false}, musicPaused{false}, musicFinished{false};
std::atomic<int>  musicRepeat{0};   // 0=off 1=uma 2=todas
std::atomic<bool> musicShuffle{false};
std::atomic<uint32_t> musicSamplesPlayed{0}, musicTotalSamples{0};
std::atomic<bool> musicAudio{false}; // análogo a weatherAudio (loop)
```

> O `playing`/`paused` globais já servem ao vídeo; para evitar colisão de
> significado, o player de música usa flags próprias **ou** reutiliza `playing`
> (garantindo que música e vídeo nunca estejam ativos ao mesmo tempo — o que é o
> caso, pois cada um chama o `stop` do outro). Recomendação: reutilizar `playing`
> para "há áudio em curso" (para o `serviceWiFi` continuar pausando Wi-Fi) e
> adicionar `musicPlaying` para a semântica específica do player.

### 7.4 Ciclo de vida (para não conflitar com vídeo/weather)

- `startMusic(path)`:
  1. `stopProgram()`/`stopWeather()` se ativos (garantir I2S1 livre e `playing`
     limpo).
  2. Montar fila do diretório, `currentTrackIndex`.
  3. Criar `Mp3Source`/`WavSource`, `begin()`.
  4. `musicPlaying = true; playing = true;` desenhar now playing.
- `musicTick()` (chamado em `loop()`, como `videoTick`):
  - Se `musicFinished` → auto-avançar (repeat/shuffle) ou parar.
  - Redesenhar progresso/relógio a ~4 Hz.
- `stopMusic()`: `musicPlaying = false`, aguardar `audioIdle` (padrão de
  `stopProgram`), `source.close()` sob `sdMutex`, `state = MUSIC_BROWSER`.

---

## 8. Integração com menu principal, navegação e toque

Pontos exatos a alterar (localizações no `src/main.cpp` atual). **Nenhuma edição
será feita neste documento** — apenas o mapa da mudança.

### 8.1 `include/UiLogic.h`

- `enum UiState`: adicionar `MUSIC_BROWSER`, `MUSIC_NOW_PLAYING` (§3.1).
- `homeTarget(selection)`:
  ```cpp
  const UiState targets[] = { VIDEO_LIBRARY, MUSIC_BROWSER, AIRCRAFT_RADAR,
                              SETTINGS, SYSTEM_INFO, WEATHER };
  return selection >= 0 && selection < 6 ? targets[selection] : HOME;
  ```

### 8.2 `include/LocalizationPTBR.h`

Novas strings: `MUSICA = "MUSICA"`, `SEM_MUSICAS = "SEM MUSICAS NO CARTAO"`,
`ARTISTA = "ARTISTA"`, `ALBUM = "ALBUM"`, `FAIXA = "FAIXA"`, `ANO = "ANO"`,
`REPETIR = "REPETIR"`, `ALEATORIO = "ALEATORIO"`, `CARREGANDO_CAPA = "..."`, etc.

### 8.3 `src/main.cpp` — `drawHome()` (linhas 1332–1363)

- `items[]`: adicionar `PTBR::MUSICA` → 6 itens.
- Loops `for (int i = 0; i < 5; i++)` (LCD e RCA) → `i < 6`.
- **Ajustar geometria** para caber 6 itens acima da barra de labels (y=184):
  - Sugestão: `int y = 42 + i * 22` com roundrect de 20 px de altura (LCD
    `fillRoundRect(12, y-2, 296, 20, 4, ...)`), mantendo `setTextSize(2)`; ou
    reduzir `setTextSize` para 1 no LCD. No RCA (que usa `setTextSize(1)`), manter
    espaçamento equivalente.
  - Os números mágicos `44` (y inicial) e `28` (passo) **devem** casar com o mapa
    de toque em `handleTouch` (§8.5).

### 8.4 `src/main.cpp` — `handleNavigation()` (linhas 1845–2036)

- No bloco `state == HOME` (1878–1899): trocar os módulos `% 5` por `% 6` e os
  `(homeSelection + 4)`/`+ 1`; no `SELECT`, tratar `MUSIC_BROWSER` → chamar
  `drawMusicBrowser()` (e inicializar o navegador na raiz de música).
- Adicionar dois blocos novos:
  - `state == MUSIC_BROWSER`: navegação de lista (LEFT/RIGHT = mover, SELECT =
    descer/tocar, BACK = subir/HOME) — espelho do bloco `VIDEO_LIBRARY`
    (1900–1922).
  - `state == MUSIC_NOW_PLAYING`:
    - `SELECT`/`PLAY_PAUSE` → `musicPaused = !musicPaused`.
    - `LEFT`/`RIGHT` (toque curto) → faixa anterior/próxima (`stopMusic` +
      `startMusic(fila[idx±1])`).
    - `PREVIOUS`/`NEXT` (toque longo lateral) → **seek** (mapeamento já emitido
      pelo `InputManager` quando auto-repeat está desligado — ver §8.6).
    - `BACK` → `stopMusic()` + voltar ao `MUSIC_BROWSER`.
- `HOME` global (1869–1877): também chamar `stopMusic()` (além de
  `weatherAudio=false` e `stopProgram`).

### 8.5 `src/main.cpp` — `handleTouch()` (linhas 2044–2099)

- Mapa do HOME (linha 2092–2094): `homeSelection = (p.y - 44) / 28;` e a faixa
  `p.y >= 44 && p.y < 181` → atualizar para o novo passo (ex.: `(p.y - 42) / 22`
  e `p.y < 174`) e limitar a `0..5`.
- `MUSIC_BROWSER`: região da lista (acima de y=184) → mover seleção para a linha
  tocada e/ou `SELECT`; `drawControllerLabels` já cobre a barra inferior.
- `MUSIC_NOW_PLAYING`: toque na imagem = `PLAY_PAUSE` (padrão do vídeo); botões
  laterais = anterior/próxima; barra de progresso tocável = seek (fase avançada).
- O `drawBackButton()`/`isBackButton` (canto sup. direito) já é genérico e serve
  ao novo estado sem mudanças.

### 8.6 Mapeamento físico dos 3 botões (sem mudar `InputManager`)

Como o `InputManager` já emite `LEFT`/`RIGHT` no toque curto e `PREVIOUS`/`NEXT` no
toque longo lateral (quando auto-repeat desligado), e já emite `HOME` no toque
longo central, o player de música aproveita:

| Botão | Toque curto | Toque longo (≥700 ms) |
|---|---|---|
| BtnA (esq) | Faixa anterior / mover seleção | Seek −10 s |
| BtnB (centro) | Play/Pause / OK | BACK (≥700 ms) / HOME (≥1500 ms) |
| BtnC (dir) | Faixa seguinte / mover seleção | Seek +10 s |

- Em `loop()`, `input.setAutoRepeat(state != VIDEO_PLAYBACK)` (linha 2404) deve
  passar a **desligar também em `MUSIC_NOW_PLAYING`** (para que segurar o lado emita
  um único `PREVIOUS/NEXT` = seek, em vez de repetir `LEFT/RIGHT`). No browser,
  auto-repeat **ligado** (lista longa).
- `NavAction::SEEK_BACKWARD/SEEK_FORWARD` já existem no enum; podem ser adotados
  como sinônimos semânticos se preferir clareza, mas **não são obrigatórios** — o
  par `PREVIOUS/NEXT` no now playing já resolve seek sem tocar no `InputManager`.

### 8.7 `loop()` (linhas 2398–2491)

- Adicionar, no bloco de estados, `if (state == MUSIC_NOW_PLAYING) musicTick();`.
- `musicTick()`: progresso a ~4 Hz + tratamento de `musicFinished` (auto-advance/
  repeat/shuffle) — análogo ao bloco `state == VIDEO_PLAYBACK` (2451–2466).

### 8.8 Diagnóstico USB (`serviceDiagnostics`, linhas 2196–2293)

Adicionar comandos para teste, no estilo existente: `diag music`, `diag music
next`, `diag music previous`, `diag music pause`, `diag music stop`, `diag music
toggle shuffle`. Reusar `handleNavigation`/`startMusic` como os comandos `diag
play`/`diag radar` já fazem.

---

## 9. Riscos e mitigações

| Risco | Impacto | Mitigação |
|---|---|---|
| **SRAM/DMA insuficiente** (heap livre ~66–72 KB, RCA/CVBS + I2S já consomem) | Crash/underrun de áudio ou vídeo | Toda alocação nova em **PSRAM** (`ps_malloc`, `LGFX_Sprite::setPsram(true)`); frame MP3 comprimido em PSRAM; nunca ampliar buffers DMA. Medir `ESP.getFreeHeap`/`getMinFreeHeap` via `diag status`. |
| **CPU de decodificação** competindo com CVBS/I2S no core 0 | Underruns de áudio | libhelix é leve (< ~10% p/ 128 kbps); manter prioridade do `audioTask`; medir `audioUnderruns`. Se necessário, adotar produtor/consumidor (§7.2) ou baixar bitrate das faixas. |
| **Tamanho do firmware** | Flash de 16 MB — folga grande | +30–50 KB irrelevantes (app atual 1.34 MB). Confirmar link após adicionar a lib. |
| **Resampling (44100/48000/32000 → 22050)** | Som errado/estourado | Decimação 2:1 para 44100; resampler linear genérico; validar com clipe sintético no `tests/`. |
| **SD lento** (SPI compartilhado com LCD) | Underruns/atraso de UI | Ler em blocos ≥4 KB sob `sdMutex` (padrão atual); ler tags/capa **uma vez por faixa**, não por redesenho; exibir nome de arquivo no browser (sem tag por linha). |
| **Latência de UI durante leitura de capa/tag** | Navegação engasgada | Leitura de capa sob demanda e limitada a 128 KB; decodificação escalada (rápida); não fazer no caminho de desenho a cada frame. |
| **Tags não-ASCII / encodings** (UTF-16, latin-1) | Caracteres quebrados nas fontes bitmap | Normalizar para ASCII (`asciiCopy`), suportar ISO-8859-1 + UTF-8, UTF-16 opcional; nunca deixar bytes não-imprimíveis chegarem a `drawString`. |
| **APIC PNG ou JPEG gigante** | RAM/parsing falho | Só JPEG; limitar a 128 KB; fallback para `cover.jpg` e depois para ícone desenhado. |
| **Conflito de posse do I2S1 / `wavFile`** entre vídeo, weather e música | Crash/duplo uso | `startMusic` chama `stopProgram`/`stopWeather`; flags `playing`/`musicPlaying` mutuamente exclusivas; ciclo de vida com espera por `audioIdle` (padrão `stopProgram`). |
| **Auto-advance/shuffle sem duração exata do MP3** | Barra de progresso imprecisa | Usar relógio de amostras decodificadas para o decorrido; estimar total por bitrate×tamanho (ou parse opcional do cabeçalho **Xing/Info** no 1º frame para duração exata). |
| **Licença RPSL do libhelix original** | Obrigação de redistribuir código | Usar `esp-libhelix-mp3` (MIT) se licença for restrição. |

---

## 10. Plano de implementação em fases (incrementos testáveis)

Cada fase é compilável e testável isoladamente (`pio run` + `diag ...` + `tests/`).

| Fase | Escopo | Entregável testável | Esforço estimado |
|---|---|---|---|
| **F0 — Estrutura** | Novos estados, `homeTarget` 6 itens, strings PTBR, `drawHome` com 6 itens, tela vazia `MUSIC_BROWSER`/`MUSIC_NOW_PLAYING`, `makeDirectories` cria `/M5RETRO/music` | HOME com 6 itens navegáveis; entrar em "MUSICA" mostra placeholder | 0.5–1 dia |
| **F1 — Navegação por pastas + WAV** | `PcmSource` + `WavSource` (refatorar `audioTask` sem mudar pacing); `MusicBrowser` percorre `/M5RETRO/music`; play/pause/prev/next de WAV; fila do diretório | Tocar WAVs da árvore de pastas com lista iPod | 1–2 dias |
| **F2 — Tags ID3** | `include/Id3.h`: ID3v1 + ID3v2 texto (TIT2/TPE1/TALB/TRCK/TYER/TDRC); tela now playing com título/artista/álbum/ano/faixa | Tags corretas no now playing (arquivos de teste no `tests/`) | 1–2 dias |
| **F3 — MP3** | `lib_deps` + `Mp3Source` (libhelix + resampler 22050 + mono→stéreo); seek básico (WAV exato; MP3 aproximado por sync); auto-advance | Tocar MP3 128–320 kbps/44100; seek; next/prev | 2–3 dias |
| **F4 — Capa do álbum** | `APIC` + `folder.jpg`/`cover.jpg`; JPEGDEC escalado → sprite PSRAM 64×64/96×96; cache; fallback desenhado | Capa aparece no now playing | 1.5–2 dias |
| **F5 — Polimento** | Shuffle/repeat (opções no browser), volume em tela, comandos `diag music *`, barra de progresso tocável (opcional), `README`/`PINOUT`/docs, revisão de RAM/CPU no aparelho | Build completo + teste físico no Core2/RCA | 1.5–2 dias |

**Total estimado:** ~7–10 dias de trabalho, mais teste físico de bancada (PAL-M,
áudio analógico, medição de underruns e heap).

### Critérios de aceite

1. HOME lista 6 itens e entra em MUSICA sem regressão nos outros 5.
2. Navegar `Artista/Album/faixa` por pastas, com destaque estilo iPod (roundrect
   ciano) e auto-repeat.
3. Tocar `.wav` (regressão: igual ao comportamento atual) e `.mp3` com play/pause,
   anterior/próxima, seek (toque longo), sem bloquear a UI (`diag status` em
   tempo real).
4. Tags ID3 (v1 e v2) exibidas; capa (folder.jpg/APIC) em RGB565 no LCD/RCA.
5. `heap` livre e `audioUnderruns` estáveis durante reprodução longa (medir via
   `diag status`), sem underruns novos.

---

## Resumo das decisões-chave

- **Decoder**: libhelix (16-bit inteiro, baixo RAM/CPU), via `arduino-libhelix`
  (ou a reimplementação MIT `esp-libhelix-mp3`). **Não** usar `ESP32-audioI2S`.
- **WAV mantido** como caminho zero-custo; MP3 adicionado atrás de uma
  **`PcmSource`** que entrega PCM 22050 Hz estéreo ao `audioTask` existente
  (pacing/underrun/volume inalterados).
- **Resample em software** para 22050 Hz (44100 → decimação 2:1; demais → linear).
- **UI**: estados novos `MUSIC_BROWSER` + `MUSIC_NOW_PLAYING`; navegação por
  **sistema de arquivos** (pastas = Artista/Álbum), tags lidas sob demanda.
- **Capa**: JPEGDEC embutido, decodificação escalada para sprite **PSRAM** RGB565
  64×64/96×96, cache de uma capa; fonte `cover.jpg`/`folder.jpg` → `APIC`.
- **Integração**: `homeTarget` passa a 6 itens; ajustes em `drawHome`,
  `handleNavigation`, `handleTouch`, `loop`, `serviceDiagnostics`; toque longo
  lateral já resolve **seek** sem alterar o `InputManager`.
- **Memória**: tudo novo em PSRAM; SRAM/DMA preservada para CVBS/I2S.

**Arquivo do plano:** `/Users/eliel/Documents/TV/docs/mp3-player-plan.md`
