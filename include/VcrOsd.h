#pragma once
// ============================================================================
// OSD do player no estilo dos videocassetes Sony/Semp dos anos 90.
//
// O OSD de um VCR daquela epoca nao tinha caixa nem faixa de fundo: os glifos
// flutuavam direto sobre a imagem e ganhavam legibilidade por um contorno preto
// de 1 px ao redor de cada traco. E esse contorno, e nao uma tarja, que faz o
// texto sobreviver tanto a uma cena clara quanto a uma escura — por isso aqui
// nada e pintado de fundo e todo glifo sai com contorno, por drawText().
//
// Vocabulario visual reproduzido:
//   - transporte desenhado como forma (triangulo, duas barras, quadrado...),
//     nunca como palavra;
//   - contador de fita em digitos grandes, no formato 0:00:00;
//   - rotulo de velocidade de gravacao (SP/LP/EP) colado no modo;
//   - branco puro como cor principal, ciano so no acento (legendas dos botoes).
//
// Tipografia: todo texto sai pela VcrFont (include/VcrFont.h), a fonte bitmap
// 12x16 de traco constante de 2 px desenhada para este OSD. O contorno preto
// vem da dilatacao da propria fonte, num passe so por glifo. As fontes do
// M5GFX (Font0/Font2) foram abandonadas aqui: traco de 1 px nao aguenta
// contorno sem entupir os vazios de 8, 9, O e R.
//
// Texto de entrada pode vir com acentos (titulo de meta.json, por exemplo). A
// fonte so tem ASCII, e em UTF-8 cada letra acentuada sao dois bytes que
// virariam dois espacos. Por isso tudo passa por ascii::normalizeUpper antes
// de chegar a fonte: "Sao Joao" sai "SAO JOAO", nunca "S O JO O".
//
// Dependencias: M5GFX (incluido pela VcrFont), SafeArea.h, Ascii.h e
// VcrFont.h. Nada de Arduino, WiFi, SD ou FreeRTOS: o mesmo header compila no
// firmware e no simulador SDL de desktop.
//
// Custo: o contador (escala 2) e as linhas pequenas (escala 1) custam ~2,2x
// os pixels do glifo cada, com contorno — ver a nota da VcrFont. O simbolo de
// transporte sai 5 vezes (4 deslocamentos em preto + 1 em branco), mas e uma
// forma so. O OSD completo fica em poucos milhares de escritas de pixel por
// repintura, contra 76800 de um quadro inteiro; a 4 Hz some no orcamento.
// ============================================================================

#include "Ascii.h"
#include "SafeArea.h"
#include "VcrFont.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace vcr {

// ---------------------------------------------------------------------------
//  Estado exibido
// ---------------------------------------------------------------------------

// Modos de transporte com simbolo proprio no painel de um VCR.
enum class Transport : uint8_t { Stop, Play, Pause, Rew, Ff, Rec };

// Velocidade de gravacao da fita. None esconde o rotulo.
enum class Speed : uint8_t { None, SP, LP, EP };

struct State {
  Transport transport = Transport::Play;
  Speed speed = Speed::SP;
  uint32_t seconds = 0;             // contador de fita, em segundos
  const char *title = nullptr;      // titulo do programa (ASCII, sem acentos)
  const char *buttonLeft = nullptr; // legendas dos tres botoes do Core2
  const char *buttonCenter = nullptr;
  const char *buttonRight = nullptr;
  bool showCounter = true; // desliga o contador + velocidade
  bool showTitle = true;   // desliga a linha do titulo
  // Fase de pisca-pisca do simbolo de transporte. O VCR piscava o simbolo de
  // PAUSE; quem chama controla o ritmo passando false alternadamente, para que
  // este header continue sem estado nem relogio proprio.
  bool blinkOn = true;
};

// ---------------------------------------------------------------------------
//  Geometria — tudo ancorado na base da area segura, crescendo para cima
// ---------------------------------------------------------------------------
namespace layout {

constexpr int kOutline = 1; // espessura do contorno preto, em pixels
constexpr int kGap = 4;     // respiro entre as linhas (ja fora dos contornos)

// Linhas pequenas em VcrFont escala 1 (12x16); contador em escala 2 (24x32),
// o "digito grande" do painel. O serrilhado da escala inteira e o que lembra o
// gerador de caracteres do VCR.
constexpr int kSmallScale = 1;
constexpr int kBigScale = 2;
constexpr int kButtonH = vcrfont::CELL_H * kSmallScale; // 16
constexpr int kTitleH = vcrfont::CELL_H * kSmallScale;  // 16
constexpr int kModeH = vcrfont::CELL_H * kBigScale;     // 32
constexpr int kIconBox = 20; // lado da caixa do simbolo de transporte

// A ultima linha pintavel e SAFE_B - 1; o contorno ainda precisa caber nela.
constexpr int kButtonY = crt::SAFE_B - kOutline - kButtonH; // 205
constexpr int kTitleY = kButtonY - kGap - kTitleH;          // 185
constexpr int kModeY = kTitleY - kGap - kModeH;             // 149

constexpr int kLeftX = crt::SAFE_L + kOutline;            // 25
constexpr int kRightX = crt::SAFE_R - 1 - kOutline;       // 294
constexpr int kCenterX = (crt::SAFE_L + crt::SAFE_R) / 2; // 160
constexpr int kMaxTextW = kRightX - kLeftX + 1;           // 270

// Primeira e ultima linha que draw() pode tocar (contorno incluido).
constexpr int kTopY = kModeY - kOutline;      // 148
constexpr int kBottomY = crt::SAFE_B - 1;     // 221
constexpr int kHeight = kBottomY - kTopY + 1; // 74

static_assert(kTopY >= crt::SAFE_T, "OSD escapa pelo topo da area segura");
static_assert(kBottomY < crt::SAFE_B, "OSD escapa pela base da area segura");
static_assert(kLeftX >= crt::SAFE_L, "OSD escapa pela esquerda da area segura");
static_assert(kRightX < crt::SAFE_R, "OSD escapa pela direita da area segura");

} // namespace layout

// Cores em RGB565, escritas na mao para o header nao depender das macros TFT_*.
constexpr uint16_t kInk = 0xFFFF;    // branco puro: cor principal
constexpr uint16_t kShadow = 0x0000; // preto: contorno
constexpr uint16_t kAccent = 0x07FF; // ciano: unico acento permitido

// ---------------------------------------------------------------------------
//  Primitivas com contorno
// ---------------------------------------------------------------------------

// Os 4 deslocamentos da sombra. Ver a nota de custo no topo do arquivo.
inline const int8_t *outlineOffsets() {
  static const int8_t kOff[8] = {-1, 0, 1, 0, 0, -1, 0, 1};
  return kOff;
}

// Texto flutuante: normaliza para ASCII maiusculo e pinta com contorno preto
// pela VcrFont. Alinhamento horizontal: 0 = esquerda em x, 1 = centro em x,
// 2 = direita em x (x e a ultima coluna). Devolve a largura desenhada.
inline int drawText(lgfx::LovyanGFX *gfx, const char *text, int x, int y, uint16_t color, int scale,
                    int align = 0) {
  if (!text || !*text)
    return 0;
  char ascii[128];
  ::ascii::normalizeUpper(ascii, sizeof(ascii), text);
  const int w = vcrfont::textWidth(ascii, scale);
  if (align == 1)
    x -= w / 2;
  else if (align == 2)
    x = x + 1 - w;
  return vcrfont::drawText(gfx, ascii, x, y, (int32_t)color, scale, (int32_t)kShadow);
}

// Simbolo de transporte dentro da caixa (x, y, box). Desenhado como forma pura:
// o firmware antigo escrevia "PLAY"/"PAUSE", mas nenhum VCR mostrava palavras.
inline void drawTransportShape(lgfx::LovyanGFX *gfx, Transport mode, int x, int y, int box, uint16_t color) {
  const int b = box - 1;
  switch (mode) {
  case Transport::Play:
    gfx->fillTriangle(x, y, x, y + b, x + b, y + b / 2, color);
    break;
  case Transport::Pause: {
    const int bar = box / 3;
    gfx->fillRect(x, y, bar, box, color);
    gfx->fillRect(x + box - bar, y, bar, box, color);
    break;
  }
  case Transport::Stop:
    gfx->fillRect(x, y, box, box, color);
    break;
  case Transport::Ff: {
    const int half = box / 2;
    gfx->fillTriangle(x, y, x, y + b, x + half - 1, y + b / 2, color);
    gfx->fillTriangle(x + half, y, x + half, y + b, x + b, y + b / 2, color);
    break;
  }
  case Transport::Rew: {
    const int half = box / 2;
    gfx->fillTriangle(x + half - 1, y, x + half - 1, y + b, x, y + b / 2, color);
    gfx->fillTriangle(x + b, y, x + b, y + b, x + half, y + b / 2, color);
    break;
  }
  case Transport::Rec:
    gfx->fillCircle(x + b / 2, y + b / 2, b / 2, color);
    break;
  }
}

// Mesmo contorno do texto, aplicado a forma: 4 copias pretas deslocadas embaixo.
inline void drawTransportIcon(lgfx::LovyanGFX *gfx, Transport mode, int x, int y, int box,
                              int radius = layout::kOutline) {
  const int8_t *off = outlineOffsets();
  for (int i = 0; i < 4; ++i)
    drawTransportShape(gfx, mode, x + off[i * 2] * radius, y + off[i * 2 + 1] * radius, box, kShadow);
  drawTransportShape(gfx, mode, x, y, box, kInk);
}

// ---------------------------------------------------------------------------
//  Formatacao
// ---------------------------------------------------------------------------

// Contador de fita no formato do painel: horas sem zero a esquerda, "0:00:00".
inline void formatCounter(uint32_t seconds, char *out, size_t outN) {
  uint32_t h = seconds / 3600U;
  const uint32_t m = (seconds / 60U) % 60U, s = seconds % 60U;
  // Único texto do OSD que não passa por fitText: alinhado à direita, uma string
  // larga demais empurraria o x para fora da área segura pela esquerda. Duas
  // casas de hora é o que o contador de um videocassete mostrava de qualquer
  // forma; acima disso o dado já veio de um WAV com taxa corrompida.
  if (h > 99U)
    h = 99U;
  snprintf(out, outN, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

// Encolhe o texto ate caber em maxW, com reticencias. A fonte e monoespacada,
// entao cabem exatamente maxW / (12 * escala) caracteres — sem medir nada.
// Normaliza antes de contar, para um acento nao valer dois caracteres.
inline void fitText(const char *src, char *out, size_t outN, int maxW, int scale) {
  if (!out || outN < 4)
    return;
  out[0] = '\0';
  if (!src || !*src || maxW <= 0 || scale <= 0)
    return; // scale zero seria divisão por zero adiante (exceção de CPU no xtensa)
  const size_t n = ::ascii::normalizeUpper(out, outN, src);
  const size_t maxChars = size_t(maxW / (vcrfont::CELL_W * scale));
  if (n <= maxChars)
    return;
  if (maxChars < 4) {
    out[maxChars] = '\0';
    return;
  }
  memcpy(out + maxChars - 3, "...", 4);
}

inline const char *speedLabel(Speed speed) {
  switch (speed) {
  case Speed::SP:
    return "SP";
  case Speed::LP:
    return "LP";
  case Speed::EP:
    return "EP";
  default:
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
//  Desenho
// ---------------------------------------------------------------------------

// Apaga a faixa que draw() ocupa. Sem fundo proprio o OSD nao se sobrescreve,
// entao quem chama limpa antes de repintar (ou deixa o proximo quadro de video
// cobrir a area, que e o caminho barato com o filme rodando). A limpeza vai ate
// a ultima linha do raster, e nao ate SAFE_B: o que passa dali fica no overscan
// e o tubo esconde, mas um monitor mais generoso mostraria a sujeira.
inline void clear(lgfx::LovyanGFX *gfx, uint16_t background = kShadow) {
  gfx->fillRect(0, layout::kTopY, crt::W, crt::H - layout::kTopY, background);
}

// Pinta o OSD sobre o que ja estiver na tela. Nao limpa nada e nao pinta fundo.
inline void draw(lgfx::LovyanGFX *gfx, const State &state) {
  using namespace layout;
  char buffer[128];

  // Linha do modo: [simbolo] SP ............................ 0:12:34
  if (state.blinkOn)
    drawTransportIcon(gfx, state.transport, kLeftX, kModeY + (kModeH - kIconBox) / 2, kIconBox);

  // Velocidade centrada na altura do simbolo, como no painel do VCR. Nao
  // depende de showCounter: pertence ao modo; para esconde-la, Speed::None.
  if (const char *speed = speedLabel(state.speed))
    drawText(gfx, speed, kLeftX + kIconBox + 6, kModeY + (kModeH - kTitleH) / 2, kInk, kSmallScale);

  if (state.showCounter) {
    formatCounter(state.seconds, buffer, sizeof(buffer));
    drawText(gfx, buffer, kRightX, kModeY, kInk, kBigScale, 2);
  }

  // Titulo do programa, truncado para nunca vazar da area segura.
  if (state.showTitle && state.title && *state.title) {
    fitText(state.title, buffer, sizeof(buffer), kMaxTextW, kSmallScale);
    drawText(gfx, buffer, kLeftX, kTitleY, kInk, kSmallScale);
  }

  // Legendas dos tres botoes — unico elemento em ciano. Sem colchetes: cada
  // rotulo fica ancorado no terco do botao fisico do Core2. O do centro e
  // medido primeiro, e os laterais ganham o que sobra de cada lado.
  if (state.buttonLeft || state.buttonCenter || state.buttonRight) {
    char center[32] = "";
    if (state.buttonCenter)
      fitText(state.buttonCenter, center, sizeof(center), kMaxTextW / 3, kSmallScale);
    const int centerW = vcrfont::textWidth(center, kSmallScale);
    const int side = (kMaxTextW - centerW) / 2 - vcrfont::CELL_W / 2;
    if (state.buttonLeft) {
      fitText(state.buttonLeft, buffer, sizeof(buffer), side, kSmallScale);
      drawText(gfx, buffer, kLeftX, kButtonY, kAccent, kSmallScale, 0);
    }
    if (*center)
      drawText(gfx, center, kCenterX, kButtonY, kAccent, kSmallScale, 1);
    if (state.buttonRight) {
      fitText(state.buttonRight, buffer, sizeof(buffer), side, kSmallScale);
      drawText(gfx, buffer, kRightX, kButtonY, kAccent, kSmallScale, 2);
    }
  }
}

} // namespace vcr
