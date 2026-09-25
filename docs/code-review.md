# Revisão de Código — Firmware M5 RETRO TV

> **Histórico do upstream.** Documento do [m5-retro-tv](https://github.com/eliel9012/m5-retro-tv)
> (M5Stack Core2 + módulo RCA), preservado como referência. Pinos, barramentos e
> números aqui são do Core2, não do Fruit Jam — para este fork, veja `AGENTS.md`
> e `PORTING.md`.

Documento de revisão completa do firmware M5 RETRO TV, descrevendo os problemas
encontrados, as correções aplicadas e o estado final do projeto.

---

## 1. Escopo da revisão

Arquivos revisados:

- `src/main.cpp` — laço principal, navegação, reprodução de mídia e TV ("radar").
- `include/*.h` — cabeçalhos públicos (`Config.h`, `Id3.h`, `UiLogic.h`,
  `InputManager.h`, `NetworkManager.h`, `SecretsManager.h`, `ConfigurationPortal.h`,
  `PlaybackIO.h`, `SafeStorage.h`, `Ascii.h`, `LocalizationPTBR.h`).
- `src/InputManager.cpp` — leitura de teclado/controles e normalização de eventos.
- `src/NetworkManager.cpp` — Wi‑Fi, NTP e API de clima.
- `src/SecretsManager.cpp` — persistência segura de credenciais/informações.
- `src/ConfigurationPortal.cpp` — portal de configuração embutido.
- `include/Id3.h` — parsing de tags ID3 (v1 e v2).

---

## 2. Bugs CRÍTICOS encontrados e corrigidos

### 2.1 MP3 com tag ID3v2 travava o device

**Sintoma:** ao reproduzir um MP3 contendo tag ID3v2 no início do arquivo, o
device travava (congelava) durante a reprodução.

**Causa:** o decoder libhelix (`MP3Decode`) não pula a tag ID3v2 automaticamente.
A função `mp3ReadPcm` alimentava o decoder com os bytes da tag, que eram
descartados sem avanço real (`consumed == 0`), fazendo `mp3ReadPcm` entrar em
*busy-loop*: o decoder nunca recebia dados de áudio válidos, mas a rotina também
não avançava o cursor do arquivo.

**Correção:**
- Antes de criar o decoder, localizar o início real do fluxo de áudio com
  `id3::audioStart()` e posicionar o arquivo com `mp3File.seek(audioStart)`.
- Descartar 1 byte em quadro corrompido, de modo que a leitura sempre avance
  mesmo diante de dados inválidos — quebrando o ciclo de `consumed == 0`.

### 2.2 Estado de MP3 vazava entre reproduções

**Sintoma:** após tocar um MP3 e voltar para HOME/diagnóstico/ir-ao-vídeo, o
áudio do vídeo saía do MP3 antigo (arquivo residual), em vez do áudio do vídeo.

**Causa:** `stopProgram` não chamava `mp3End()`. O flag `mp3Mode` permanecia
`true`, e o pipeline de áudio continuava apontando para o stream anterior.

**Correção:** `mp3End()` passou a ser chamado dentro de `stopProgram()`,
garantindo o encerramento completo do decoder e a liberação do estado de MP3.

### 2.3 Corrida no acesso ao SD (startMusic)

**Sintoma:** risco de *data race* ao iniciar música, com acesso concorrente ao
cartão SD sem a proteção do mutex.

**Causa:** `startMusic` setava `playing = true` **antes** das leituras de SD
(reconstrução da fila de reprodução e leitura da capa/álbum), sem `sdMutex`,
expondo as estruturas a acesso simultâneo.

**Correção:** a atribuição `playing = true` foi adiada para **depois** das
leituras de SD, que passam a ocorrer com o `audioTask` ainda ocioso (sem ler o
SD concorrentemente).

### 2.4 MP3 nunca tocava (guard de pausa do `audioTask`)

**Sintoma:** a reprodução de MP3 não emitia som algum — o áudio simplesmente
não saía.

**Causa:** o guard de pausa do `audioTask` era
`(!playing && !weatherAudio) || paused || !wavFile`. No modo MP3 o arquivo aberto
é `mp3File` (o `wavFile` fica nulo), então `!wavFile` era sempre `true` e o task
permanecia em pausa, sem nunca chegar ao laço de decodificação (`mp3ReadPcm`).

**Correção:** a condição passou a ser `!wavFile && !mp3Mode`, de modo que o MP3
(com `mp3Mode = true` e `wavFile = null`) sai da pausa e chega ao decoder.

### 2.5 Retorno de `MP3Decode` tratado como contagem de amostras

**Sintoma:** mesmo com o guard de pausa corrigido, o MP3 terminava "na hora"
(sem emitir som), pois o buffer PCM nunca era preenchido.

**Causa:** `MP3Decode()` retorna **código de erro** (`0` = sucesso, `< 0` = erro),
não o número de amostras. O código fazia `const int samps = MP3Decode(...)` e
`if (samps > 0) mp3PcmCount = samps;` — como em sucesso o retorno é `0`, a
condição nunca era verdadeira e o frame decodificado era descartado.

**Correção:** usar `err == 0` para detectar sucesso e obter a quantidade real de
amostras via `MP3GetLastFrameInfo().outputSamps` (confirmado por teste host do
libhelix: retorno 0 + `outputSamps=2304` por frame MPEG1 estéreo).

---

## 3. Bugs ALTOS corrigidos

### 3.1 `weatherStatus` com corrida entre task e loop

`weatherStatus` era um `char[32]` atualizado pela task assíncrona e lido pelo
laço principal sem sincronização.

**Correção:** passou a ser `std::atomic<const char*>`, tornando a troca do
ponteiro atômica e eliminando a leitura de buffer parcialmente escrito.

### 3.2 `drawLibrary` re-varria o SD a cada tecla

`drawLibrary` realizava uma varredura completa do SD em cada evento de tecla,
provocando lentidão perceptiva na navegação da biblioteca.

**Correção:** a lista de programas foi cacheada nos membros `libraryPaths`,
`libraryCount` e `libraryScanned`, populada uma única vez por `scanLibrary()`
e reutilizada após o primeiro scan.

### 3.3 NTP apenas quando `!playing`

O `configTime()` (sincronização NTP) só era solicitado quando `!playing`, o que
atrasava indefinidamente a correção do relógio durante reprodução contínua.

**Correção:** `configTime()` passou a ser pedido **mesmo durante** o playback,
independentemente do estado de `playing`.

---

## 4. Limpezas / problemas MÉDIOS corrigidos

- **Dead code `PREVIOUS`/`NEXT`:** os códigos legados `PREVIOUS` e `NEXT` foram
  removidos e normalizados para `LEFT`/`RIGHT` no topo da função `handleNavigation`.
- **`drawControllerLabels` duplicado no radar:** a chamada era feita duas vezes
  na tela do radar; removida a redundância.
- **Self-assignment frágil de `currentTitle`:** `startProgram` atribuía
  `currentTitle` a partir de `c_str()` de um buffer temporário. A cópia foi
  tornada estável, evitando ponteiro pendurado/self-assignment.
- **Log spam de `isProgramFolder`:** a função emitia log no Serial a cada
  verificação; o log foi suprimido para evitar inundação do monitor serial.

---

## 5. Melhorias de UI/UX SUGERIDAS (AINDA NÃO aplicadas)

Lista de melhorias futuras identificadas durante a revisão, sem implementação
nesta etapa:

- **Truncar títulos/artistas/álbuns longos** no *now playing* — atualmente
  estouram visualmente à direita.
- **Duração/progresso do MP3** estimado fixo em 128 kbps; usar o bitrate real do
  1º quadro ou dos headers `Xing`/`VBRI`.
- **Shuffle/repeat, seek** (`SEEK_BACKWARD`/`SEEK_FORWARD` já existem no enum),
  **número da faixa** ("3/8") e **ajuste de volume durante a música**.
- **Item "IDIOMA" morto** no menu de configurações — remover ou ligar ao
  localização real (ver `LocalizationPTBR.h`).
- **Duas implementações de ASCII** convivem: `asciiCopy` (no weather) descarta
  acentos, enquanto `ascii::normalize` (em `Ascii.h`) mapeia caracteres.
  Unificar em uma única rotina.
- **Home na RCA vs. LCD** desenhadas de forma diferente — padronizar o layout.
- **Timestamp "atualizado há Xs"** no radar/weather — exibir idade da última
  atualização de dados.

---

## 6. Testes

- `test_id3.cpp` foi integrado ao `run.sh` da suite nativa.
- Novo teste cobrindo `id3::audioStart` (offset do início do audio após ID3v2).
- Suite nativa completa verde:
  MJPEG · WAV · PCM · storage · navigation · input · network · portal · id3 · schematik.

---

## 7. Estado final

- Build limpo (sem warnings/erros).
- Testes nativos verdes.
- Commit de revisão aplicado ao repositório.

---

## Resumo

A revisão eliminou cinco bugs críticos (travamento por ID3v2, vazamento de estado
de MP3, corrida no SD, o guard de pausa que impedia o MP3 de tocar e o retorno de
`MP3Decode` tratado como contagem de amostras), três bugs altos (raça no
`weatherStatus`, re-varredura do SD na biblioteca e NTP condicionado a `!playing`)
e quatro problemas médios (dead code de navegação, chamada duplicada, atribuição
frágil e log spam), deixando a suíte de testes intacta e o projeto em estado
estável.