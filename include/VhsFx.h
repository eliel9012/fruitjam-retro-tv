#pragma once
// ============================================================================
//  VhsFx — artefatos de fita de videocassete sobre a reproducao de video.
//
//  O aparelho ja imita um VCR no OSD (VcrOsd.h); o que faltava era a IMAGEM
//  parecer uma fita gasta. Este header aplica os quatro artefatos que qualquer
//  um reconhece como VHS, na ordem em que valem a pena:
//
//    1. faixa de troca de cabeca (head switching) na base do quadro;
//    2. tremor horizontal de linhas isoladas (line jitter);
//    3. rajadas de erro de tracking, que rolam pela tela;
//    4. sangramento horizontal de croma (chroma bleed).
//
//  ---------------------------------------------------------------------------
//  A restricao que desenhou este arquivo: nao sobra tempo
//  ---------------------------------------------------------------------------
//  Medido no aparelho a 320x240 RGB565: decode JPEG 25,3 ms, blit CVBS 12,3 ms,
//  23,5 quadros/s sustentados. Um passe por pixel sobre os 76.800 pixels do
//  quadro custaria milissegundos e comeria a taxa inteira. Entao aqui:
//
//    * quase tudo e POR LINHA, nao por pixel. Deslocar uma linha e mudar o x de
//      um pushImage, custo zero em pixels escritos;
//    * o que e por pixel (so o croma) anda JUNTO com o blit que ja acontece,
//      dentro do buffer de MCU que o JPEGDEC acabou de preencher — nao existe
//      segundo passe sobre o quadro;
//    * o ruido toca uma fracao minuscula do quadro: a faixa de troca de cabeca
//      tem 5 a 9 linhas, a de tracking 8 a 16. Somadas, menos de 10% das 240.
//
//  Orcamento por efeito. Os contadores sao os que sim/probes/vhs_fx.cpp mede de
//  verdade (pior caso em 120 quadros, rajada de tracking inclusa); as constantes
//  vem do aparelho: 12,3 ms / 76.800 px = 0,160 us por pixel escrito, ~6 us de
//  setup por pushImage, ~14 ciclos por pixel de croma a 240 MHz, ~7 us para
//  gerar uma linha de ruido. Numeros para o pior formato, 320x240:
//
//    efeito                 pior caso     de onde vem o numero
//    ------------------------------------------------------------------------
//    line jitter + rasgo    0,27-0,83 ms  ate 18 linhas deslocadas partem as
//                                         tiras de MCU (128x16) em mais ~140
//                                         pushImage por quadro. Os PIXELS sao
//                                         exatamente os mesmos: so a chamada e
//                                         nova. 140 x 6 us = 0,83 ms.
//    troca de cabeca        ~0,35 ms      <=5 linhas de ruido: 5 x 320 px x
//                                         0,160 us = 0,26 ms de escrita, mais
//                                         5 x 13 us de geracao e setup. As
//                                         linhas de RASGO custam zero (so
//                                         mudam o x de um pushImage que ja ia
//                                         acontecer).
//    tracking               ~0,93 ms      <=15 linhas de ruido, mesma conta.
//    chroma bleed (n1)      1,12 ms       1 de cada 4 linhas: 19.200 px.
//    chroma bleed (n2)      2,24 ms       1 de cada 2 linhas: 38.400 px.
//    chroma bleed (n3)      4,48 ms       todas: 76.800 px. So o RUIM usa.
//
//  Por nivel de desgaste, somando (medido pelo probe, 320x240):
//
//    NOVA   1,58 ms  -> 23,5 fps caem para 22,7.
//    GASTA  3,00 ms  -> 22,0 fps.
//    RUIM   6,59 ms  -> 20,3 fps. E uma escolha explicita do usuario, e nao
//                      dessincroniza nada: o relogio continua sendo o PCM
//                      entregue, entao o videoTick() simplesmente descarta
//                      mais quadros.
//
//  O croma sozinho responde por 70% do custo em qualquer nivel. Quem precisar
//  de folga desliga so ele (cfg.chroma = false) e fica com os tres artefatos
//  de linha por menos de 1,3 ms. O padrao recomendado e GASTA.
//
//  ---------------------------------------------------------------------------
//  O que ficou de fora, por ser caro demais
//  ---------------------------------------------------------------------------
//    * dropout branco horizontal com "arraste" de varias linhas: exigiria
//      guardar a linha anterior inteira e reescreve-la — segundo passe sobre a
//      imagem, sem o ganho visual dos quatro acima;
//    * ruido de luminancia granulado no quadro TODO (o "chiado" da fita):
//      76.800 pixels de leitura-modificacao-escrita, ~4,5 ms, pelo mesmo preco
//      do chroma bleed e com muito menos leitura de "VHS";
//    * separacao Y/C de verdade (converter para YUV, filtrar C, voltar):
//      3 multiplicacoes por pixel, mais de 10 ms. O truque empacotado usado em
//      smearRow() entrega a mesma leitura por 1/8 do preco;
//    * flagging / skew do topo do quadro (a curva em S das primeiras linhas):
//      cabe no mecanismo de deslocamento e custa quase nada, mas so aparece em
//      TV sem TBC e ficou de fora para nao inflar a API. E a extensao obvia.
//
//  ---------------------------------------------------------------------------
//  Memoria
//  ---------------------------------------------------------------------------
//  Filter tem 972 bytes (medido): int8_t shift_[240] (240 B) e uint16_t line_[320]
//  (640 B). Nada de LGFX_Sprite, nada de quadro de rascunho — um sprite de 5 KB
//  ja causou falta de memoria neste repositorio, e o minimo de heap livre
//  medido aqui e 28-38 KB. A escolha foi SRAM e nao PSRAM justamente por ser
//  pequeno: line_ e lido e escrito uma vez por linha de ruido, e a PSRAM
//  (~4x mais lenta) so atrapalharia. DECLARE COMO GLOBAL, nunca na pilha de uma
//  tarefa que faz HTTPS (ver AGENTS.md 2.3).
//
//  ---------------------------------------------------------------------------
//  Cor
//  ---------------------------------------------------------------------------
//  O buffer de MCU do JPEGDEC e SEMPRE RGB565 (o firmware pede
//  jpeg.setPixelType(RGB565_LITTLE_ENDIAN)), independente da profundidade do
//  painel — e por isso smearRow() pode mexer nos bits na mao. Ja o painel pode
//  estar em RGB565 ou RGB332 (settings.color16): nada aqui depende disso,
//  porque tudo sai por pushImage de lgfx::rgb565_t, que o LovyanGFX converte.
//
//  ARMADILHA DE TIPO: o LovyanGFX escolhe o formato da cor pelo TIPO DO
//  ARGUMENTO. uint16_t/int16_t/int32_t viram RGB565; uint32_t vira RGB888
//  (misc/colortype.hpp:861-866). Um (uint32_t)0xBDF7 vira R=0,G=189,B=247 em
//  silencio. Toda cor aqui e uint16_t, de proposito.
//
//  O ruido e SO luminancia (cinza), o que alem de ser o que a fita fazia de
//  verdade evita croma saturado na saida composta — ciano puro (0x07FF) produz
//  dot crawl no NTSC e nao aparece em lugar nenhum deste arquivo.
//
//  ---------------------------------------------------------------------------
//  Uso (integracao em src/main.cpp)
//  ---------------------------------------------------------------------------
//    vhs::Filter vhsFilter;                       // global, 972 B
//
//    // ao comecar um programa:
//    vhsFilter.configure(vhs::presetFor(settings.vhsWear));
//    vhsFilter.begin(0xC0FFEE);
//
//    // em readAndShowOneFrame(), ANTES do jpeg.decode():
//    vhsFilter.beginFrame(millis(), (CRT_W - videoWidth) / 2,
//                         (CRT_H - videoHeight) / 2, videoWidth, videoHeight);
//
//    // em jpegDraw(), no lugar do rca.pushImage():
//    vhsFilter.pushBlock(&rca, draw->x + ox, draw->y + oy, draw->iWidth,
//                        draw->iHeight, draw->pPixels);
//
//    // logo depois do jpeg.decode() bem-sucedido:
//    vhsFilter.drawOverlay(&rca);
//
//  beginFrame() e o unico ponto que avanca o estado. redrawCurrentFrame() (o
//  redecode do quadro pausado) NAO deve chama-lo: reaproveitando a mesma tabela
//  de deslocamento, a imagem pausada sai identica quadro apos quadro, sem
//  tremer embaixo do OSD. Basta repetir o drawOverlay() depois.
//
//  Nada de Arduino, SD, FreeRTOS ou millis() interno: o relogio entra por
//  parametro e o PRNG e semeado pelo chamador, entao o probe do simulador
//  reproduz uma execucao inteira bit a bit.
// ============================================================================

#include "fj/Gfx.h"
#include <stdint.h>
#include <string.h>

#include "SafeArea.h"
#include "UiLogic.h" // timeReached(): millis() da a volta em ~49 dias

namespace vhs {

// ---------------------------------------------------------------------------
//  Estado de conservacao da fita — o que o usuario escolhe de fato
// ---------------------------------------------------------------------------

enum class Wear : uint8_t { Off = 0, Nova = 1, Gasta = 2, Ruim = 3 };

inline const char *wearLabel(Wear w) {
  return w == Wear::Nova ? "NOVA" : w == Wear::Gasta ? "GASTA" : w == Wear::Ruim ? "RUIM" : "DESLIGADO";
}

// Ciclo para a tela de configuracoes: DESLIGADO -> NOVA -> GASTA -> RUIM -> ...
inline Wear nextWear(Wear w) { return Wear(((uint8_t)w + 1) & 3); }
inline Wear prevWear(Wear w) { return Wear(((uint8_t)w + 3) & 3); }

// ---------------------------------------------------------------------------
//  Configuracao: cada efeito liga e desliga sozinho, com intensidade 1..3
// ---------------------------------------------------------------------------

struct Config {
  bool enabled = false;

  bool headSwitch = true;   // faixa de troca de cabeca na base do quadro
  uint8_t headLevel = 2;    // 1..3

  bool jitter = true;       // tremor horizontal de linhas isoladas
  uint8_t jitterLevel = 2;  // 1..3

  bool tracking = true;     // rajadas de erro de tracking que rolam
  uint8_t trackingLevel = 2;// 1..3
  uint16_t trackingGapMs = 6000; // intervalo medio entre rajadas

  bool chroma = true;       // sangramento horizontal de croma
  // 1 = 1 linha a cada 4 (~1,1 ms), 2 = 1 a cada 2 (~2,3 ms),
  // 3 = todas (~4,5 ms). E o unico efeito caro do arquivo.
  uint8_t chromaLevel = 1;
};

// Combinacoes prontas. E isto que a tela de configuracoes oferece; os campos
// individuais existem para o probe e para quem quiser afinar no codigo.
inline Config presetFor(Wear w) {
  Config c;
  switch (w) {
  case Wear::Nova: // fita nova: so a troca de cabeca e um tremor raro
    c.enabled = true;
    c.headLevel = 1;
    c.jitterLevel = 1;
    c.tracking = false;
    c.chromaLevel = 1;
    break;
  case Wear::Gasta: // a fita que todo mundo tinha: tracking de vez em quando
    c.enabled = true;
    c.headLevel = 2;
    c.jitterLevel = 2;
    c.trackingLevel = 2;
    c.trackingGapMs = 8000;
    c.chromaLevel = 2;
    break;
  case Wear::Ruim: // fita de locadora no fim da vida
    c.enabled = true;
    c.headLevel = 3;
    c.jitterLevel = 3;
    c.trackingLevel = 3;
    c.trackingGapMs = 2500;
    c.chromaLevel = 3;
    break;
  default:
    c.enabled = false;
    break;
  }
  return c;
}

// ---------------------------------------------------------------------------
//  Cores e limites
// ---------------------------------------------------------------------------

// Cinza puro em RGB565. Corpo de UM return so: constexpr com laco ou variavel
// local compila no simulador (C++17) e QUEBRA no firmware (C++11) — ja
// aconteceu com a VcrFont (AGENTS.md 7.1).
constexpr uint16_t gray565(int v) {
  return (uint16_t)((((v < 0 ? 0 : (v > 255 ? 255 : v)) >> 3) << 11) |
                    (((v < 0 ? 0 : (v > 255 ? 255 : v)) >> 2) << 5) |
                    ((v < 0 ? 0 : (v > 255 ? 255 : v)) >> 3));
}

namespace limits {
constexpr int kMaxShift = 8;      // deslocamento maximo de uma linha, em px
constexpr int kMaxHeadRows = 9;   // altura maxima da faixa de troca de cabeca
constexpr int kMaxTrackRows = 16; // altura maxima da faixa de tracking
constexpr int kMaxJitterRows = 24;// linhas tremidas por quadro, teto
} // namespace limits

// ---------------------------------------------------------------------------
//  PRNG: xorshift32. rand() esta fora de questao (lento, global e nao
//  reproduzivel entre execucoes). Semeado pelo chamador, entao o probe repete
//  uma execucao inteira bit a bit.
// ---------------------------------------------------------------------------

class Rng {
public:
  void seed(uint32_t s) { state_ = s ? s : 0x1234567Bu; }
  uint32_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
  }
  // Inteiro em [0, n). Multiplicacao alta em vez de modulo: o LX6 do ESP32
  // resolve divisao em software e isto roda por linha de ruido.
  uint32_t below(uint32_t n) { return n ? (uint32_t)(((uint64_t)next() * n) >> 32) : 0; }
  uint32_t state() const { return state_; }

private:
  uint32_t state_ = 0x1234567Bu;
};

// ---------------------------------------------------------------------------
//  Filtro
// ---------------------------------------------------------------------------

class Filter {
public:
  // ---- ciclo de vida -------------------------------------------------------

  void configure(const Config &cfg) { cfg_ = cfg; }
  const Config &config() const { return cfg_; }

  // Zera o estado e semeia o PRNG. Chamar ao entrar na reproducao.
  void begin(uint32_t seed) {
    rng_.seed(seed);
    memset(shift_, 0, sizeof(shift_));
    headTop_ = headNoiseTop_ = headRows_ = 0;
    trkTop_ = trkRows_ = 0;
    trkActive_ = false;
    trkY_ = 0;
    shiftsActive_ = false;
    trkEndMs_ = 0;
    trkNextMs_ = 0;
    topBias_ = 0;
    armed_ = false;
    resetStats();
  }

  // Avanca o estado um quadro e reconstroi a tabela de deslocamento por linha.
  // (px, py, pw, ph) e o retangulo do video dentro do quadro de 320x240 — o
  // jpegDraw() ja centraliza ali, e os artefatos pertencem a IMAGEM, nao ao
  // letterbox preto em volta.
  void beginFrame(uint32_t nowMs, int px, int py, int pw, int ph) {
    picX_ = px;
    picY_ = py;
    picW_ = pw;
    picH_ = ph;
    memset(shift_, 0, sizeof(shift_));
    shiftsActive_ = false; // senao um quadro herdaria o corte em faixas do anterior
    headRows_ = trkRows_ = 0;
    if (!cfg_.enabled || pw <= 0 || ph <= 0)
      return;
    if (!armed_) { // primeira rajada nunca cai no quadro 1
      trkNextMs_ = nowMs + cfg_.trackingGapMs;
      armed_ = true;
    }
    buildTracking(nowMs);
    buildHeadSwitch();
    buildJitter();
  }

  // ---- caminho do blit (chamado de dentro do jpegDraw) ---------------------

  // Substitui o rca.pushImage() do jpegDraw. Faz o croma no proprio buffer de
  // MCU (nenhum segundo passe pelo quadro) e empurra a tira em faixas de
  // deslocamento constante — linha nao deslocada nao custa chamada extra.
  // `px` e o buffer RGB565 do JPEGDEC, com passo `w`; e escrito no lugar, o que
  // e seguro porque o JPEGDEC o repreenche a cada tira.
  void pushBlock(lgfx::LovyanGFX *gfx, int x, int y, int w, int h, uint16_t *px) {
    if (!gfx || !px || w <= 0 || h <= 0)
      return;
    if (!cfg_.enabled) {
      gfx->pushImage(x, y, w, h, reinterpret_cast<const lgfx::rgb565_t *>(px));
      ++pushes_;
      return;
    }
    if (cfg_.chroma && cfg_.chromaLevel)
      smearBlock(px, w, h, y);
    if (!shiftsActive_) {
      gfx->pushImage(x, y, w, h, reinterpret_cast<const lgfx::rgb565_t *>(px));
      ++pushes_;
      return;
    }
    int r = 0;
    while (r < h) {
      const int s = shiftAt(y + r);
      int r2 = r + 1;
      while (r2 < h && shiftAt(y + r2) == s)
        ++r2;
      // O recorte fica com o LovyanGFX: pushImage corta contra _clip_l/_clip_r
      // e corrige o src_x, entao um deslocamento que sai do raster e aparado,
      // nunca escrito fora. (LGFXBase.cpp:1425-1440.)
      gfx->pushImage(x + s, y + r, w, r2 - r, reinterpret_cast<const lgfx::rgb565_t *>(px + (size_t)r * w));
      ++pushes_;
      r = r2;
    }
  }

  // ---- faixas de ruido (chamado depois do decode) --------------------------

  // Pinta as faixas de ruido de luminancia: troca de cabeca na base e, quando
  // ativa, a rajada de tracking. Sao as unicas linhas do quadro que este
  // arquivo escreve do zero.
  void drawOverlay(lgfx::LovyanGFX *gfx) {
    if (!gfx || !cfg_.enabled || picW_ <= 0)
      return;
    const int w = picW_ > crt::W ? crt::W : picW_;
    if (headRows_ > 0) {
      // A base da faixa e o ponto mais claro: e ali que o sinal da cabeca que
      // sai some de vez. Rampa de 96 ate 208 em luminancia.
      for (int i = 0; i < headRows_; ++i) {
        const int y = headNoiseTop_ + i;
        const int base = 96 + (headRows_ > 1 ? (112 * i) / (headRows_ - 1) : 112);
        paintNoiseRow(gfx, y, w, (uint8_t)base, 110);
      }
    }
    if (trkRows_ > 0) {
      for (int i = 0; i < trkRows_; ++i)
        paintNoiseRow(gfx, trkTop_ + i, w, 128, 140);
    }
  }

  // ---- consultas (usadas pelo probe e pelo diagnostico) --------------------

  int lineShift(int y) const { return shiftAt(y); }
  int headNoiseTop() const { return headRows_ ? headNoiseTop_ : -1; }
  int headNoiseRows() const { return headRows_; }
  int trackingTop() const { return trkRows_ ? trkTop_ : -1; }
  int trackingRows() const { return trkRows_; }
  bool trackingActive() const { return trkActive_; }
  uint32_t rngState() const { return rng_.state(); }

  // Contadores de custo, no espirito do ScreenFx::touched().
  void resetStats() { pushes_ = noiseRows_ = smearedRows_ = smearedPixels_ = 0; }
  uint32_t pushes() const { return pushes_; }
  uint32_t noiseRows() const { return noiseRows_; }
  uint32_t smearedRows() const { return smearedRows_; }
  uint32_t smearedPixels() const { return smearedPixels_; }

private:
  // ---- tabela de deslocamento ---------------------------------------------

  int shiftAt(int y) const {
    return (y < 0 || y >= crt::H) ? 0 : (int)shift_[y];
  }

  void setShift(int y, int v) {
    // Fora do retangulo do video so existe letterbox preto: deslocar uma linha
    // dali nao produz artefato nenhum e ainda arrastaria sujeira para dentro da
    // imagem. O recorte e aqui, uma vez, e nao em cada chamador.
    if (y < 0 || y >= crt::H || y < picY_ || y >= picY_ + picH_)
      return;
    if (v > limits::kMaxShift)
      v = limits::kMaxShift;
    else if (v < -limits::kMaxShift)
      v = -limits::kMaxShift;
    shift_[y] = (int8_t)v;
    if (v)
      shiftsActive_ = true;
  }

  // Rajada de tracking: uma faixa de ruido que ROLA pela imagem, como quando o
  // botao de tracking esta fora de ponto. Sobe (o sentido classico do VHS) a
  // poucas linhas por quadro e leva junto um tremor pesado nas bordas.
  void buildTracking(uint32_t nowMs) {
    if (!cfg_.tracking || !cfg_.trackingLevel) {
      trkActive_ = false;
      return;
    }
    if (!trkActive_ && timeReached(nowMs, trkNextMs_)) {
      trkActive_ = true;
      trkY_ = picY_ + picH_ - 1;                  // entra pela base
      trkEndMs_ = nowMs + 700 + rng_.below(900);  // 0,7 a 1,6 s de rajada
      topBias_ = 255;                             // e o que joga o tremor pro topo
    }
    if (!trkActive_)
      return;
    if (timeReached(nowMs, trkEndMs_)) {
      trkActive_ = false;
      // Intervalo com jitter de +-50% em volta do valor configurado, para a
      // rajada nao virar metronomo.
      const uint32_t gap = cfg_.trackingGapMs ? cfg_.trackingGapMs : 6000;
      trkNextMs_ = nowMs + gap / 2 + rng_.below(gap);
      return;
    }
    const int lvl = clampLevel(cfg_.trackingLevel);
    trkRows_ = 6 + 3 * lvl;                        // 9, 12 ou 15 linhas
    if (trkRows_ > limits::kMaxTrackRows)
      trkRows_ = limits::kMaxTrackRows;
    trkY_ -= 2 + lvl;                              // sobe 3 a 5 linhas por quadro
    if (trkY_ < picY_ - trkRows_)
      trkY_ = picY_ + picH_ - 1;                   // volta pela base
    trkTop_ = trkY_;
    clampBand(trkTop_, trkRows_);
    // Bordas da faixa: a imagem em volta da rajada e arrastada de lado.
    for (int i = 1; i <= 3; ++i) {
      const int amp = (limits::kMaxShift * (4 - i) * lvl) / 9;
      setShift(trkTop_ - i, (int)rng_.below((uint32_t)(2 * amp + 1)) - amp);
      setShift(trkTop_ + trkRows_ - 1 + i, (int)rng_.below((uint32_t)(2 * amp + 1)) - amp);
    }
  }

  // Faixa de troca de cabeca: as ultimas linhas do quadro. Em cima, o rasgo —
  // a imagem escorrega de lado numa rampa; embaixo, ruido puro. E o artefato
  // mais reconhecivel do VHS e custa quase nada: o rasgo so muda o x de um
  // pushImage que ja ia acontecer.
  void buildHeadSwitch() {
    if (!cfg_.headSwitch || !cfg_.headLevel)
      return;
    const int lvl = clampLevel(cfg_.headLevel);
    int noise = 2 + lvl;  // 3, 4 ou 5 linhas de ruido
    int tear = 1 + lvl;   // 2, 3 ou 4 linhas de rasgo acima delas
    if (noise + tear > limits::kMaxHeadRows)
      noise = limits::kMaxHeadRows - tear;
    headNoiseTop_ = picY_ + picH_ - noise;
    headTop_ = headNoiseTop_ - tear;
    headRows_ = noise;
    clampBand(headNoiseTop_, headRows_);
    // Rampa do rasgo: zero na linha de cima, maximo colado no ruido. O sinal
    // negativo (para a esquerda) e o sentido em que a maioria dos decks torcia.
    const int amp = 2 + 2 * lvl;
    for (int i = 0; i < tear; ++i) {
      const int y = headTop_ + i;
      const int mag = (amp * (i + 1)) / tear;
      setShift(y, -(mag + (int)rng_.below(2)));
    }
  }

  // Tremor de linhas isoladas. Poucas linhas por quadro, sorteadas; depois de
  // um erro de tracking a densidade se concentra no topo do quadro, que e onde
  // o servo do VCR levava mais tempo para reencontrar a trilha.
  void buildJitter() {
    if (!cfg_.jitter || !cfg_.jitterLevel)
      return;
    const int lvl = clampLevel(cfg_.jitterLevel);
    int n = 6 * lvl; // 6, 12 ou 18 linhas
    if (n > limits::kMaxJitterRows)
      n = limits::kMaxJitterRows;
    const int amp = lvl; // +-1, +-2 ou +-3 px
    const int top = picY_, h = picH_;
    if (h <= 0)
      return;
    const int quarter = h / 4 > 0 ? h / 4 : 1;
    for (int i = 0; i < n; ++i) {
      const uint32_t r = rng_.next();
      int y = top + (int)(((uint64_t)(r >> 8) * (uint32_t)h) >> 24);
      // Vies para o topo enquanto o distúrbio do tracking nao decai.
      if ((r & 0xFF) < topBias_)
        y = top + (int)rng_.below((uint32_t)quarter);
      int d = (int)rng_.below((uint32_t)(2 * amp + 1)) - amp;
      if (!d)
        d = (r & 0x100) ? 1 : -1; // linha sorteada nunca sai de graca
      setShift(y, shiftAt(y) + d);
    }
    if (topBias_)
      topBias_ = (uint8_t)((topBias_ * 7) / 8); // decai em ~15 quadros
  }

  // ---- ruido ---------------------------------------------------------------

  // Uma linha de ruido de LUMINANCIA, em corridas horizontais de 1 a 4 px. A
  // corrida nao e so estetica (o ruido da fita era mesmo riscado na
  // horizontal): ela divide por ~2,5 o numero de chamadas ao PRNG.
  void paintNoiseRow(lgfx::LovyanGFX *gfx, int y, int w, uint8_t base, int spread) {
    if (y < 0 || y >= crt::H || w <= 0)
      return;
    int x = 0;
    while (x < w) {
      const uint32_t r = rng_.next();
      int run = 1 + (int)((r >> 3) & 3);
      if (x + run > w)
        run = w - x;
      const int delta = ((int)((r >> 8) & 0xFF) - 128) * spread;
      const uint16_t c = gray565((int)base + (delta >> 7));
      for (int i = 0; i < run; ++i)
        line_[x + i] = c;
      x += run;
    }
    gfx->pushImage(picX_, y, w, 1, reinterpret_cast<const lgfx::rgb565_t *>(line_));
    ++pushes_;
    ++noiseRows_;
  }

  // ---- croma ---------------------------------------------------------------

  // Sangramento horizontal de croma dentro do proprio buffer de MCU.
  //
  // O VHS gravava croma em ~0,4 MHz contra ~3 MHz de luminancia, entao a cor
  // escorre para a direita e a luz nao. Um filtro IIR de um polo por linha
  // reproduz isso: acc = (acc + pixel) / 2, arrastado da esquerda para a
  // direita, com a cauda exponencial caindo em ~3 px.
  //
  // O truque: media de dois RGB565 SEM desempacotar, e so nos campos R e B.
  //   media(a,b) por campo = (a & b) + (((a ^ b) >> 1) & mascara_deslocada)
  // Mantendo G intacto (0x07E0), o verde — que carrega quase toda a
  // luminancia percebida — nao borra, e o resultado le como cor escorrendo,
  // nao como imagem fora de foco. Custa ~11 instrucoes por pixel, contra as 3
  // multiplicacoes que uma conversao YUV de verdade exigiria.
  //
  // Limite conhecido e aceito: o filtro reinicia a cada tira de MCU (o JPEGDEC
  // entrega 128 px de largura), entao o primeiro pixel de cada tira nao herda a
  // cauda do vizinho da esquerda. E 1 pixel a cada 128, invisivel num tubo.
  static void smearRow(uint16_t *row, int w) {
    uint16_t acc = row[0];
    for (int x = 1; x < w; ++x) {
      const uint16_t c = row[x];
      const uint16_t m =
          (uint16_t)((c & 0x07E0) | (uint16_t)(((acc & c) & 0xF81F) + (uint16_t)(((acc ^ c) >> 1) & 0x780F)));
      row[x] = m;
      acc = m; // IIR: a cauda acompanha, um passe so
    }
  }

  void smearBlock(uint16_t *px, int w, int h, int y0) {
    // Nivel 1 = 1 linha a cada 4, 2 = 1 a cada 2, 3 = todas. O passo vertical e
    // a unica maneira honesta de comprar desempenho aqui: metade das linhas,
    // metade do custo. Num tubo a 240p, a linha nao filtrada some no borrao do
    // proprio sinal composto.
    const int lvl = clampLevel(cfg_.chromaLevel);
    const int step = lvl >= 3 ? 1 : (lvl == 2 ? 2 : 4);
    for (int r = 0; r < h; ++r) {
      if (((y0 + r) % step) != 0)
        continue;
      smearRow(px + (size_t)r * w, w);
      ++smearedRows_;
      smearedPixels_ += (uint32_t)w;
    }
  }

  // ---- utilidades ----------------------------------------------------------

  static int clampLevel(uint8_t v) { return v < 1 ? 1 : (v > 3 ? 3 : (int)v); }

  // Prende uma faixa (topo, altura) dentro da IMAGEM, e a imagem dentro do
  // raster. Depois disto nenhuma linha de ruido pode cair no letterbox preto
  // nem fora de [0, crt::H) — e o que garante que a rajada de tracking suma
  // pela borda de cima em vez de pintar chiado sobre a tarja.
  void clampBand(int &top, int &rows) const {
    int lo = picY_ < 0 ? 0 : picY_;
    int hi = picY_ + picH_;
    if (hi > crt::H)
      hi = crt::H;
    if (top < lo) {
      rows -= (lo - top);
      top = lo;
    }
    if (top + rows > hi)
      rows = hi - top;
    if (rows < 0)
      rows = 0;
  }

  Config cfg_;
  Rng rng_;

  int8_t shift_[crt::H];  // deslocamento por linha do framebuffer, em px
  uint16_t line_[crt::W]; // uma linha de ruido, reusada

  bool shiftsActive_ = false;
  int picX_ = 0, picY_ = 0, picW_ = 0, picH_ = 0;

  int headTop_ = 0, headNoiseTop_ = 0, headRows_ = 0;
  int trkTop_ = 0, trkRows_ = 0, trkY_ = 0;
  bool trkActive_ = false;
  uint32_t trkEndMs_ = 0, trkNextMs_ = 0;
  uint8_t topBias_ = 0;
  bool armed_ = false;

  uint32_t pushes_ = 0, noiseRows_ = 0, smearedRows_ = 0, smearedPixels_ = 0;
};

} // namespace vhs
