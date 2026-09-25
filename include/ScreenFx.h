#pragma once
// ============================================================================
//  ScreenFx — transições entre telas da saída composta (RCA) do M5 RETRO TV.
//
//  Vocabulário do Weather Star 4000 (o gerador de caracteres que o The Weather
//  Channel usava nos anos 80): esmaecimento para preto, esmaecimento a partir
//  do preto, dissolução entre duas páginas, deslizamento lateral entre
//  "condições atuais" e "previsão estendida", e cortina vertical.
//
//  ---------------------------------------------------------------------------
//  Por que dithering ordenado e não mistura alfa
//  ---------------------------------------------------------------------------
//  O quadro CVBS é RGB332 (1 byte/pixel) e já ocupa 76.800 bytes da SRAM
//  interna. Não sobra espaço para dois sprites de tela cheia, então não há como
//  interpolar origem e destino pixel a pixel. A saída é decidir POR PIXEL quem
//  aparece, usando uma matriz de Bayer 8x8: a cada nível 0..63 um sexagésimo
//  quarto dos pixels troca de lado. O olho (e principalmente um tubo, que já
//  borra o sinal) integra isso como um esmaecimento contínuo, e o resultado
//  granulado é exatamente a textura da época.
//
//  Propriedade que interessa ao orçamento: um esmaecimento completo escreve
//  cada pixel da tela UMA única vez (76.800 escritas no total), distribuídas ao
//  longo da duração. Cada nível custa W*H/64 = 1.200 escritas.
//
//  ---------------------------------------------------------------------------
//  O que precisa de buffer e o que não precisa
//  ---------------------------------------------------------------------------
//  fadeOut, slide e wipe não alocam nada: a origem já está no framebuffer e o
//  destino é redesenhado por região (recorte via setClipRect).
//
//  fadeIn e crossfade são a exceção: revelar o destino pixel a pixel exige uma
//  cópia dele em algum lugar legível. Aqui isso é um LGFX_Sprite 320x240 a 8
//  bits (76.800 bytes) que O CHAMADOR fornece — ScreenFx nunca aloca. No Core2
//  esse sprite só cabe na PSRAM (setPsram(true)), e o custo tem duas partes:
//
//    * pintar o destino DENTRO do sprite, uma vez, no início da transição.
//      A PSRAM do Core2 é ~4x mais lenta que a SRAM em escrita sequencial, e
//      esta é a parte cara: estimar 3 a 5x o tempo de um drawWeatherFrame()
//      normal (dezenas de ms). É um soluço único, no começo — por isso ele
//      acontece ANTES do primeiro passo visível, e não no meio.
//    * ler 1.200 bytes por nível. Espalhados de 8 em 8, tocam ~2.400 linhas de
//      cache por nível; na ordem de centenas de microssegundos. Irrelevante.
//
//  Se o sprite não vier (nullptr, ou sem buffer), fadeIn/crossfade degradam
//  para corte seco / esmaecimento para preto + corte seco, e o método devolve
//  false para o chamador saber. Nunca travam nem alocam por conta própria.
//
//  ---------------------------------------------------------------------------
//  Uso
//  ---------------------------------------------------------------------------
//  A transição é uma máquina de estados dirigida pelo relógio do chamador —
//  nada de delay(), porque o loop() precisa continuar alimentando o áudio e a
//  rede. O tempo entra como parâmetro (nem millis() é chamado aqui dentro),
//  o que também torna o arquivo testável no simulador de desktop.
//
//    crt::fx::Transition tx(&rca);
//    tx.slide(millis(), crt::fx::screen(pintaAtual), crt::fx::screen(pintaExt),
//             crt::fx::DIR_LEFT);
//    ...
//    void loop() {
//      if (tx.busy()) tx.tick(millis());  // devolve true no quadro final
//      else           weatherTick();
//    }
//
//  Contrato do pintor:
//    * soma (ox, oy) a TODAS as coordenadas. Só o slide passa valores
//      diferentes de zero; um pintor que ignore o deslocamento continua
//      correto para fade, crossfade e wipe (no slide degrada para uma cortina
//      horizontal, sem quebrar).
//    * NÃO mexe no recorte (setClipRect/clearClipRect): é por ele que slide e
//      wipe limitam o repinte. Para o fundo use fillScreen(), que já respeita
//      o recorte corrente e por isso preenche exatamente a região da vez.
//    * pode ser chamado mais de uma vez no mesmo quadro (slide) e com um
//      LGFX_Sprite como destino (crossfade), então precisa ser reentrante e
//      não pode depender de estado global de desenho.
//
//  ---------------------------------------------------------------------------
//  Custo medido (sim/probes/screen_fx.cpp, quadro de 320x240 = 76.800 px)
//  ---------------------------------------------------------------------------
//    efeito       duração  passos   pixels escritos   média por passo
//    fadeOut       480 ms      30      76.800 (1 tela)      2.560
//    fadeIn        480 ms      30     153.600 (2 telas)     5.120  (*)
//    crossfade     640 ms      40      76.800 (1 tela)      1.920
//    fadeThrough   760 ms      48     153.600 (2 telas)     3.200
//    slide         520 ms      33   2.534.400 (33 telas)   76.800  (**)
//    wipe          480 ms      30      76.800 (1 tela)      2.560
//
//    (*)  inclui o fillScreen inicial que garante o ponto de partida.
//    (**) o slide é o único caro: repinta a tela inteira a cada passo de 8 px,
//         porque as duas páginas se movem. Mesmo assim são ~77 mil pixels por
//         passo em memória interna (preenchimentos em bloco, na casa de dezenas
//         de microssegundos) mais o texto de duas páginas — bem abaixo dos
//         16,6 ms de um campo NTSC. Se apertar, aumente SLIDE_STEP.
//
//  Depende apenas do M5GFX e recebe o destino como lgfx::LovyanGFX*, então roda
//  igual no painel CVBS do aparelho e no painel SDL do simulador.
// ============================================================================

#include "fj/Gfx.h"
#include <stdint.h>

namespace crt {
namespace fx {

// Redesenha uma tela inteira em `dst`, com todo ponto deslocado de (ox, oy).
using Paint = void (*)(lgfx::LovyanGFX *dst, int ox, int oy, void *user);

struct Screen {
  Paint paint;
  void *user;
};

inline Screen screen(Paint p, void *user = nullptr) {
  Screen s;
  s.paint = p;
  s.user = user;
  return s;
}

// Para onde o conteúdo novo caminha. No slide é a direção do empurrão; no wipe
// é a direção em que a cortina avança.
enum Dir : uint8_t { DIR_LEFT, DIR_RIGHT, DIR_DOWN, DIR_UP };

// Matriz de Bayer 8x8 clássica: 64 níveis, cada valor aparece uma única vez na
// célula. Fica num static de função para o header continuar sem .cpp e sem
// duplicar o símbolo entre unidades de tradução.
inline const uint8_t *bayer8() {
  static const uint8_t m[64] = {
      0,  32, 8,  40, 2,  34, 10, 42, //
      48, 16, 56, 24, 50, 18, 58, 26, //
      12, 44, 4,  36, 14, 46, 6,  38, //
      60, 28, 52, 20, 62, 30, 54, 22, //
      3,  35, 11, 43, 1,  33, 9,  41, //
      51, 19, 59, 27, 49, 17, 57, 25, //
      15, 47, 7,  39, 13, 45, 5,  37, //
      63, 31, 55, 23, 61, 29, 53, 21, //
  };
  return m;
}

class Transition {
public:
  static const int LEVELS = 64;    // níveis do dither (matriz 8x8)
  static const int SLIDE_STEP = 8; // px: granularidade do empurrão
  static const int WIPE_STEP = 4;  // px: altura mínima da banda da cortina

  explicit Transition(lgfx::LovyanGFX *dst = nullptr) : dst_(dst) { reset(); }

  void attach(lgfx::LovyanGFX *dst) { dst_ = dst; }

  // -- início das transições -------------------------------------------------

  // Esmaece a tela atual para uma cor sólida. Não usa buffer nenhum.
  void fadeOut(uint32_t now, uint32_t ms = 420, uint16_t color = 0) {
    begin(KIND_FADE, now, ms);
    color_ = color;
    src_ = nullptr;
    chain_ = false;
  }

  // Esmaece a partir de uma cor sólida até `to`. Pinta `to` dentro de `scratch`
  // (320x240, 8 bits) uma única vez e revela a partir dali.
  // Devolve false se não houve sprite utilizável — nesse caso `to` é pintada
  // de uma vez só (corte seco) e a transição já nasce concluída.
  bool fadeIn(uint32_t now, const Screen &to, lgfx::LGFX_Sprite *scratch, uint32_t ms = 420,
              uint16_t color = 0) {
    if (!prepareSource(to, scratch))
      return hardCut(to);
    begin(KIND_DISSOLVE, now, ms);
    dst_->fillScreen(color); // garante o ponto de partida mesmo sem fadeOut antes
    touched_ += (uint32_t)dst_->width() * dst_->height();
    to_ = to;
    color_ = color;
    chain_ = false;
    return true;
  }

  // Dissolve a tela que já está no framebuffer para `to`, sem passar pelo preto.
  bool crossfade(uint32_t now, const Screen &to, lgfx::LGFX_Sprite *scratch, uint32_t ms = 640) {
    if (!prepareSource(to, scratch))
      return hardCut(to);
    begin(KIND_DISSOLVE, now, ms);
    to_ = to;
    chain_ = false;
    return true;
  }

  // Apaga para `color` e, ao chegar no preto, reacende em `to` — a transição
  // mais usada pelo The Weather Channel entre blocos diferentes.
  // Sem sprite, vira esmaecimento para preto seguido de corte seco (false).
  bool fadeThrough(uint32_t now, const Screen &to, lgfx::LGFX_Sprite *scratch, uint32_t msOut = 380,
                   uint32_t msIn = 380, uint16_t color = 0) {
    // O sprite é pintado agora, antes do primeiro passo visível: o soluço da
    // PSRAM some atrás do esmaecimento em vez de aparecer no meio dele.
    const bool ok = prepareSource(to, scratch);
    begin(KIND_FADE, now, msOut);
    color_ = color;
    to_ = to;
    chainMs_ = msIn;
    chain_ = true;
    chainDither_ = ok;
    return ok;
  }

  // Empurra a tela `from` para fora enquanto `to` entra pelo lado oposto.
  // Nenhum buffer: as duas páginas são redesenhadas recortadas, a cada passo.
  void slide(uint32_t now, const Screen &from, const Screen &to, Dir dir, uint32_t ms = 520) {
    begin(KIND_SLIDE, now, ms);
    from_ = from;
    to_ = to;
    dir_ = dir;
    src_ = nullptr;
    chain_ = false;
  }

  // Cortina: `to` é revelada banda a banda por cima do que já está na tela.
  // O efeito mais barato — no total repinta a tela exatamente uma vez.
  void wipe(uint32_t now, const Screen &to, Dir dir = DIR_DOWN, uint32_t ms = 480) {
    begin(KIND_WIPE, now, ms);
    to_ = to;
    dir_ = dir;
    src_ = nullptr;
    chain_ = false;
  }

  // -- avanço ----------------------------------------------------------------

  // Desenha o passo correspondente a `now`. Devolve true quando a transição
  // terminou (e também quando não há nenhuma em curso), para o chamador poder
  // escrever `if (tx.tick(now)) { ... voltou ao desenho normal ... }`.
  bool tick(uint32_t now) {
    if (kind_ == KIND_IDLE)
      return true;
    if (dst_ == nullptr) {
      // Sem destino não há o que desenhar. Encerrar de verdade, senão busy()
      // continuaria true e o laço `if (busy()) tick(); else desenhaNormal();`
      // travaria para sempre.
      kind_ = KIND_IDLE;
      src_ = nullptr;
      chain_ = false;
      return true;
    }
    switch (kind_) {
    case KIND_FADE:
    case KIND_DISSOLVE:
      tickDither(now);
      break;
    case KIND_SLIDE:
      tickSlide(now);
      break;
    case KIND_WIPE:
      tickWipe(now);
      break;
    default:
      break;
    }
    return kind_ == KIND_IDLE;
  }

  bool busy() const { return kind_ != KIND_IDLE; }

  // Vai direto ao quadro final (troca de modo, botão apertado no meio, etc.).
  void skip() {
    if (kind_ == KIND_IDLE || dst_ == nullptr)
      return;
    dst_->clearClipRect();
    if (kind_ == KIND_FADE && !chain_) {
      dst_->fillScreen(color_);
      touched_ += (uint32_t)dst_->width() * dst_->height();
    } else if (kind_ == KIND_DISSOLVE && src_) {
      src_->pushSprite(dst_, 0, 0);
      touched_ += (uint32_t)dst_->width() * dst_->height();
    } else if (to_.paint) {
      to_.paint(dst_, 0, 0, to_.user);
      touched_ += (uint32_t)dst_->width() * dst_->height();
    }
    kind_ = KIND_IDLE;
    src_ = nullptr;
    chain_ = false;
  }

  // -- diagnóstico (usado pelo probe para relatar custo) ---------------------
  uint32_t steps() const { return steps_; }     // passos realmente desenhados
  uint32_t touched() const { return touched_; } // pixels escritos/repintados
  int progress() const { return progress_; }    // 0..100 do trecho atual

private:
  enum Kind : uint8_t { KIND_IDLE, KIND_FADE, KIND_DISSOLVE, KIND_SLIDE, KIND_WIPE };

  void reset() {
    kind_ = KIND_IDLE;
    from_.paint = nullptr;
    from_.user = nullptr;
    to_.paint = nullptr;
    to_.user = nullptr;
    src_ = nullptr;
    color_ = 0;
    dir_ = DIR_LEFT;
    t0_ = 0;
    dur_ = 1;
    done_ = 0;
    steps_ = 0;
    touched_ = 0;
    progress_ = 0;
    chain_ = false;
    chainDither_ = false;
    chainMs_ = 0;
  }

  void begin(Kind k, uint32_t now, uint32_t ms) {
    kind_ = k;
    t0_ = now;
    dur_ = ms ? ms : 1;
    done_ = 0;
    steps_ = 0;
    touched_ = 0;
    progress_ = 0;
    if (dst_)
      dst_->clearClipRect();
  }

  // Pinta `to` dentro do sprite do chamador. false = sprite inutilizável.
  bool prepareSource(const Screen &to, lgfx::LGFX_Sprite *scratch) {
    src_ = nullptr;
    if (!dst_ || !to.paint || !scratch || !scratch->getBuffer())
      return false;
    if (scratch->width() < dst_->width() || scratch->height() < dst_->height())
      return false;
    // revealLevel() lê o sprite como RGB332. Num sprite de 16 bpp isso pegaria o
    // byte baixo de um RGB565 e pintaria cor aleatória — e o LGFX_Sprite nasce em
    // rgb565_2Byte, então esquecer o setColorDepth(8) é o erro provável. Recusar
    // aqui degrada para corte seco, que é o contrato documentado.
    if (scratch->getColorDepth() != lgfx::color_depth_t::rgb332_1Byte)
      return false;
    scratch->clearClipRect();
    to.paint(scratch, 0, 0, to.user);
    src_ = scratch;
    return true;
  }

  bool hardCut(const Screen &to) {
    kind_ = KIND_IDLE;
    src_ = nullptr;
    chain_ = false;
    if (dst_ && to.paint) {
      dst_->clearClipRect();
      to.paint(dst_, 0, 0, to.user);
      touched_ = (uint32_t)dst_->width() * dst_->height();
      steps_ = 1;
    }
    progress_ = 100;
    return false;
  }

  // Escreve um nível da matriz de Bayer sobre a tela: 1/64 dos pixels, na malha
  // de 8 em 8. Sem origem (src_) escreve a cor sólida; com origem copia o pixel
  // equivalente do sprite. Tudo dentro de um único startWrite.
  void revealLevel(int level) {
    const uint8_t *m = bayer8();
    int mx = 0, my = 0;
    for (int i = 0; i < 64; ++i) {
      if (m[i] == level) {
        mx = i & 7;
        my = i >> 3;
        break;
      }
    }
    const int w = dst_->width(), h = dst_->height();
    // A escolha é pelo tipo de transição, não pela existência de src_: durante
    // o primeiro tempo de um fadeThrough o sprite do destino já está pintado,
    // mas quem tem de aparecer ali ainda é a cor sólida.
    const bool fromSprite = (kind_ == KIND_DISSOLVE) && (src_ != nullptr);
    dst_->startWrite();
    if (fromSprite) {
      for (int y = my; y < h; y += 8)
        for (int x = mx; x < w; x += 8)
          dst_->writePixel(x, y, lgfx::rgb332_t((uint8_t)src_->readPixelValue(x, y)));
    } else {
      dst_->setColor(lgfx::rgb565_t(color_));
      for (int y = my; y < h; y += 8)
        for (int x = mx; x < w; x += 8)
          dst_->writePixel(x, y);
    }
    dst_->endWrite();
    touched_ += (uint32_t)((w + 7 - mx) / 8) * (uint32_t)((h + 7 - my) / 8);
  }

  void tickDither(uint32_t now) {
    int target = (int)((uint64_t)elapsed(now) * LEVELS / dur_);
    if (target > LEVELS)
      target = LEVELS;
    progress_ = target * 100 / LEVELS;
    if (target <= done_) {
      if (done_ >= LEVELS)
        finishDither(now);
      return;
    }
    while (done_ < target)
      revealLevel(done_++);
    ++steps_;
    if (done_ >= LEVELS)
      finishDither(now);
  }

  void finishDither(uint32_t now) {
    if (chain_) {
      // Segundo tempo do fadeThrough: a tela está na cor sólida, agora reacende.
      chain_ = false;
      if (chainDither_ && src_) {
        const uint32_t st = steps_, px = touched_;
        begin(KIND_DISSOLVE, now, chainMs_);
        steps_ = st; // o relatório de custo vale pela transição inteira
        touched_ = px;
        return;
      }
      hardCut(to_);
      return;
    }
    kind_ = KIND_IDLE;
    src_ = nullptr;
    progress_ = 100;
  }

  void tickSlide(uint32_t now) {
    const int w = dst_->width(), h = dst_->height();
    const bool horiz = (dir_ == DIR_LEFT || dir_ == DIR_RIGHT);
    const int span = horiz ? w : h;
    int s = (int)((uint64_t)elapsed(now) * span / dur_);
    if (s > span)
      s = span;
    progress_ = s * 100 / span;
    const bool last = (s >= span);
    // Passos de 8 px: abaixo disso o movimento não é visível num tubo e só
    // gastaria um repinte duplo de tela cheia à toa.
    if (!last)
      s &= ~(SLIDE_STEP - 1);
    if (s == done_ && !last)
      return;
    done_ = s;
    ++steps_;

    if (last) {
      dst_->clearClipRect();
      if (to_.paint)
        to_.paint(dst_, 0, 0, to_.user);
      touched_ += (uint32_t)w * h;
      kind_ = KIND_IDLE;
      progress_ = 100;
      return;
    }

    // Duas pinturas recortadas, complementares: somadas cobrem a tela uma vez.
    int fx0, fy0, tx0, ty0;
    if (dir_ == DIR_LEFT) {
      fx0 = -s, fy0 = 0, tx0 = w - s, ty0 = 0;
      paintClipped(from_, 0, 0, w - s, h, fx0, fy0);
      paintClipped(to_, w - s, 0, s, h, tx0, ty0);
    } else if (dir_ == DIR_RIGHT) {
      fx0 = s, fy0 = 0, tx0 = s - w, ty0 = 0;
      paintClipped(to_, 0, 0, s, h, tx0, ty0);
      paintClipped(from_, s, 0, w - s, h, fx0, fy0);
    } else if (dir_ == DIR_UP) {
      fx0 = 0, fy0 = -s, tx0 = 0, ty0 = h - s;
      paintClipped(from_, 0, 0, w, h - s, fx0, fy0);
      paintClipped(to_, 0, h - s, w, s, tx0, ty0);
    } else { // DIR_DOWN
      fx0 = 0, fy0 = s, tx0 = 0, ty0 = s - h;
      paintClipped(to_, 0, 0, w, s, tx0, ty0);
      paintClipped(from_, 0, s, w, h - s, fx0, fy0);
    }
    dst_->clearClipRect();
  }

  void tickWipe(uint32_t now) {
    const int w = dst_->width(), h = dst_->height();
    const bool horiz = (dir_ == DIR_LEFT || dir_ == DIR_RIGHT);
    const int span = horiz ? w : h;
    int p = (int)((uint64_t)elapsed(now) * span / dur_);
    if (p > span)
      p = span;
    progress_ = p * 100 / span;
    const bool last = (p >= span);
    if (!last)
      p &= ~(WIPE_STEP - 1);
    if (p <= done_)
      return;

    // Só a banda recém-descoberta é repintada; no total a tela sai uma vez só.
    const int band = p - done_;
    if (dir_ == DIR_DOWN)
      paintClipped(to_, 0, done_, w, band, 0, 0);
    else if (dir_ == DIR_UP)
      paintClipped(to_, 0, h - p, w, band, 0, 0);
    else if (dir_ == DIR_RIGHT)
      paintClipped(to_, done_, 0, band, h, 0, 0);
    else // DIR_LEFT
      paintClipped(to_, w - p, 0, band, h, 0, 0);
    dst_->clearClipRect();

    done_ = p;
    ++steps_;
    if (last) {
      kind_ = KIND_IDLE;
      progress_ = 100;
    }
  }

  void paintClipped(const Screen &s, int cx, int cy, int cw, int ch, int ox, int oy) {
    if (cw <= 0 || ch <= 0 || !s.paint)
      return;
    dst_->setClipRect(cx, cy, cw, ch);
    s.paint(dst_, ox, oy, s.user);
    touched_ += (uint32_t)cw * ch;
  }

  uint32_t elapsed(uint32_t now) const { return now - t0_; } // seguro no wrap de millis()

  lgfx::LovyanGFX *dst_;
  lgfx::LGFX_Sprite *src_;
  Screen from_, to_;
  Kind kind_;
  Dir dir_;
  uint16_t color_;
  uint32_t t0_, dur_, chainMs_;
  int done_, progress_;
  uint32_t steps_, touched_;
  bool chain_, chainDither_;
};

} // namespace fx
} // namespace crt
