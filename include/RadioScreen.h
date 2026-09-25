#pragma once
// ============================================================================
// RadioScreen — a tela do rádio pela internet do M5 RETRO TV.
//
// A especificação é curta e é a regra que decide todo o resto deste arquivo:
// **o logotipo da estação, e nada além de um relógio em hora local.** Não é uma
// tela de player. Não tem barra de progresso, não tem espectro, não tem capa,
// não tem menu. É um relógio de parede com o logotipo da rádio em cima, que por
// acaso está tocando som. Tudo que for acrescentado aqui tira dela exatamente o
// que a torna boa de deixar ligada.
//
// As duas únicas concessões são uma linha de estado (CONECTANDO / NO AR / SEM
// SINAL) e, quando o servidor manda, o nome da música num letreiro lento. Ambas
// em corpo pequeno, cinza, no rodapé — presentes para quem procurar, invisíveis
// para quem não.
//
// ---------------------------------------------------------------------------
//  Repintura parcial: por que isto não é otimização prematura
// ---------------------------------------------------------------------------
//
// A saída composta roda a ~23 quadros/s e o framebuffer do CVBS vive na SRAM.
// Repintar 320x240 uma vez por segundo, só para mudar dois dígitos, gastaria
// ciclos do core 1 e — pior — piscaria a tela inteira no tubo uma vez por
// segundo. É o mesmo motivo pelo qual o relógio do menu inicial do src/main.cpp
// redesenha só a própria faixa.
//
// Então o desenho é dividido em regiões e `tick()` repinta SÓ o que mudou:
//
//   R_LOGO    nunca, depois da primeira pintura
//   R_SEC     uma vez por segundo  (72x32 px)
//   R_HOUR    uma vez por minuto   (180x48 px)
//   R_DATE    uma vez por dia
//   R_STATUS  quando o estado da conexão muda
//   R_TICKER  um passo de caractere a cada 500 ms, e só se o título não couber
//
// `tick()` devolve a máscara das regiões repintadas e `regionRect()` dá o
// retângulo de cada uma, para quem integra saber o que sujou (útil para as
// transições do ScreenFx.h e para o espelhamento no LCD).
//
// ---------------------------------------------------------------------------
//  Restrições do tubo que este arquivo respeita
// ---------------------------------------------------------------------------
//
// * Tudo dentro de crt::SAFE_* (SafeArea.h): um CRT come ~7% de cada borda.
//   Só o fundo sangra até o limite do raster.
// * Nada de ciano saturado (TFT_CYAN / 0x07FF): na composta ele produz dot
//   crawl visível. O acento seguro do projeto é 0x96BC.
// * Texto ASCII sem acento: as fontes bitmap só têm 0x20..0x7E. O título vindo
//   do servidor já chega normalizado pelo RadioStream.h, mas esta tela
//   normaliza de novo antes de desenhar — sai mais barato do que confiar.
// * Nada de LGFX_Sprite: não há SRAM para isso (uma alocação de 5 KB já
//   derrubou o aparelho). Todo desenho vai direto no destino.
//
// A entrada de desenho é (LovyanGFX *dst, int ox, int oy), como os pintores da
// previsão do tempo: o mesmo código serve ao painel CVBS, ao LCD do Core2 e ao
// painel SDL do simulador, e o deslocamento permite usar as transições.
//
// Compila em -std=gnu++11. Todo corpo de constexpr aqui é um único return.
// ============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Ascii.h"
#include "SafeArea.h"
#include "UiLogic.h" // timeReached()
#include "VcrFont.h" // traz a LovyanGFX (fj/Gfx.h) junto

// ---------------------------------------------------------------------------
//  Logotipo da estação (header GERADO, fora deste arquivo)
// ---------------------------------------------------------------------------
//
// O bitmap do logotipo NÃO é criado aqui: ele é gerado a partir da arte da
// própria estação e entregue em include/DiarioLogo.h. Quando esse header não
// existe, a tela cai no nome da estação em texto ASCII e continua correta.
//
// Contrato esperado de include/DiarioLogo.h (é só isto; nada mais é lido):
//
//   #pragma once
//   #include <stdint.h>
//   namespace diariologo {
//   constexpr int      WIDTH       = 176;     // <= radioui::LOGO_MAX_W (272)
//   constexpr int      HEIGHT      =  56;     // <= radioui::LOGO_MAX_H  (64)
//   constexpr bool     HAS_TRANSPARENT = true;
//   constexpr uint16_t TRANSPARENT  = 0x0000; // cor tratada como buraco
//   const uint16_t     PIXELS[WIDTH * HEIGHT] = { ... };
//   }
//
//   * PIXELS é RGB565 na ordem NATIVA do uint16_t (o mesmo formato que o
//     src/main.cpp já empurra com pushImage e lgfx::rgb565_t), linha a linha,
//     começando pelo canto superior esquerdo, sem cabeçalho e sem padding.
//   * HAS_TRANSPARENT = false para um logotipo retangular opaco; nesse caso
//     TRANSPARENT é ignorado (mas precisa existir, para o header compilar).
//   * O gerador deve EVITAR ciano saturado (0x07FF e vizinhos) na arte: na
//     saída composta essa cor glitcha. Se a arte original tiver, puxe o
//     vermelho para cima como o RCA_ACCENT do projeto faz.
//   * WIDTH/HEIGHT são constexpr int porque o tamanho do vetor depende deles.
//
// Nada além de WIDTH, HEIGHT, HAS_TRANSPARENT, TRANSPARENT e PIXELS é usado.
#if defined(__has_include)
#if __has_include("DiarioLogo.h")
#include "DiarioLogo.h"
#define RADIOUI_HAVE_LOGO 1
#endif
#endif

namespace radioui {

// ---------------------------------------------------------------------------
//  Cores
// ---------------------------------------------------------------------------

constexpr uint16_t BG = 0x000F;     // TFT_NAVY, o fundo das outras telas
constexpr uint16_t INK = 0xFFFF;    // branco: croma zero, nunca glitcha
constexpr uint16_t ACCENT = 0x96BC; // azul-claro seguro para NTSC (ver main.cpp)
constexpr uint16_t DIM = 0x8410;    // cinza médio, também sem croma
constexpr uint16_t SHADOW = 0x0000; // contorno dos glifos grandes

// ---------------------------------------------------------------------------
//  Geometria
// ---------------------------------------------------------------------------
//
// Tudo derivado de crt::SAFE_* — duas constantes descrevendo a mesma geometria
// é a armadilha 8 do AGENTS.md.
//
//   logotipo  [20, 84)   faixa de 272x64, centralizado
//   HH:MM     [96,144)   VcrFont escala 3 (36x48 por célula), 5 células = 180
//   :SS       [112,144)  VcrFont escala 2 (24x32), 3 células = 72, base alinhada
//   data      [152,168)  escala 1
//   estado    [182,198)  escala 1
//   letreiro  [202,218)  escala 1
//
// O relógio não é HH:MM:SS do mesmo tamanho porque 8 células na escala 3 dariam
// 288 px e a largura segura tem 272. Em vez de encolher tudo para a escala 2, os
// segundos descem um degrau: o dado que se lê de longe (a hora) fica o maior
// possível e a região que repinta a cada segundo fica a MENOR possível.

struct Rect {
  int16_t x, y, w, h;
};

constexpr int LOGO_MAX_W = crt::SAFE_W; // 272
constexpr int LOGO_MAX_H = 64;

constexpr int HOUR_SCALE = 3;
constexpr int SEC_SCALE = 2;
constexpr int SMALL_SCALE = 1;

constexpr int HOUR_W = 5 * vcrfont::CELL_W * HOUR_SCALE; // "HH:MM" = 180
constexpr int SEC_W = 3 * vcrfont::CELL_W * SEC_SCALE;   // ":SS"   =  72
constexpr int CLOCK_GAP = 8;
constexpr int CLOCK_W = HOUR_W + CLOCK_GAP + SEC_W; // 260
constexpr int CLOCK_X = crt::SAFE_L + (crt::SAFE_W - CLOCK_W) / 2; // 30
constexpr int CLOCK_Y = 96;
constexpr int HOUR_H = vcrfont::CELL_H * HOUR_SCALE; // 48
constexpr int SEC_H = vcrfont::CELL_H * SEC_SCALE;   // 32

constexpr Rect LOGO_RECT = {(int16_t)crt::SAFE_L, (int16_t)(crt::SAFE_T + 2), (int16_t)LOGO_MAX_W,
                            (int16_t)LOGO_MAX_H};
constexpr Rect HOUR_RECT = {(int16_t)CLOCK_X, (int16_t)CLOCK_Y, (int16_t)HOUR_W, (int16_t)HOUR_H};
// Base alinhada com a da hora: CLOCK_Y + HOUR_H - SEC_H.
constexpr Rect SEC_RECT = {(int16_t)(CLOCK_X + HOUR_W + CLOCK_GAP), (int16_t)(CLOCK_Y + HOUR_H - SEC_H),
                           (int16_t)SEC_W, (int16_t)SEC_H};
constexpr Rect DATE_RECT = {(int16_t)crt::SAFE_L, 152, (int16_t)crt::SAFE_W, 16};
constexpr Rect STATUS_RECT = {(int16_t)crt::SAFE_L, 182, (int16_t)crt::SAFE_W, 16};
constexpr Rect TICKER_RECT = {(int16_t)crt::SAFE_L, 202, (int16_t)crt::SAFE_W, 16};

// Quantos caracteres cabem no letreiro (escala 1 = 12 px por célula).
constexpr int TICKER_COLS = crt::SAFE_W / vcrfont::CELL_W; // 22

// Um passo de caractere a cada 500 ms. Lento de propósito: o ponto da tela é o
// relógio, e um letreiro rápido rouba o olho.
constexpr uint32_t TICKER_STEP_MS = 500;

// ---------------------------------------------------------------------------
//  Regiões
// ---------------------------------------------------------------------------

enum Region : uint8_t {
  R_NONE = 0,
  R_LOGO = 1 << 0,
  R_HOUR = 1 << 1,
  R_SEC = 1 << 2,
  R_DATE = 1 << 3,
  R_STATUS = 1 << 4,
  R_TICKER = 1 << 5,
  R_ALL = 0x3F
};

inline Rect regionRect(uint8_t bit) {
  return bit == R_LOGO     ? LOGO_RECT
         : bit == R_HOUR   ? HOUR_RECT
         : bit == R_SEC    ? SEC_RECT
         : bit == R_DATE   ? DATE_RECT
         : bit == R_STATUS ? STATUS_RECT
         : bit == R_TICKER ? TICKER_RECT
                           : Rect{0, 0, 0, 0};
}

// ---------------------------------------------------------------------------
//  Estado exibido
// ---------------------------------------------------------------------------
//
// Struct burra de propósito: esta tela não sabe o que é Wi-Fi, socket ou NTP.
// Quem integra preenche e chama. É o que permite ao probe desenhar qualquer
// combinação sem simular nada.
struct State {
  uint8_t hour = 0, minute = 0, second = 0; // hora LOCAL já resolvida
  uint8_t day = 1, month = 1;
  uint16_t year = 2026;
  uint8_t weekday = 0;       // 0 = domingo ... 6 = sábado
  bool timeValid = false;    // false -> desenha --:-- / --
  const char *stationName = "";  // usado só quando não há DiarioLogo.h
  const char *statusText = "";    // radio::connLabel(): "NO AR", "CONECTANDO"...
  const char *title = "";         // StreamTitle; "" quando não houver
};

// Abreviações pt-BR em ASCII (AGENTS.md 2.7: as fontes não têm acento).
inline const char *weekdayName(uint8_t w) {
  static const char *const N[7] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
  return N[w < 7 ? w : 0];
}
inline const char *monthName(uint8_t m) {
  static const char *const N[12] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN",
                                    "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};
  return N[(m >= 1 && m <= 12) ? m - 1 : 0];
}

// ---------------------------------------------------------------------------
//  Cache da repintura parcial
// ---------------------------------------------------------------------------
//
// Mora no CHAMADOR, não em `static` de função. A armadilha 6 do AGENTS.md é
// exatamente estado estático herdado da visita anterior a uma tela — aqui basta
// zerar o Cache ao entrar (cacheReset) e o problema não existe.
struct Cache {
  bool painted = false;
  uint8_t hour = 255, minute = 255, second = 255;
  uint8_t day = 255, month = 255;
  uint16_t year = 0;
  bool timeValid = false;
  char status[16] = {0};
  uint32_t titleHash = 0;
  int16_t titleLen = 0;
  int16_t tickerPos = 0;
  uint32_t tickerAt = 0;
};

inline void cacheReset(Cache &c) {
  c = Cache();
}

namespace detail {

// FNV-1a de 32 bits. Só serve para detectar "o título mudou"; colisão aqui
// custa um letreiro desatualizado até a próxima música, não um bug.
inline uint32_t hashStr(const char *s) {
  uint32_t h = 2166136261u;
  for (; s && *s; ++s)
    h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

inline void clearRect(LovyanGFX *dst, int ox, int oy, const Rect &r) {
  dst->fillRect(ox + r.x, oy + r.y, r.w, r.h, BG);
}

// Duas casas, ou "--" quando o relógio ainda não foi sincronizado.
inline void two(char *dst, int v, bool valid) {
  if (valid)
    snprintf(dst, 3, "%02d", v % 100);
  else {
    dst[0] = '-';
    dst[1] = '-';
    dst[2] = 0;
  }
}

} // namespace detail

// ---------------------------------------------------------------------------
//  Pintores
// ---------------------------------------------------------------------------

// O fundo sangra até a borda do raster: sem tarja preta em volta, senão a
// imagem parece pequena no tubo (AGENTS.md 2.5).
inline void drawBackground(LovyanGFX *dst, int ox, int oy) {
  if (!dst)
    return;
  dst->fillRect(ox, oy, crt::W, crt::H, BG);
}

// Logotipo, ou o nome da estação em texto quando não há o header gerado.
inline void drawLogo(LovyanGFX *dst, int ox, int oy, const State &s) {
  if (!dst)
    return;
  detail::clearRect(dst, ox, oy, LOGO_RECT);
#ifdef RADIOUI_HAVE_LOGO
  const int w = diariologo::WIDTH <= LOGO_MAX_W ? diariologo::WIDTH : LOGO_MAX_W;
  const int h = diariologo::HEIGHT <= LOGO_MAX_H ? diariologo::HEIGHT : LOGO_MAX_H;
  const int x = ox + LOGO_RECT.x + (LOGO_RECT.w - w) / 2;
  const int y = oy + LOGO_RECT.y + (LOGO_RECT.h - h) / 2;
  const lgfx::rgb565_t *px = reinterpret_cast<const lgfx::rgb565_t *>(diariologo::PIXELS);
  if (diariologo::HAS_TRANSPARENT)
    dst->pushImage(x, y, w, h, px, lgfx::rgb565_t(diariologo::TRANSPARENT));
  else
    dst->pushImage(x, y, w, h, px);
#else
  // Sem logotipo: o nome da estação em corpo grosso, centralizado na faixa.
  // Normaliza mesmo vindo de constante — é barato e fecha o caminho.
  char name[24];
  ascii::normalizeUpper(name, sizeof(name), s.stationName && *s.stationName ? s.stationName : "RADIO");
  const int y = LOGO_RECT.y + (LOGO_RECT.h - vcrfont::textHeight(2)) / 2;
  vcrfont::drawTextCentered(dst, name, ox + LOGO_RECT.x, oy + y, LOGO_RECT.w, ACCENT, 2);
#endif
}

// HH:MM. Contorno preto por glifo: no tubo o branco puro sobre azul "sangra" e
// o contorno devolve a borda (é a mesma técnica do OSD do player).
inline void drawHour(LovyanGFX *dst, int ox, int oy, const State &s) {
  if (!dst)
    return;
  detail::clearRect(dst, ox, oy, HOUR_RECT);
  char buf[8];
  detail::two(buf, s.hour, s.timeValid);
  buf[2] = ':';
  detail::two(buf + 3, s.minute, s.timeValid);
  vcrfont::drawText(dst, buf, ox + HOUR_RECT.x, oy + HOUR_RECT.y, INK, HOUR_SCALE, SHADOW);
}

// :SS — a única região que repinta uma vez por segundo, e a menor de todas.
inline void drawSeconds(LovyanGFX *dst, int ox, int oy, const State &s) {
  if (!dst)
    return;
  detail::clearRect(dst, ox, oy, SEC_RECT);
  char buf[8];
  buf[0] = ':';
  detail::two(buf + 1, s.second, s.timeValid);
  vcrfont::drawText(dst, buf, ox + SEC_RECT.x, oy + SEC_RECT.y, ACCENT, SEC_SCALE, SHADOW);
}

inline void drawDate(LovyanGFX *dst, int ox, int oy, const State &s) {
  if (!dst)
    return;
  detail::clearRect(dst, ox, oy, DATE_RECT);
  char buf[24];
  if (s.timeValid)
    snprintf(buf, sizeof(buf), "%s %02u %s %04u", weekdayName(s.weekday), (unsigned)s.day,
             monthName(s.month), (unsigned)s.year);
  else
    snprintf(buf, sizeof(buf), "--- -- --- ----");
  vcrfont::drawTextCentered(dst, buf, ox + DATE_RECT.x, oy + DATE_RECT.y, DATE_RECT.w, DIM, SMALL_SCALE);
}

// Linha de estado. "NO AR" ganha o acento; o resto fica cinza, para que a tela
// no ar não tenha nada competindo com o relógio.
inline void drawStatus(LovyanGFX *dst, int ox, int oy, const State &s) {
  if (!dst)
    return;
  detail::clearRect(dst, ox, oy, STATUS_RECT);
  char buf[20];
  ascii::normalizeUpper(buf, sizeof(buf), s.statusText ? s.statusText : "");
  if (!buf[0])
    return;
  const uint16_t color = strcmp(buf, "NO AR") == 0 ? ACCENT : DIM;
  vcrfont::drawTextCentered(dst, buf, ox + STATUS_RECT.x, oy + STATUS_RECT.y, STATUS_RECT.w, color,
                            SMALL_SCALE);
}

// Janela de TICKER_COLS caracteres do título, começando em `pos`.
//
// Quando o título cabe, `pos` é ignorado e o texto sai centralizado e parado —
// um letreiro que rola sem precisar é só ruído.
inline void tickerWindow(const char *title, int pos, char *dst, size_t cap) {
  char norm[160];
  ascii::normalizeUpper(norm, sizeof(norm), title ? title : "");
  const int n = (int)strlen(norm);
  if (n <= TICKER_COLS) {
    strncpy(dst, norm, cap - 1);
    dst[cap - 1] = 0;
    return;
  }
  // Separador entre o fim e o recomeço, para não emendar palavra com palavra.
  static const char SEP[] = "   ---   ";
  const int sep = (int)sizeof(SEP) - 1;
  const int period = n + sep;
  int p = pos % period;
  if (p < 0)
    p += period;
  size_t j = 0;
  for (int k = 0; k < TICKER_COLS && j + 1 < cap; ++k) {
    const int idx = (p + k) % period;
    dst[j++] = idx < n ? norm[idx] : SEP[idx - n];
  }
  dst[j] = 0;
}

inline void drawTicker(LovyanGFX *dst, int ox, int oy, const State &s, int pos) {
  if (!dst)
    return;
  detail::clearRect(dst, ox, oy, TICKER_RECT);
  if (!s.title || !*s.title)
    return;
  char win[TICKER_COLS + 2];
  tickerWindow(s.title, pos, win, sizeof(win));
  vcrfont::drawTextCentered(dst, win, ox + TICKER_RECT.x, oy + TICKER_RECT.y, TICKER_RECT.w, DIM,
                            SMALL_SCALE);
}

// Repintura completa. É o que as transições do ScreenFx.h chamam e o que
// `tick()` faz na primeira vez.
inline void draw(LovyanGFX *dst, int ox, int oy, const State &s) {
  if (!dst)
    return;
  dst->startWrite();
  drawBackground(dst, ox, oy);
  drawLogo(dst, ox, oy, s);
  drawHour(dst, ox, oy, s);
  drawSeconds(dst, ox, oy, s);
  drawDate(dst, ox, oy, s);
  drawStatus(dst, ox, oy, s);
  drawTicker(dst, ox, oy, s, 0);
  dst->endWrite();
}

// ---------------------------------------------------------------------------
//  Repintura incremental
// ---------------------------------------------------------------------------

// Repinta só o que mudou e devolve a máscara das regiões sujas (0 = nada
// mudou, e nesse caso NADA foi escrito no destino).
//
// `nowMs` é o millis() do chamador e só serve ao passo do letreiro. A
// comparação usa timeReached() porque o millis() dá a volta em ~49 dias — e
// `nowMs > c.tickerAt` cru é justamente o que a AGENTS.md 3.3 proíbe.
inline uint8_t tick(LovyanGFX *dst, int ox, int oy, const State &s, Cache &c, uint32_t nowMs) {
  if (!dst)
    return R_NONE;
  if (!c.painted) {
    draw(dst, ox, oy, s);
    c.painted = true;
    c.hour = s.hour;
    c.minute = s.minute;
    c.second = s.second;
    c.day = s.day;
    c.month = s.month;
    c.year = s.year;
    c.timeValid = s.timeValid;
    ascii::normalizeUpper(c.status, sizeof(c.status), s.statusText ? s.statusText : "");
    c.titleHash = detail::hashStr(s.title);
    c.titleLen = (int16_t)(s.title ? strlen(s.title) : 0);
    c.tickerPos = 0;
    c.tickerAt = nowMs + TICKER_STEP_MS;
    return R_ALL;
  }

  uint8_t dirty = R_NONE;
  const bool validChanged = (c.timeValid != s.timeValid);

  dst->startWrite();

  if (validChanged || c.second != s.second) {
    drawSeconds(dst, ox, oy, s);
    c.second = s.second;
    dirty |= R_SEC;
  }
  if (validChanged || c.minute != s.minute || c.hour != s.hour) {
    drawHour(dst, ox, oy, s);
    c.hour = s.hour;
    c.minute = s.minute;
    dirty |= R_HOUR;
  }
  if (validChanged || c.day != s.day || c.month != s.month || c.year != s.year) {
    drawDate(dst, ox, oy, s);
    c.day = s.day;
    c.month = s.month;
    c.year = s.year;
    dirty |= R_DATE;
  }
  c.timeValid = s.timeValid;

  char status[16];
  ascii::normalizeUpper(status, sizeof(status), s.statusText ? s.statusText : "");
  if (strcmp(status, c.status) != 0) {
    drawStatus(dst, ox, oy, s);
    memcpy(c.status, status, sizeof(status));
    dirty |= R_STATUS;
  }

  const uint32_t h = detail::hashStr(s.title);
  if (h != c.titleHash) {
    // Música nova: o letreiro recomeça do início, senão a primeira volta
    // entraria pelo meio do título.
    c.titleHash = h;
    c.titleLen = (int16_t)(s.title ? strlen(s.title) : 0);
    c.tickerPos = 0;
    c.tickerAt = nowMs + TICKER_STEP_MS;
    drawTicker(dst, ox, oy, s, 0);
    dirty |= R_TICKER;
  } else if (c.titleLen > TICKER_COLS && timeReached(nowMs, c.tickerAt)) {
    ++c.tickerPos;
    c.tickerAt = nowMs + TICKER_STEP_MS;
    drawTicker(dst, ox, oy, s, c.tickerPos);
    dirty |= R_TICKER;
  }

  dst->endWrite();
  return dirty;
}

} // namespace radioui
