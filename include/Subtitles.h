#pragma once
// ============================================================================
// Subtitles — legendas .srt no estilo do closed caption da linha 21 do NTSC.
//
// Por que existe: o par MJPEG+WAV do cartao nao carrega legenda nenhuma. Quem
// prepara o video (tools/prepare_video.py) pode deixar um `.srt` ao lado do
// `.mjpeg`, e este modulo o le enquanto o filme roda.
//
//   /M5RETRO/videos/foo/video.mjpeg  ->  /M5RETRO/videos/foo/video.srt
//
// Vocabulario visual reproduzido — o closed caption de verdade, nao a legenda
// de cinema: caixa preta opaca atras de cada linha, texto branco puro em caixa
// alta, no maximo duas linhas, ancorado na base. Nada de ciano: croma saturado
// (TFT_CYAN / 0x07FF) faz dot crawl na saida composta, e o CC original era
// branco no preto justamente porque era o que sobrevivia ao sinal.
//
// ---------------------------------------------------------------------------
//  Tres decisoes que valem explicacao
// ---------------------------------------------------------------------------
//
// 1) LEITURA SO PARA A FRENTE, SEM INDICE. Um .srt de um filme longo tem
//    milhares de cues; indexar todos custaria dezenas de KB de SRAM, que e o
//    recurso escasso aqui (ver AGENTS.md secao 2.2). Este leitor guarda no
//    maximo DUAS cues: a exibida e a proxima. O resto fica no cartao. Custo:
//    nao da para voltar. Depois de qualquer salto para tras (reiniciar o
//    programa, trocar de video) o chamador faz `arquivo.seek(0)` e `reset()`.
//
// 2) O RELOGIO E O DO AUDIO, NAO O `millis()`. O firmware ja acerta o video por
//    `samplesPlayed` (ver videoTick em src/main.cpp); a legenda usa o mesmo
//    relogio ou desanda junto com o video nos quadros descartados. Por isso a
//    API recebe `positionMs` pronto e nunca chama millis():
//
//      const uint32_t posMs = sampleRate ? uint32_t(uint64_t(samplesPlayed.load())
//                                                   * 1000ULL / sampleRate) : 0;
//
// 3) O CHAMADOR E DONO DO sdMutex. Este header NAO toma o mutex — nao inclui
//    FreeRTOS nem SD, justamente para compilar tambem no simulador de desktop.
//    `update()` recebe o `File&` ja aberto e **exige que o chamador segure o
//    sdMutex durante a chamada** (uma tomada por chamada, devolvida logo
//    depois — nunca segurar por mais tempo, senao o audio passa fome). Em
//    troca, `update()` faz pouquissima leitura: 192 bytes por bloco e no maximo
//    `kCuesPerUpdate` cues por chamada. `draw()` NAO toca no cartao.
//
// ---------------------------------------------------------------------------
//  Orcamento
// ---------------------------------------------------------------------------
//  sizeof(Subtitles) ~ 330 bytes (static_assert abaixo trava em 1 KB), sem
//  alocacao dinamica, sem String e sem LGFX_Sprite. A instancia deve ser
//  global/estatica: um objeto destes na pilha de uma tarefa que faz HTTPS ja
//  seria metade do respiro do handshake (AGENTS.md secao 2.3). `update()` usa
//  ~280 bytes de pilha em buffers locais; roda no loop() (8192), onde cabe.
//
// ---------------------------------------------------------------------------
//  Conflito com o OSD do player
// ---------------------------------------------------------------------------
//  A caixa da legenda ocupa y 168..199, e o OSD de videocassete (VcrOsd.h)
//  ocupa 148..221. Eles se sobrepoem de proposito: os dois querem a base da
//  tela, como na TV de verdade. Quem chama decide a precedencia — o caminho
//  recomendado e esconder a legenda enquanto `drawPlaybackOsd()` estiver
//  visivel (o OSD e transitorio, 3 s), simplesmente nao chamando `draw()`.
//
// ---------------------------------------------------------------------------
//  Limitacao conhecida da fonte
// ---------------------------------------------------------------------------
//  A VcrFont tem 45 glifos: espaco, 0-9, A-Z e : . - / % + ? ! — nao tem
//  virgula. Legenda e texto cheio de virgula, e cada uma sai como um espaco
//  (glyphIndex devolve SPACE para o que nao existe). O texto NAO e alterado
//  aqui de proposito: trocar ',' por '.' seria mentir na pontuacao. Resolver
//  de verdade custa um glifo novo na VcrFont.h (16 linhas de tabela, 32 bytes
//  de flash) e uma entrada em glyphIndex — fora do escopo deste header.
//
// Dependencias: SafeArea.h, VcrFont.h (que traz a LovyanGFX) e Ascii.h. Nada de
// Arduino, SD ou FreeRTOS — o mesmo header compila no firmware (gnu++11) e no
// simulador SDL.
// ============================================================================

#include "Ascii.h"
#include "SafeArea.h"
#include "VcrFont.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace subs {

// ---------------------------------------------------------------------------
//  Geometria — ancorada na base da area segura, ACIMA da barra dos botoes
// ---------------------------------------------------------------------------
namespace layout {

constexpr int kScale = 1;                 // 12x16 na tela; CC nunca foi grande
constexpr int kLineH = vcrfont::CELL_H;   // 16
constexpr int kMaxLines = 2;              // o CC da linha 21 tambem parava em 2
constexpr int kPadX = 4;                  // respiro da caixa preta, por lado
constexpr int kGapToBar = 2;              // folga ate a barra de legendas

// Colunas que cabem na largura segura com a caixa preta inclusa:
// (272 - 8) / 12 = 22 caracteres.
constexpr int kMaxCols = (crt::SAFE_W - 2 * kPadX) / (vcrfont::CELL_W * kScale);

constexpr int kBlockH = kMaxLines * kLineH;       // 32
constexpr int kBottom = crt::BAR_Y - kGapToBar;   // 200 — primeira linha livre
constexpr int kTop = kBottom - kBlockH;           // 168

// A celula da VcrFont ja tem 2 px de folga em cima e embaixo, entao a caixa
// preta nao precisa de padding vertical proprio: sai com a altura da celula.
static_assert(kBottom <= crt::BAR_Y, "legenda invade a barra dos botoes");
static_assert(kTop >= crt::SAFE_T, "legenda escapa pelo topo da area segura");
static_assert(kMaxCols >= 10, "area segura estreita demais para uma legenda");

} // namespace layout

// Cores em RGB565, escritas na mao (o header nao depende das macros TFT_*).
// Branco no preto: o par do CC original e o unico que nao sofre dot crawl.
constexpr uint16_t kInk = 0xFFFF;
constexpr uint16_t kBox = 0x0000;

// Retangulo em coordenadas do destino (ja com o deslocamento ox/oy aplicado).
// Sem inicializador de membro de proposito: em C++11 um NSDMI tiraria o status
// de agregado e quebraria a inicializacao por chaves.
struct Rect {
  int x, y, w, h;
};

// Faixa maxima que `draw()` pode tocar. Serve a quem prefere repintar sempre a
// mesma regiao a rastrear o retangulo sujo de cada cue.
inline Rect band() {
  Rect r;
  r.x = crt::SAFE_L;
  r.y = layout::kTop;
  r.w = crt::SAFE_W;
  r.h = layout::kBlockH;
  return r;
}

// Troca a extensao de `videoPath` por ".srt" (o `.srt` mora ao lado do video).
// Sem extensao, anexa. Devolve false se nao couber em `cap`.
inline bool makeSrtPath(const char *videoPath, char *dst, size_t cap) {
  if (!videoPath || !dst || cap < 5)
    return false;
  const size_t n = strlen(videoPath);
  size_t cut = n;
  for (size_t i = n; i > 0; --i) {
    const char c = videoPath[i - 1];
    if (c == '/')
      break; // ponto depois da ultima barra, ou nao vale
    if (c == '.') {
      cut = i - 1;
      break;
    }
  }
  if (cut + 5 > cap)
    return false;
  memcpy(dst, videoPath, cut);
  memcpy(dst + cut, ".srt", 5);
  return true;
}

// ---------------------------------------------------------------------------
//  Leitor + renderizador
// ---------------------------------------------------------------------------
//
// `Stream` e qualquer coisa com `read(uint8_t *, size_t) -> tamanho lido`,
// exatamente o contrato que `playback::MjpegReader` usa: o `File` do Arduino
// serve, e no desktop uma classe de 10 linhas sobre um buffer ou um
// std::istream tambem. E o que torna o parser testavel sem cartao SD.
class Subtitles {
public:
  // Quantas cues no maximo cada `update()` consome. Limita o trabalho (e o
  // tempo com o sdMutex na mao) quando o relogio pula varias cues de uma vez.
  static const int kCuesPerUpdate = 4;
  // Teto de linhas varridas atras de um "-->" numa unica chamada. Um arquivo de
  // lixo nao pode prender o loop; como o fluxo so anda para a frente, cada
  // chamada progride e o EOF encerra.
  static const int kScanPerUpdate = 64;
  // Teto de linhas de texto de UMA cue. Depois disso a cue e fechada onde esta.
  static const int kTextLines = 16;

  Subtitles() { reset(); }

  // Esquece tudo. Use ao abrir outro .srt, ou depois de `arquivo.seek(0)` —
  // este leitor nao volta sozinho.
  void reset() {
    cursor_ = count_ = 0;
    first_ = true;
    eof_ = false;
    haveCur_ = haveNext_ = false;
    lastStart_ = 0;
    started_ = false;
    seq_ = 0;
    cur_.reset();
    next_.reset();
    dirty_.x = dirty_.y = dirty_.w = dirty_.h = 0;
  }

  // Avanca o parser ate a cue correspondente a `positionMs`.
  //
  // PRE-CONDICAO: o chamador segura o sdMutex durante esta chamada (ela le do
  // cartao). Nao chame com o mutex ja tomado por outro caminho, e nao segure o
  // mutex em volta de `draw()` — `draw()` nao le nada.
  template <class Stream> void update(Stream &file, uint32_t positionMs) {
    for (int guard = 0; guard < kCuesPerUpdate; ++guard) {
      if (!haveNext_) {
        if (eof_)
          break;
        if (!parseCue(file, next_))
          break; // EOF, ou orcamento de varredura estourado: tenta na proxima
        haveNext_ = true;
      }
      if (next_.startMs > positionMs)
        break; // a proxima ainda esta no futuro: nada a promover
      cur_ = next_;
      haveCur_ = true;
      haveNext_ = false;
      ++seq_;
    }
  }

  // Ha cue valida cobrindo `positionMs`?
  bool active(uint32_t positionMs) const {
    return haveCur_ && cur_.count && positionMs >= cur_.startMs && positionMs < cur_.endMs;
  }

  // 0 quando nada esta no ar; caso contrario um numero que muda a cada troca de
  // cue. Guarde o valor anterior: mudou, repinte; virou 0, apague.
  uint32_t revision(uint32_t positionMs) const { return active(positionMs) ? seq_ : 0; }

  // Linhas ja normalizadas (ASCII maiusculo) e quebradas. `i` de 0 a 1.
  uint8_t lineCount() const { return cur_.count; }
  const char *line(uint8_t i) const { return i < cur_.count ? cur_.text[i] : ""; }
  uint32_t cueStartMs() const { return cur_.startMs; }
  uint32_t cueEndMs() const { return cur_.endMs; }
  bool finished() const { return eof_ && !haveNext_; }

  // Pinta a legenda sobre a imagem. Nao le o cartao: usa a cue ja em memoria.
  // `ox`/`oy` seguem a convencao dos pintores do repositorio (dst, ox, oy, ...):
  // com deslocamento zero e o desenho normal.
  //
  // Sem cue no ar nao desenha nada E NAO MEXE no retangulo sujo — e assim que o
  // chamador ainda consegue saber o que precisa repintar depois que a cue sai.
  void draw(lgfx::LovyanGFX *dst, int ox, int oy, uint32_t positionMs) {
    if (!dst || !active(positionMs))
      return;
    const int n = cur_.count;
    const int top = layout::kBottom - n * layout::kLineH;
    int minX = crt::W, maxX = 0;
    dst->startWrite();
    for (int i = 0; i < n; ++i) {
      const int w = vcrfont::textWidth(cur_.text[i], layout::kScale);
      const int x = (crt::W - w) / 2; // centro do quadro = centro da area segura
      const int y = top + i * layout::kLineH;
      // Caixa opaca por linha, como no CC de verdade (cada fileira tinha o seu
      // fundo, e nao um bloco unico atras das duas).
      dst->fillRect(ox + x - layout::kPadX, oy + y, w + 2 * layout::kPadX, layout::kLineH, kBox);
      // Sem contorno: a caixa ja da o contraste, e contorno preto sobre preto
      // seria 2,15x os pixels por glifo de graca (ver VcrFont.h).
      vcrfont::drawText(dst, cur_.text[i], ox + x, oy + y, (int32_t)kInk, layout::kScale,
                        vcrfont::NO_OUTLINE);
      if (x - layout::kPadX < minX)
        minX = x - layout::kPadX;
      if (x + w + layout::kPadX > maxX)
        maxX = x + w + layout::kPadX;
    }
    dst->endWrite();
    dirty_.x = ox + minX;
    dirty_.y = oy + top;
    dirty_.w = maxX - minX;
    dirty_.h = n * layout::kLineH;
  }

  // Ultimo retangulo pintado, em coordenadas do destino. `w == 0` = nada sujo.
  // O caminho barato para apaga-lo e deixar o proximo quadro do filme cobrir a
  // area; com o video pausado, redecodifique o quadro (redrawCurrentFrame()).
  Rect dirty() const { return dirty_; }

  // Apaga o retangulo sujo e o zera. Atalho para quando nao ha imagem embaixo
  // (letterbox) — sobre o filme prefira repintar o quadro.
  void clear(lgfx::LovyanGFX *dst, int ox, int oy, uint16_t background = kBox) {
    if (dst && dirty_.w > 0)
      dst->fillRect(dirty_.x + ox, dirty_.y + oy, dirty_.w, dirty_.h, background);
    dirty_.x = dirty_.y = dirty_.w = dirty_.h = 0;
  }

private:
  // Capacidade do texto cru de uma cue depois de tirar as etiquetas e
  // normalizar: 2 linhas de 22 mais folga para a quebra de palavra.
  static const size_t kTextCap = 80;
  static const size_t kLineCap = 128; // linha do arquivo; o excedente e comido

  struct Cue {
    uint32_t startMs, endMs;
    uint8_t count;
    char text[layout::kMaxLines][layout::kMaxCols + 1];
    void reset() {
      startMs = endMs = 0;
      count = 0;
      text[0][0] = text[1][0] = 0;
    }
  };

  uint8_t block_[192];
  size_t cursor_, count_;
  bool first_, eof_;
  bool haveCur_, haveNext_, started_;
  uint32_t lastStart_, seq_;
  Cue cur_, next_;
  Rect dirty_;

  // -------------------------------------------------------------------------
  //  Fluxo de bytes
  // -------------------------------------------------------------------------

  // Proximo byte, ou -1 no fim. Le em blocos para nao pagar uma chamada de
  // sistema por caractere.
  template <class Stream> int nextByte(Stream &file) {
    if (cursor_ == count_) {
      if (eof_)
        return -1;
      const size_t got = file.read(block_, sizeof(block_));
      // `got` grande demais cobre tambem um read() com sinal devolvendo -1.
      if (got == 0 || got > sizeof(block_)) {
        eof_ = true;
        return -1;
      }
      count_ = got;
      cursor_ = 0;
      if (first_) {
        first_ = false;
        // BOM UTF-8: so pode estar nos 3 primeiros bytes do arquivo, entao
        // sempre cai neste primeiro bloco.
        if (count_ >= 3 && block_[0] == 0xEF && block_[1] == 0xBB && block_[2] == 0xBF)
          cursor_ = 3;
        if (cursor_ == count_)
          return nextByte(file);
      }
    }
    return block_[cursor_++];
  }

  // Le uma linha sem o terminador. CRLF, LF e a ultima linha sem quebra valem.
  // Linha maior que `cap` e truncada mas CONSUMIDA ate o fim — e o que garante
  // que um arquivo binario passado por engano nao trave o parser.
  template <class Stream> bool readLine(Stream &file, char *dst, size_t cap) {
    size_t n = 0;
    for (;;) {
      const int c = nextByte(file);
      if (c < 0) {
        dst[n] = 0;
        return n > 0;
      }
      if (c == '\n')
        break;
      if (c == '\r')
        continue;
      if (n + 1 < cap)
        dst[n++] = (char)c;
    }
    dst[n] = 0;
    return true;
  }

  // -------------------------------------------------------------------------
  //  Numeros e tempos
  // -------------------------------------------------------------------------

  static bool isDigit(char c) { return c >= '0' && c <= '9'; }

  // Le digitos a partir de `i`. Devolve quantos leu (0 = nao era numero).
  static int readUInt(const char *s, size_t &i, uint32_t &out) {
    int digits = 0;
    out = 0;
    while (isDigit(s[i])) {
      if (out < 100000000UL) // trava boba contra estouro em lixo do tipo "999999999999"
        out = out * 10 + uint32_t(s[i] - '0');
      ++i;
      ++digits;
    }
    return digits;
  }

  // `HH:MM:SS,mmm` a partir de `i`. Aceita ponto no lugar da virgula (varios
  // conversores geram assim), horas com 1 a 3 digitos e fracao de 1 a 3 casas.
  static bool parseTime(const char *s, size_t &i, uint32_t &ms) {
    while (s[i] == ' ' || s[i] == '\t')
      ++i;
    uint32_t h = 0, m = 0, sec = 0, frac = 0;
    if (!readUInt(s, i, h) || s[i] != ':')
      return false;
    ++i;
    if (!readUInt(s, i, m) || s[i] != ':')
      return false;
    ++i;
    if (!readUInt(s, i, sec))
      return false;
    if (s[i] == ',' || s[i] == '.') {
      ++i;
      const size_t before = i;
      if (!readUInt(s, i, frac))
        return false;
      size_t digits = i - before;
      // Normaliza para milissegundos: "5" = 500 ms, "05" = 50 ms, "005" = 5 ms.
      while (digits < 3) {
        frac *= 10;
        ++digits;
      }
      while (digits > 3) {
        frac /= 10;
        --digits;
      }
    }
    if (h > 999 || m > 59 || sec > 59 || frac > 999)
      return false;
    ms = h * 3600000UL + m * 60000UL + sec * 1000UL + frac;
    return true;
  }

  // Posicao do "-->" na linha, ou -1.
  static int findArrow(const char *s) {
    for (int i = 0; s[i]; ++i)
      if (s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>')
        return i;
    return -1;
  }

  // Inicio do carimbo de tempo que termina antes da seta. Voltar a partir da
  // seta em vez de comecar da coluna 0 e o que faz "1 00:00:01,000 --> ..."
  // funcionar: ha conversor que gruda o numero de sequencia na mesma linha.
  static size_t timeStart(const char *s, int arrow) {
    size_t i = (size_t)arrow;
    while (i && (s[i - 1] == ' ' || s[i - 1] == '\t'))
      --i;
    while (i && (isDigit(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == ',' || s[i - 1] == '.'))
      --i;
    return i;
  }

  // -------------------------------------------------------------------------
  //  Texto
  // -------------------------------------------------------------------------

  // Tira as etiquetas que o .srt carrega (<i>, </font>, {\an8}) e devolve o
  // texto cru. Roda ANTES da normalizacao: '<', '>', '{' e '}' sao ASCII e
  // nunca aparecem no meio de uma sequencia UTF-8, entao e seguro.
  static void stripTags(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    char closing = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
      const char c = src[i];
      if (closing) {
        if (c == closing)
          closing = 0;
        continue;
      }
      if (c == '<') {
        closing = '>';
        continue;
      }
      if (c == '{') {
        closing = '}';
        continue;
      }
      dst[j++] = c;
    }
    dst[j] = 0;
  }

  // Quebra `text` (ja em ASCII maiusculo, com '\n' separando as linhas do
  // arquivo) em no maximo kMaxLines linhas de kMaxCols colunas. Quebra na
  // ultima folga que couber; palavra maior que a linha e cortada na marra. O
  // que sobrar vira "..." no fim da ultima linha — o CC tambem cortava, mas
  // sem aviso, e aqui o aviso custa 3 caracteres e evita parecer bug.
  static uint8_t wrap(const char *text, char out[layout::kMaxLines][layout::kMaxCols + 1]) {
    uint8_t n = 0;
    size_t i = 0;
    while (n < layout::kMaxLines) {
      while (text[i] == ' ' || text[i] == '\n')
        ++i;
      if (!text[i])
        break;
      size_t take = 0, lastSpace = 0;
      bool haveSpace = false;
      size_t j = i;
      while (text[j] && text[j] != '\n' && take < (size_t)layout::kMaxCols) {
        if (text[j] == ' ') {
          lastSpace = take;
          haveSpace = true;
        }
        ++take;
        ++j;
      }
      // So vale quebrar na folga se a linha realmente continua depois dela.
      size_t adv = take;
      if (text[j] && text[j] != '\n' && haveSpace && lastSpace > 0)
        adv = lastSpace;
      size_t len = adv;
      memcpy(out[n], text + i, len);
      while (len && out[n][len - 1] == ' ')
        --len;
      out[n][len] = 0;
      i += adv;
      if (len)
        ++n;
      else if (adv == 0)
        break; // paranoia: sem avanco nao pode haver segunda volta
    }
    // Sobrou conteudo? Marca o corte na ultima linha.
    while (text[i] == ' ' || text[i] == '\n')
      ++i;
    if (text[i] && n > 0) {
      char *last = out[n - 1];
      size_t len = strlen(last);
      if (len + 3 <= (size_t)layout::kMaxCols) {
        last[len] = last[len + 1] = last[len + 2] = '.';
        last[len + 3] = 0;
      } else if (len >= 3) {
        last[len - 1] = last[len - 2] = last[len - 3] = '.';
      }
    }
    for (uint8_t k = n; k < layout::kMaxLines; ++k)
      out[k][0] = 0;
    return n;
  }

  // -------------------------------------------------------------------------
  //  Uma cue
  // -------------------------------------------------------------------------

  // Le a proxima cue VALIDA. Devolve false no EOF ou quando o orcamento de
  // linhas varridas acabou (nesse caso a proxima chamada continua de onde
  // parou — o fluxo so anda para a frente, entao nao ha laco infinito).
  //
  // Tolerancias, todas exercitadas pelo sim/probes/subtitles.cpp: numero de
  // sequencia ausente, errado ou substituido por lixo; linhas em branco de
  // sobra; CRLF; BOM. Cue com fim <= inicio, com tempo impossivel ou fora de
  // ordem e DESCARTADA — sem legenda e melhor que legenda errada.
  template <class Stream> bool parseCue(Stream &file, Cue &out) {
    char line[kLineCap];
    for (int scanned = 0; scanned < kScanPerUpdate; ++scanned) {
      if (!readLine(file, line, sizeof(line))) {
        eof_ = true;
        return false;
      }
      const int arrow = findArrow(line);
      if (arrow < 0)
        continue; // numero de sequencia, linha em branco ou lixo: ignora
      size_t p = timeStart(line, arrow);
      uint32_t startMs = 0, endMs = 0;
      if (!parseTime(line, p, startMs))
        continue;
      p = (size_t)arrow + 3;
      if (!parseTime(line, p, endMs))
        continue;

      // Texto: ate a linha em branco ou o fim do arquivo. Limitado a kTextLines
      // para que uma cue absurda nao leia o arquivo inteiro numa chamada so; o
      // excedente cai na varredura seguinte e e ignorado por nao ter seta.
      char raw[kTextCap], tmp[kLineCap];
      size_t used = 0;
      raw[0] = 0;
      bool more = true;
      for (int t = 0; more && t < kTextLines; ++t) {
        more = readLine(file, line, sizeof(line));
        if (!more) {
          eof_ = true;
          break;
        }
        // Linha em branco fecha a cue. Tambem fecha se ja apareceu outra seta,
        // caso o arquivo tenha esquecido a separacao (acontece).
        if (!line[0])
          break;
        if (used && findArrow(line) >= 0)
          break;
        stripTags(line, tmp, sizeof(tmp));
        if (used + 1 < sizeof(raw) && used)
          raw[used++] = '\n';
        // normalizeUpper termina em \0 e devolve o que escreveu: da para
        // emendar as linhas da cue sem buffer intermediario.
        used += ::ascii::normalizeUpper(raw + used, sizeof(raw) - used, tmp);
      }

      // Validacao. Ordem crescente exigida porque o leitor nao volta: uma cue
      // que comeca antes da anterior nunca poderia ser exibida mesmo.
      if (endMs <= startMs)
        continue;
      if (started_ && startMs < lastStart_)
        continue;
      out.startMs = startMs;
      out.endMs = endMs;
      out.count = wrap(raw, out.text);
      if (!out.count)
        continue; // cue sem texto util (so etiquetas, ou so acento sem ASCII)
      lastStart_ = startMs;
      started_ = true;
      return true;
    }
    return false;
  }
};

// O orcamento de SRAM em constante, para nao virar comentario desatualizado.
static_assert(sizeof(Subtitles) <= 1024, "Subtitles estourou 1 KB de SRAM");

} // namespace subs
