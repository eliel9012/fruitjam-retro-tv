// ============================================================================
//  Probe dos icones de condicao do tempo (include/WeatherIcons.h).
//
//  Desenha todos os icones do catalogo em 48 e em 96 px sobre o mesmo fundo
//  pulsante da tela de previsao e grava PNGs 320x240. Roda headless: o ponto e
//  OLHAR o resultado ja quantizado em RGB332, que e onde tom parecido vira a
//  mesma cor e dois icones viram o mesmo desenho.
//
//    make -C sim probes && ./sim/build/probe_weather_icons
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "SafeArea.h"
#include "WeatherIcons.h"

using namespace crt;

static lgfx::Panel_sdl panel;
static M5GFX rca;

// Todo o catalogo, na ordem em que o olho deve compara-los: as familias que se
// parecem ficam vizinhas, que e onde a ambiguidade aparece.
static const wx::WeatherIcon ICONS[] = {
    wx::WX_CLEAR,      wx::WX_FEW_CLOUDS, wx::WX_PARTLY_CLOUDY, wx::WX_CLOUDY,
    wx::WX_FOG,        wx::WX_DRIZZLE,    wx::WX_RAIN_LIGHT,    wx::WX_RAIN,
    wx::WX_RAIN_HEAVY, wx::WX_SHOWERS,    wx::WX_THUNDER,       wx::WX_THUNDER_HAIL,
    wx::WX_SNOW,       wx::WX_SNOW_GRAINS, wx::WX_UNKNOWN};

// Rotulo curto so do probe: iconName() nao cabe numa celula de 80 px.
static const char *SHORT[] = {"LIMPO",    "POUCAS",   "PARCIAL",  "NUBLADO", "NEVOEIRO",
                              "GAROA",    "CH.FRACA", "CHUVA",    "CH.FORTE", "PANCADAS",
                              "TROVOADA", "GRANIZO",  "NEVE",     "GRAOS",   "?"};

static const int N = (int)(sizeof(ICONS) / sizeof(ICONS[0]));

// Mesmo fundo da tela de previsao (weatherBackground do firmware). O fundo
// pulsa bastante, entao os icones sao conferidos em mais de uma fase.
static uint16_t weatherBackground(uint32_t ms) {
  const float t = (ms % 12000) / 12000.0f * 2.0f * 3.14159265f;
  const int r = (int)(2.0f + 2.0f * sinf(t));
  const int g = (int)(1.5f + 1.2f * sinf(t + 2.09f));
  const int b = (int)(3.0f + 1.0f * sinf(t + 4.19f));
  return (uint16_t)(((r * 31 / 4) << 11) | ((g * 63 / 3) << 5) | (b * 31 / 4));
}

// O binario pode ser chamado da raiz do repo ou de sim/, entao tenta os dois
// diretorios de saida antes de desistir.
static void savePng(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, W, H);
  if (!png) {
    fprintf(stderr, "createPng falhou para %s\n", name);
    return;
  }
  const char *dirs[] = {"sim/build/", "build/", ""};
  for (const char *d : dirs) {
    char path[256];
    snprintf(path, sizeof(path), "%s%s", d, name);
    FILE *f = fopen(path, "wb");
    if (f) {
      fwrite(png, 1, len, f);
      fclose(f);
      printf("gravado %s\n", path);
      break;
    }
  }
  free(png);
}

// Grade de `cols` x `rows` celulas com rotulo sob cada icone.
static void grid(uint32_t ms, int size, int cols, int rows, int first, const char *name) {
  const uint16_t bg = weatherBackground(ms);
  rca.fillScreen(bg);
  rca.setFont(&fonts::Font0);
  rca.setTextSize(1);
  rca.setTextDatum(top_center);
  rca.setTextColor(TFT_WHITE, bg);

  const int cw = W / cols, ch = H / rows;
  for (int i = 0; i < cols * rows && first + i < N; ++i) {
    const int cx = (i % cols) * cw + cw / 2;
    const int cy = (i / cols) * ch + ch / 2;
    wx::drawWeatherIcon(&rca, cx, cy - 6, size, ICONS[first + i]);
    rca.drawString(SHORT[first + i], cx, cy + ch / 2 - 10);
  }
  savePng(name);
}

int main(int, char **) {
  panel.setScaling(3, 3);
  rca.setPanel(&panel);
  if (!rca.init())
    return 1;
  rca.setColorDepth(8); // RGB332, igual ao firmware na saida composta

  // 48 px: tamanho de lista/rodape. Tres fases do fundo para conferir se o
  // contorno preto some quando o azul escurece.
  grid(0, 48, 5, 3, 0, "wx48_a.png");
  grid(3000, 48, 5, 3, 0, "wx48_b.png");
  grid(7000, 48, 5, 3, 0, "wx48_c.png");

  // 96 px: tamanho do icone principal da previsao.
  grid(0, 96, 3, 2, 0, "wx96_1.png");
  grid(0, 96, 3, 2, 6, "wx96_2.png");
  grid(0, 96, 3, 2, 12, "wx96_3.png");
  return 0;
}
