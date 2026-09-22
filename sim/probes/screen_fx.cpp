// ============================================================================
//  probe_screen_fx — bancada do include/ScreenFx.h
//
//  Roda cada transição contra um relógio sintético de 60 Hz (16 ms por quadro,
//  a cadência de campo do NTSC) e grava cinco PNGs por efeito, nas marcas de
//  0%, 25%, 50%, 75% e 100% da duração. O painel está em 8 bits para que a
//  quantização RGB332 do quadro composto apareça nos PNGs tal como na TV.
//
//  Ao final imprime o custo medido: quantos passos a transição desenhou e
//  quantos pixels foram escritos/repintados no total. É esse número que decide
//  se o efeito cabe no orçamento por quadro do ESP32.
//
//    make -C sim probes && ./sim/build/probe_screen_fx
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SafeArea.h"
#include "ScreenFx.h"

using namespace crt;

static lgfx::Panel_sdl panel;
static M5GFX rca;

// ---------------------------------------------------------------------------
//  Duas páginas no espírito da tela de previsão: "condições atuais" e
//  "previsão estendida", que é justamente o par que o Weather Star trocava com
//  deslizamento. Ambas honram (ox, oy) — sem isso o slide não empurra nada.
// ---------------------------------------------------------------------------

static void pageNow(lgfx::LovyanGFX *g, int ox, int oy, void *) {
  const uint16_t bg = g->color565(0, 72, 160);
  g->fillScreen(bg); // recortado pelo clipRect corrente: é o que barateia tudo
  g->setTextDatum(top_center);
  g->setTextSize(1);

  g->setFont(&fonts::Font4);
  g->setTextColor(TFT_YELLOW, bg);
  g->drawString("FRANCA - SP", ox + W / 2, oy + SAFE_T);
  g->drawFastHLine(ox + SAFE_L, oy + SAFE_T + 30, SAFE_W, TFT_CYAN);

  g->setTextColor(TFT_WHITE, bg);
  g->drawString("PARCIALMENTE NUBLADO", ox + W / 2, oy + SAFE_T + 40);
  g->setTextSize(2);
  g->setTextColor(TFT_YELLOW, bg);
  g->drawString("27 C", ox + W / 2, oy + SAFE_T + 76);

  g->setFont(&fonts::Font2);
  g->setTextSize(1);
  g->setTextColor(TFT_WHITE, bg);
  g->drawString("UMIDADE  58%", ox + W / 2, oy + SAFE_T + 138);
  g->drawString("VENTO  12 KM/H  NE", ox + W / 2, oy + SAFE_T + 158);
  g->drawFastHLine(ox + SAFE_L, oy + TICKER_Y - 4, SAFE_W, TFT_CYAN);
}

static void pageForecast(lgfx::LovyanGFX *g, int ox, int oy, void *) {
  const uint16_t bg = g->color565(112, 0, 128);
  g->fillScreen(bg);
  g->setTextDatum(top_center);
  g->setTextSize(1);

  g->setFont(&fonts::Font4);
  g->setTextColor(TFT_YELLOW, bg);
  g->drawString("PREVISAO 3 DIAS", ox + W / 2, oy + SAFE_T);
  g->drawFastHLine(ox + SAFE_L, oy + SAFE_T + 30, SAFE_W, TFT_CYAN);

  static const char *dias[3] = {"QUA", "QUI", "SEX"};
  static const char *maxi[3] = {"29", "31", "26"};
  static const char *mini[3] = {"17", "18", "16"};
  for (int i = 0; i < 3; ++i) {
    const int cx = ox + SAFE_L + SAFE_W * (2 * i + 1) / 6;
    g->setFont(&fonts::Font4);
    g->setTextColor(TFT_CYAN, bg);
    g->drawString(dias[i], cx, oy + SAFE_T + 46);
    g->setTextColor(TFT_YELLOW, bg);
    g->drawString(maxi[i], cx, oy + SAFE_T + 84);
    g->setTextColor(TFT_WHITE, bg);
    g->drawString(mini[i], cx, oy + SAFE_T + 116);
    g->drawRect(cx - 30, oy + SAFE_T + 40, 60, 112, TFT_DARKGREY);
  }
  g->setFont(&fonts::Font2);
  g->setTextColor(TFT_WHITE, bg);
  g->drawString("MAXIMA / MINIMA EM GRAUS C", ox + W / 2, oy + SAFE_T + 160);
  g->drawFastHLine(ox + SAFE_L, oy + TICKER_Y - 4, SAFE_W, TFT_CYAN);
}

// ---------------------------------------------------------------------------
//  Gravação dos PNGs.
// ---------------------------------------------------------------------------

static FILE *openOut(const char *name, int idx) {
  char path[160];
  // Funciona tanto rodando de dentro de sim/ quanto da raiz do repositório.
  const char *dirs[2] = {"build", "sim/build"};
  for (int d = 0; d < 2; ++d) {
    snprintf(path, sizeof(path), "%s/fx_%s_%d.png", dirs[d], name, idx);
    FILE *f = fopen(path, "wb");
    if (f) {
      printf("    %s\n", path);
      return f;
    }
  }
  return nullptr;
}

static void shot(const char *name, int idx) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, W, H);
  if (!png)
    return;
  FILE *f = openOut(name, idx);
  if (f) {
    fwrite(png, 1, len, f);
    fclose(f);
  }
  free(png);
}

// ---------------------------------------------------------------------------
//  Motor do probe: alimenta tick() a 60 Hz e fotografa nas cinco marcas.
// ---------------------------------------------------------------------------

struct Run {
  const char *name;
  uint32_t dur;
  uint32_t frames;
};

static Run drive(crt::fx::Transition &tx, const char *name, uint32_t dur) {
  printf("  %s (%u ms)\n", name, (unsigned)dur);
  static const int marks[5] = {0, 25, 50, 75, 100};
  uint32_t t = 0, frames = 0;
  int next = 0;
  for (;;) {
    const bool done = tx.tick(t);
    ++frames;
    while (next < 5 && (uint32_t)t * 100 >= (uint32_t)marks[next] * dur) {
      shot(name, next);
      ++next;
    }
    if (done)
      break;
    t += 16;         // um campo NTSC
    if (t > dur * 4) // rede de segurança: transição que não termina é bug
      break;
  }
  while (next < 5) {
    shot(name, next);
    ++next;
  }
  Run r = {name, dur, frames};
  return r;
}

static void report(const char *name, const crt::fx::Transition &tx, uint32_t frames) {
  const uint32_t full = (uint32_t)W * H;
  printf("    passos=%u  quadros=%u  pixels=%u (%.2f telas)  media=%u px/passo\n\n", (unsigned)tx.steps(),
         (unsigned)frames, (unsigned)tx.touched(), (double)tx.touched() / full,
         (unsigned)(tx.steps() ? tx.touched() / tx.steps() : 0));
}

int main(int, char **) {
  panel.setScaling(2, 2);
  rca.setPanel(&panel);
  if (!rca.init()) {
    fprintf(stderr, "falha ao inicializar o painel SDL\n");
    return 1;
  }
  rca.setColorDepth(8); // RGB332, igual ao firmware na saída composta

  // Sprite de composição do chamador: 320x240 a 8 bits = 76.800 bytes. No Core2
  // isto tem de ser PSRAM (setPsram(true)); aqui é memória de desktop.
  lgfx::LGFX_Sprite scratch(&rca);
  scratch.setPsram(true);
  scratch.setColorDepth(8);
  scratch.createSprite(W, H);
  const bool hasScratch = scratch.getBuffer() != nullptr;
  printf("sprite de composicao: %s\n\n", hasScratch ? "ok (76800 bytes)" : "INDISPONIVEL");

  crt::fx::Transition tx(&rca);
  const crt::fx::Screen now = crt::fx::screen(pageNow);
  const crt::fx::Screen fc = crt::fx::screen(pageForecast);
  uint32_t frames;

  // 1. Esmaecimento para preto.
  pageNow(&rca, 0, 0, nullptr);
  tx.fadeOut(0, 480, TFT_BLACK);
  frames = drive(tx, "fadeout", 480).frames;
  report("fadeout", tx, frames);

  // 2. Esmaecimento a partir do preto.
  rca.fillScreen(TFT_BLACK);
  tx.fadeIn(0, fc, &scratch, 480, TFT_BLACK);
  frames = drive(tx, "fadein", 480).frames;
  report("fadein", tx, frames);

  // 3. Crossfade direto entre as duas páginas.
  pageNow(&rca, 0, 0, nullptr);
  tx.crossfade(0, fc, &scratch, 640);
  frames = drive(tx, "crossfade", 640).frames;
  report("crossfade", tx, frames);

  // 4. Esmaecimento para preto e de volta (o par clássico do TWC).
  pageNow(&rca, 0, 0, nullptr);
  tx.fadeThrough(0, fc, &scratch, 380, 380, TFT_BLACK);
  frames = drive(tx, "fadethrough", 760).frames;
  report("fadethrough", tx, frames);

  // 5. Deslizamento horizontal: a página atual sai pela esquerda.
  pageNow(&rca, 0, 0, nullptr);
  tx.slide(0, now, fc, crt::fx::DIR_LEFT, 520);
  frames = drive(tx, "slide", 520).frames;
  report("slide", tx, frames);

  // 6. Cortina vertical de cima para baixo.
  pageNow(&rca, 0, 0, nullptr);
  tx.wipe(0, fc, crt::fx::DIR_DOWN, 480);
  frames = drive(tx, "wipe", 480).frames;
  report("wipe", tx, frames);

  // 7. Degradação sem sprite: crossfade deve virar corte seco e devolver false.
  pageNow(&rca, 0, 0, nullptr);
  const bool ok = tx.crossfade(0, fc, nullptr, 640);
  printf("crossfade sem sprite: devolveu %s, busy=%s (esperado: false/false)\n", ok ? "true" : "false",
         tx.busy() ? "true" : "false");

  // 8. skip() no meio da transição: deve saltar direto ao quadro final.
  pageNow(&rca, 0, 0, nullptr);
  tx.slide(0, now, fc, crt::fx::DIR_LEFT, 520);
  tx.tick(160);
  tx.skip();
  shot("skip", 0);
  printf("skip no meio do slide: busy=%s (esperado: false)\n", tx.busy() ? "true" : "false");

  scratch.deleteSprite();
  return 0;
}
