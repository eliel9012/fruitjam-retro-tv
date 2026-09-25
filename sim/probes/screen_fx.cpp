// ============================================================================
//  probe_screen_fx — bancada do include/ScreenFx.h
//
//  Roda cada transição contra um relógio sintético de 60 Hz (16 ms por quadro,
//  a cadência de quadro do DVI 640x480@60) e grava cinco PNGs por efeito, nas
//  marcas de 0%, 25%, 50%, 75% e 100% da duração. O painel está em 16 bits
//  (RGB565), a mesma profundidade do canvas `tv` do Fruit Jam.
//
//  Ao final imprime o custo medido: quantos passos a transição desenhou e
//  quantos pixels foram escritos/repintados no total. É esse número que decide
//  se o efeito cabe no orçamento por quadro do aparelho.
//
//  E confere, pixel a pixel, que o crossfade com sprite de 16 bits termina
//  EXATAMENTE na página de destino — um swap de bytes esquecido na leitura do
//  sprite passaria despercebido no olho (cor parecida) e aparece aqui.
//
//    make -C sim probes && ./sim/build/probe_screen_fx
// ============================================================================

#include "fj/Gfx.h" // LovyanGFX + backend SDL, a mesma porta de entrada do firmware
#include "SimPanel.h" // painel SDL em 320x240, não no 240x320 padrão da LovyanGFX

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "SafeArea.h"
#include "ScreenFx.h"

using namespace crt;

static lgfx::Panel_sdl panel;
static lgfx::LGFX_Device tv;

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
  uint8_t *png = (uint8_t *)tv.createPng(&len, 0, 0, W, H);
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
    t += 16;         // um quadro a 60 Hz
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
  sim::configure(panel);
  tv.setPanel(&panel);
  if (!tv.init()) {
    fprintf(stderr, "falha ao inicializar o painel SDL\n");
    return 1;
  }
  tv.setColorDepth(16); // RGB565, igual ao canvas `tv` do Fruit Jam

  // Sprite de composição do chamador: 320x240 a 16 bits = 153.600 bytes. No
  // aparelho o buffer vem de ps_malloc() e entra por setBuffer() — o
  // setPsram(true) da LovyanGFX no rp2040 é malloc comum (ver ScreenFx.h).
  // Aqui o mesmo caminho, com malloc de desktop.
  uint16_t *scratchPx = (uint16_t *)malloc((size_t)W * H * 2);
  lgfx::LGFX_Sprite scratch(&tv);
  if (scratchPx)
    scratch.setBuffer(scratchPx, W, H, 16);
  const bool hasScratch = scratch.getBuffer() != nullptr;
  printf("sprite de composicao: %s\n\n", hasScratch ? "ok (153600 bytes, RGB565)" : "INDISPONIVEL");
  int failures = 0;

  crt::fx::Transition tx(&tv);
  const crt::fx::Screen now = crt::fx::screen(pageNow);
  const crt::fx::Screen fc = crt::fx::screen(pageForecast);
  uint32_t frames;

  // 1. Esmaecimento para preto.
  pageNow(&tv, 0, 0, nullptr);
  tx.fadeOut(0, 480, TFT_BLACK);
  frames = drive(tx, "fadeout", 480).frames;
  report("fadeout", tx, frames);

  // 2. Esmaecimento a partir do preto.
  tv.fillScreen(TFT_BLACK);
  tx.fadeIn(0, fc, &scratch, 480, TFT_BLACK);
  frames = drive(tx, "fadein", 480).frames;
  report("fadein", tx, frames);

  // 3. Crossfade direto entre as duas páginas.
  pageNow(&tv, 0, 0, nullptr);
  tx.crossfade(0, fc, &scratch, 640);
  frames = drive(tx, "crossfade", 640).frames;
  report("crossfade", tx, frames);
  {
    // O quadro final do crossfade tem de ser idêntico à página desenhada direto.
    std::vector<uint16_t> got((size_t)W * H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        got[(size_t)y * W + x] = tv.readPixel(x, y);
    pageForecast(&tv, 0, 0, nullptr);
    int diff = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        diff += got[(size_t)y * W + x] != tv.readPixel(x, y);
    printf("crossfade 16 bits: %d pixels diferentes da pagina de destino (esperado: 0)\n\n", diff);
    if (diff)
      ++failures;
  }

  // 4. Esmaecimento para preto e de volta (o par clássico do TWC).
  pageNow(&tv, 0, 0, nullptr);
  tx.fadeThrough(0, fc, &scratch, 380, 380, TFT_BLACK);
  frames = drive(tx, "fadethrough", 760).frames;
  report("fadethrough", tx, frames);

  // 5. Deslizamento horizontal: a página atual sai pela esquerda.
  pageNow(&tv, 0, 0, nullptr);
  tx.slide(0, now, fc, crt::fx::DIR_LEFT, 520);
  frames = drive(tx, "slide", 520).frames;
  report("slide", tx, frames);

  // 6. Cortina vertical de cima para baixo.
  pageNow(&tv, 0, 0, nullptr);
  tx.wipe(0, fc, crt::fx::DIR_DOWN, 480);
  frames = drive(tx, "wipe", 480).frames;
  report("wipe", tx, frames);

  // 7. Degradação sem sprite: crossfade deve virar corte seco e devolver false.
  pageNow(&tv, 0, 0, nullptr);
  const bool ok = tx.crossfade(0, fc, nullptr, 640);
  printf("crossfade sem sprite: devolveu %s, busy=%s (esperado: false/false)\n", ok ? "true" : "false",
         tx.busy() ? "true" : "false");
  if (ok || tx.busy())
    ++failures;

  // 7b. Profundidade que o ScreenFx não sabe reinterpretar (24 bits): também
  //     corte seco. E 8 bits (RGB332, o contrato do upstream) continua aceito.
  {
    lgfx::LGFX_Sprite deep(&tv);
    deep.setColorDepth(24);
    deep.createSprite(W, H);
    pageNow(&tv, 0, 0, nullptr);
    const bool ok24 = tx.crossfade(0, fc, &deep, 640);
    tx.skip();
    lgfx::LGFX_Sprite small(&tv);
    small.setColorDepth(8);
    small.createSprite(W, H);
    pageNow(&tv, 0, 0, nullptr);
    const bool ok8 = tx.crossfade(0, fc, &small, 640);
    tx.skip();
    printf("crossfade com sprite de 24 bits: %s (esperado: false); de 8 bits: %s (esperado: true)\n",
           ok24 ? "true" : "false", ok8 ? "true" : "false");
    if (ok24 || !ok8)
      ++failures;
    deep.deleteSprite();
    small.deleteSprite();
  }

  // 8. skip() no meio da transição: deve saltar direto ao quadro final.
  pageNow(&tv, 0, 0, nullptr);
  tx.slide(0, now, fc, crt::fx::DIR_LEFT, 520);
  tx.tick(160);
  tx.skip();
  shot("skip", 0);
  printf("skip no meio do slide: busy=%s (esperado: false)\n", tx.busy() ? "true" : "false");
  if (tx.busy())
    ++failures;

  scratch.deleteSprite(); // só solta a referência: o buffer é nosso
  free(scratchPx);
  printf("\n%s\n", failures ? "FALHOU" : "ok");
  return failures ? 1 : 0;
}
