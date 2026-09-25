#pragma once
// Abertura do aparelho: o "prefixo da emissora" que aparece enquanto o boot
// acorda o cartão, o rádio e o DAC.
//
// Por que existe: o Fruit Jam sai de fábrica com o Fruit Jam OS (CircuitPython),
// cuja tela de boot é personalizável. Gravar este firmware substitui aquele
// sistema inteiro, então a abertura passa a ser nossa — e leva o nome do dono,
// no estilo das vinhetas de identificação de canal dos anos 80.
//
// Composição, de cima para baixo, toda dentro da área segura (SafeArea.h):
//   produto   "FRUIT JAM RETRO TV", fonte VCR 1x, ciano
//   nome      o dono, fonte VCR no maior tamanho que caiba (até 4x), com contorno
//   faixas    cinco listras do arco-íris retrô, sangrando só até a caixa segura
//   status    a etapa do boot, trocada a cada passo sem repintar o resto
//   versão    canto inferior direito (só o número: "v0.1")
//
// Todo texto é cortado em MAX_CHARS letras: nada sai da caixa segura.
//
// Só LovyanGFX + VcrFont: compila no firmware e no simulador (probe
// sim/probes/boot_splash.cpp), que é onde a diagramação é conferida.
#include "Ascii.h"
#include "SafeArea.h"
#include "VcrFont.h"

namespace bootsplash {

// Geometria. As faixas e o status ficam abaixo do nome; o status tem banda
// própria para ser repintado sozinho a cada etapa do boot.
constexpr int PRODUCT_Y = crt::SAFE_T + 14;  // 32
constexpr int NAME_MID_Y = 104;              // centro vertical do nome
constexpr int STRIPES_Y = 150;
constexpr int STRIPE_H = 4;
constexpr int STRIPE_COUNT = 5;
constexpr int STATUS_Y = 176;
constexpr int STATUS_H = vcrfont::CELL_H;
constexpr int VERSION_Y = crt::SAFE_B - vcrfont::CELL_H; // 206
constexpr int MAX_SCALE = 4;

// Cores RGB565 como int32_t (convenção da VcrFont; AGENTS.md, armadilha 11).
constexpr int32_t INK = 0xFFFF;
constexpr int32_t OUTLINE = 0x0000;
constexpr int32_t PRODUCT = 0x5EFF; // ciano do Weather Channel
constexpr int32_t STATUS = 0xBDF7;  // cinza claro
constexpr int32_t VERSION_INK = 0x7BEF;
constexpr uint16_t STRIPES[STRIPE_COUNT] = {0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x041F};

// Letras que cabem na largura segura na fonte VCR 1x (272 / 12 = 22). Texto
// maior é cortado aqui: a fonte não quebra linha, e vazar para o overscan
// esconderia o fim do nome no tubo.
constexpr int MAX_CHARS = crt::SAFE_W / vcrfont::CELL_W;

// Normaliza para as maiúsculas da fonte VCR e corta no que cabe.
inline void fitText(char (&buf)[40], const char *src) {
  ascii::normalizeUpper(buf, sizeof(buf), src ? src : "");
  if (MAX_CHARS < (int)sizeof(buf))
    buf[MAX_CHARS] = '\0';
}

// Fundo: degradê do azul quase preto ao cobalto, como o da previsão do tempo.
inline uint16_t backgroundRow(int y) {
  const int b = 2 + (y * 22) / crt::H; // 2..23 de 31 no azul
  const int g = (y * 6) / crt::H;      // um fio de verde para não ficar roxo
  return (uint16_t)((g << 5) | b);
}

inline void paintBackground(GfxTarget *g, int y0, int h) {
  for (int y = y0; y < y0 + h; ++y)
    g->drawFastHLine(0, y, crt::W, backgroundRow(y));
}

// Maior escala da fonte VCR em que `text` cabe na largura segura.
inline int fitScale(const char *text) {
  const int w1 = vcrfont::textWidth(text, 1);
  if (w1 <= 0)
    return 1;
  int s = crt::SAFE_W / w1;
  if (s > MAX_SCALE)
    s = MAX_SCALE;
  return s < 1 ? 1 : s;
}

inline void centered(GfxTarget *g, const char *text, int y, int32_t color, int scale,
                     int32_t outline = vcrfont::NO_OUTLINE) {
  const int x = (crt::W - vcrfont::textWidth(text, scale)) / 2;
  vcrfont::drawText(g, text, x, y, color, scale, outline);
}

// Só a linha de status: chamada a cada etapa do boot, sem piscar o resto.
inline void drawStatus(GfxTarget *g, const char *status) {
  paintBackground(g, STATUS_Y, STATUS_H);
  char buf[40];
  fitText(buf, status);
  centered(g, buf, STATUS_Y, STATUS, 1);
}

// Tela inteira. `owner` é o nome do dono (texto livre: passa por
// normalizeUpper, porque a fonte VCR só tem maiúsculas, dígitos e pontuação).
inline void draw(GfxTarget *g, const char *owner, const char *product, const char *status,
                 const char *version) {
  paintBackground(g, 0, crt::H);

  char buf[40];
  fitText(buf, product);
  centered(g, buf, PRODUCT_Y, PRODUCT, 1);

  fitText(buf, owner);
  const int scale = fitScale(buf);
  centered(g, buf, NAME_MID_Y - (vcrfont::CELL_H * scale) / 2, INK, scale, OUTLINE);

  for (int i = 0; i < STRIPE_COUNT; ++i)
    g->fillRect(crt::SAFE_L, STRIPES_Y + i * STRIPE_H, crt::SAFE_W, STRIPE_H, STRIPES[i]);

  drawStatus(g, status);

  fitText(buf, version);
  vcrfont::drawText(g, buf, crt::SAFE_R - vcrfont::textWidth(buf, 1), VERSION_Y, VERSION_INK, 1);
}

} // namespace bootsplash
