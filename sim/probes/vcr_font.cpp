// ============================================================================
//  Probe da VcrFont — bancada para olhar a fonte do OSD glifo por glifo.
//
//  Roda o MESMO rasterizador M5GFX do aparelho, em RGB332 (setColorDepth(8)),
//  e grava PNGs. O ponto é conferir com o olho o que nenhuma asserção pega:
//  traço que afina, contador que fecha, letra que vira outra.
//
//    make -C sim probes && ./sim/build/probe_vcr_font
//
//  Sai em sim/build/:
//    vcr_font_grid.png     conjunto completo, 1x e 2x, com a célula marcada
//    vcr_font_zoom*.png    os glifos ampliados 10x, com a grade de pixels
//    vcr_font_osd.png      frases reais do OSD sobre vídeo falso, 320x240
//    vcr_font_compare.png  a mesma frase na VcrFont e nas fonts::FontN
//
//  Imprime também a contagem de fillRect por caractere, que é o número citado
//  no orçamento do include/VcrFont.h.
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SafeArea.h"
#include "VcrFont.h"

using namespace crt;

static lgfx::Panel_sdl panel;
static M5GFX rca;

static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:.-/%+?!";

// ---------------------------------------------------------------------------
//  Saída em arquivo. O binário pode ser chamado da raiz do repositório ou de
//  dentro de sim/, então tentamos os dois caminhos antes de desistir.
// ---------------------------------------------------------------------------
static bool savePng(LovyanGFX *gfx, int w, int h, const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)gfx->createPng(&len, 0, 0, w, h);
  if (!png) {
    fprintf(stderr, "falha ao gerar PNG de %s\n", name);
    return false;
  }
  char path[256];
  FILE *f = nullptr;
  for (const char *dir : {"sim/build", "build"}) {
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((f = fopen(path, "wb")))
      break;
  }
  if (!f) {
    fprintf(stderr, "nao consegui escrever %s\n", name);
    free(png);
    return false;
  }
  fwrite(png, 1, len, f);
  fclose(f);
  free(png);
  printf("[probe] %s (%zu bytes)\n", path, len);
  return true;
}

// ---------------------------------------------------------------------------
//  Custo: conta os fillRect que o blit emitiria, sem pintar nada.
// ---------------------------------------------------------------------------
static int rectCount(const uint16_t *rows) {
  int n = 0;
  for (int r = 0; r < vcrfont::CELL_H;) {
    const uint16_t m = rows[r];
    if (!m) {
      ++r;
      continue;
    }
    int h = 1;
    while (r + h < vcrfont::CELL_H && rows[r + h] == m)
      ++h;
    for (int c = 0; c < vcrfont::CELL_W;) {
      if (!(m & (1u << (vcrfont::CELL_W - 1 - c)))) {
        ++c;
        continue;
      }
      ++n;
      while (c < vcrfont::CELL_W && (m & (1u << (vcrfont::CELL_W - 1 - c))))
        ++c;
    }
    r += h;
  }
  return n;
}

static int inkCount(const uint16_t *rows) {
  int n = 0;
  for (int r = 0; r < vcrfont::CELL_H; ++r)
    for (int c = 0; c < vcrfont::CELL_W; ++c)
      if (rows[r] & (1u << (vcrfont::CELL_W - 1 - c)))
        ++n;
  return n;
}

static void reportCost() {
  int rects = 0, ink = 0, ring = 0, ringRects = 0, worstRects = 0;
  char worst = ' ';
  for (const char *p = CHARSET; *p; ++p) {
    const uint16_t *g = vcrfont::glyph(*p);
    uint16_t dil[vcrfont::CELL_H], halo[vcrfont::CELL_H];
    vcrfont::detail::dilate(g, dil);
    for (int r = 0; r < vcrfont::CELL_H; ++r)
      halo[r] = (uint16_t)(dil[r] & ~g[r]);

    const int r = rectCount(g);
    rects += r;
    ink += inkCount(g);
    ring += inkCount(halo);
    ringRects += rectCount(halo);
    if (r > worstRects) {
      worstRects = r;
      worst = *p;
    }
  }
  const double n = (double)strlen(CHARSET);
  printf("[probe] %d glifos na tabela, %d bytes de flash\n", vcrfont::GLYPH_COUNT, vcrfont::FLASH_BYTES);
  printf("[probe] sem contorno: %.1f fillRect e %.1f px por char (pior: '%c', %d fillRect)\n", rects / n,
         ink / n, worst, worstRects);
  printf("[probe] contorno por dilatacao: +%.1f fillRect e +%.1f px (%.2fx os px do glifo)\n", ringRects / n,
         ring / n, ring / (double)ink);
  printf("[probe] total com contorno %.2fx um glifo simples; a versao ingenua (8 copias "
         "deslocadas) daria 9.00x\n",
         (ink + ring) / (double)ink);
}

// ---------------------------------------------------------------------------
//  (a) Conjunto completo em grade.
// ---------------------------------------------------------------------------
static void pageGrid() {
  const int COLS = 11;
  const int n = (int)strlen(CHARSET);
  const int rows = (n + COLS - 1) / COLS;
  const int cw = vcrfont::CELL_W * 2, ch = vcrfont::CELL_H * 2;
  const int pad = 8, top = 28;
  const int Wp = pad * 2 + COLS * (cw + pad);
  const int Hp = top + rows * (ch + pad) + 74;

  LGFX_Sprite page(&rca);
  page.setColorDepth(8);
  if (!page.createSprite(Wp, Hp)) {
    fprintf(stderr, "sem memoria para a pagina da grade\n");
    return;
  }
  page.fillSprite(page.color888(16, 16, 24));

  page.setTextColor(page.color888(140, 140, 160));
  page.setFont(&fonts::Font0);
  page.drawString("VcrFont 12x16 - celula marcada em cinza, tinta em branco", pad, 8);

  for (int i = 0; i < n; ++i) {
    const int x = pad + (i % COLS) * (cw + pad);
    const int y = top + (i / COLS) * (ch + pad);
    // A moldura mostra os limites da célula: é aqui que se vê se a folga
    // lateral está mesmo sobrando dos dois lados.
    page.drawRect(x, y, cw, ch, page.color888(70, 70, 90));
    vcrfont::drawChar(&page, CHARSET[i], x, y, TFT_WHITE, 2);
  }

  // Linha em tamanho real (1x), que é como o OSD realmente aparece.
  int y = top + rows * (ch + pad) + 10;
  page.setTextColor(page.color888(140, 140, 160));
  page.drawString("tamanho real (1x):", pad, y);
  y += 12;
  vcrfont::drawText(&page, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", pad, y, TFT_WHITE, 1);
  y += 18;
  vcrfont::drawText(&page, "0123456789 :.-/%+?!", pad, y, TFT_WHITE, 1);

  savePng(&page, Wp, Hp, "vcr_font_grid.png");
  page.deleteSprite();
}

// ---------------------------------------------------------------------------
//  Os glifos que costumam desandar neste tamanho, bem grandes.
// ---------------------------------------------------------------------------
static void pageZoom(const char *hard, const char *file) {
  const int n = (int)strlen(hard);
  const int s = 10;
  const int cw = vcrfont::CELL_W * s, ch = vcrfont::CELL_H * s;
  const int COLS = 5;
  const int rows = (n + COLS - 1) / COLS;
  const int pad = 10, top = 24;
  const int Wp = pad + COLS * (cw + pad);
  const int Hp = top + rows * (ch + pad);

  LGFX_Sprite page(&rca);
  page.setColorDepth(8);
  if (!page.createSprite(Wp, Hp)) {
    fprintf(stderr, "sem memoria para a pagina de zoom\n");
    return;
  }
  page.fillSprite(page.color888(16, 16, 24));
  page.setFont(&fonts::Font0);
  page.setTextColor(page.color888(140, 140, 160));
  page.drawString("10x - a grade e o pixel do bitmap; o traco tem que dar 2 quadrados", pad, 6);

  for (int i = 0; i < n; ++i) {
    const int x = pad + (i % COLS) * (cw + pad);
    const int y = top + (i / COLS) * (ch + pad);
    // Grade de pixels: a espessura do traço tem que dar 2 quadradinhos em
    // qualquer direção; onde der 1, o glifo afinou.
    for (int gx = 0; gx <= vcrfont::CELL_W; ++gx)
      page.drawFastVLine(x + gx * s, y, ch + 1, page.color888(48, 48, 64));
    for (int gy = 0; gy <= vcrfont::CELL_H; ++gy)
      page.drawFastHLine(x, y + gy * s, cw + 1, page.color888(48, 48, 64));
    vcrfont::drawChar(&page, hard[i], x, y, TFT_WHITE, s);
  }

  savePng(&page, Wp, Hp, file);
  page.deleteSprite();
}

// ---------------------------------------------------------------------------
//  Detalhe do contorno: o anel que a dilatação produz, em escala grande e
//  sobre um fundo que muda de claro para escuro no meio da palavra.
// ---------------------------------------------------------------------------
static void pageOutline() {
  const char *phrase = "PLAY SP";
  const int s = 4;
  const int Wp = vcrfont::textWidth(phrase, s) + 16;
  const int Hp = 2 * vcrfont::textHeight(s) + 40;

  LGFX_Sprite page(&rca);
  page.setColorDepth(8);
  if (!page.createSprite(Wp, Hp)) {
    fprintf(stderr, "sem memoria para a pagina de contorno\n");
    return;
  }
  for (int x = 0; x < Wp; ++x)
    page.drawFastVLine(x, 0, Hp, x < Wp / 2 ? page.color888(230, 230, 220) : page.color888(20, 20, 20));

  page.setFont(&fonts::Font0);
  page.setTextColor(page.color888(255, 0, 255));
  page.drawString("sem contorno", 8, 4);
  vcrfont::drawText(&page, phrase, 8, 14, TFT_WHITE, s);
  page.drawString("com contorno (dilatacao)", 8, 24 + vcrfont::textHeight(s));
  vcrfont::drawText(&page, phrase, 8, 34 + vcrfont::textHeight(s), TFT_WHITE, s, TFT_BLACK);

  savePng(&page, Wp, Hp, "vcr_font_outline.png");
  page.deleteSprite();
}

// ---------------------------------------------------------------------------
//  Vídeo falso: gradiente sujo com faixas e ruído, para ter tudo que o
//  contorno precisa enfrentar — claro, escuro e borda de contraste.
// ---------------------------------------------------------------------------
static void fakeVideo() {
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      int r = 40 + (x * 150) / W;
      int g = 90 + (y * 120) / H;
      int b = 200 - (x * 160) / W;
      if (((x / 24) + (y / 24)) & 1) {
        r = r * 3 / 4;
        g = g * 3 / 4;
      }
      rca.drawPixel(x, y, rca.color888(r, g, b));
    }
  }
  // Um objeto claro e um escuro cruzando as linhas de texto: é aí que o texto
  // sem contorno some (no claro) e o preto puro some (no escuro).
  rca.fillRect(120, 10, 130, 90, rca.color888(250, 250, 240));
  rca.fillRect(40, 150, 110, 70, rca.color888(12, 12, 12));
}

// ---------------------------------------------------------------------------
//  (b) Frases reais do OSD, tamanho real, com e sem contorno.
// ---------------------------------------------------------------------------
static void pageOsd() {
  fakeVideo();

  const int32_t white = TFT_WHITE;
  const int32_t black = TFT_BLACK;

  // As duas primeiras linhas sem contorno: onde cruzam o objeto branco, somem.
  vcrfont::drawText(&rca, "PLAY  SP  0:12:34", SAFE_L, SAFE_T, white, 1);
  vcrfont::drawText(&rca, "M5 RETRO TV", SAFE_L, SAFE_T + 18, white, 1);

  // As mesmas duas com contorno: legíveis sobre o mesmo objeto.
  vcrfont::drawText(&rca, "PLAY  SP  0:12:34", SAFE_L, SAFE_T + 42, white, 1, black);
  vcrfont::drawText(&rca, "M5 RETRO TV", SAFE_L, SAFE_T + 60, white, 1, black);

  // 2x com contorno, centralizado — o modo 24x32 dos títulos.
  vcrfont::drawTextCentered(&rca, "REC 100%", 0, 96, W, rca.color565(255, 80, 80), 2, black);

  // Linha com as letras de diagonal difícil, em tamanho real sobre o preto.
  vcrfont::drawText(&rca, "VHS/DVD MIX WXYZ", 4, 138, white, 1, black);

  // Faixa do OSD, tal como o player a usa.
  vcrfont::drawText(&rca, "CH 03 - 12/25", SAFE_L, crt::SAFE_B - 40, white, 1, black);
  vcrfont::drawTextCentered(&rca, "SP 0:12:34", 0, crt::SAFE_B - 22, W, white, 1, black);

  savePng(&rca, W, H, "vcr_font_osd.png");
}

// ---------------------------------------------------------------------------
//  (c) Lado a lado com as fontes embutidas, para ver a diferença de traço.
// ---------------------------------------------------------------------------
static void pageCompare() {
  const int Wp = 420, Hp = 240;
  LGFX_Sprite page(&rca);
  page.setColorDepth(8);
  if (!page.createSprite(Wp, Hp)) {
    fprintf(stderr, "sem memoria para a pagina de comparacao\n");
    return;
  }
  page.fillSprite(page.color888(24, 24, 128)); // o azul das telas do firmware

  const char *phrase = "PLAY SP 0:12:34";
  const int32_t white = TFT_WHITE;
  const int32_t label = page.color565(120, 160, 200);
  const int32_t black = TFT_BLACK;

  page.setFont(&fonts::Font0);
  page.setTextColor(label);

  int y = 10;
  page.drawString("VcrFont 1x (12x16)", 8, y);
  vcrfont::drawText(&page, phrase, 8, y + 12, white, 1);
  y += 44;

  page.setTextColor(label);
  page.drawString("VcrFont 1x + contorno", 8, y);
  vcrfont::drawText(&page, phrase, 8, y + 12, white, 1, black);
  y += 44;

  page.setTextColor(label);
  page.drawString("fonts::Font2 (16 px)", 8, y);
  page.setFont(&fonts::Font2);
  page.setTextColor(white);
  page.drawString(phrase, 8, y + 12);
  y += 44;

  page.setFont(&fonts::Font0);
  page.setTextColor(label);
  page.drawString("fonts::Font0 setTextSize(2) (12x16)", 8, y);
  page.setTextColor(white);
  page.setTextSize(2);
  page.drawString(phrase, 8, y + 12);
  page.setTextSize(1);
  y += 44;

  page.setTextColor(label);
  page.drawString("fonts::Font4 (26 px)", 8, y);
  page.setFont(&fonts::Font4);
  page.setTextColor(white);
  page.drawString(phrase, 8, y + 12);

  savePng(&page, Wp, Hp, "vcr_font_compare.png");
  page.deleteSprite();
}

int main(int, char **) {
  auto cfg = panel.config();
  cfg.memory_width = cfg.panel_width = W;
  cfg.memory_height = cfg.panel_height = H;
  panel.config(cfg);
  panel.setScaling(3, 3);
  rca.setPanel(&panel);
  if (!rca.init()) {
    fprintf(stderr, "falha ao iniciar o painel SDL\n");
    return 1;
  }
  rca.setColorDepth(8); // RGB332, igual ao firmware na saida composta

  reportCost();
  pageGrid();
  pageZoom("SGRMWQ4689", "vcr_font_zoom.png");
  pageZoom("0BDOKNXYZ%", "vcr_font_zoom2.png");
  pageZoom("JI17V52TAP", "vcr_font_zoom3.png");
  pageOutline();
  pageOsd();
  pageCompare();
  return 0;
}
