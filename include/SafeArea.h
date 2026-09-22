#pragma once
// ============================================================================
// Geometria da saída composta (RCA) do M5 RETRO TV.
//
// O quadro continua sendo 320x240 esticado sobre toda a área ativa do NTSC, que
// a TV mostra em 4:3. O que estas constantes definem é onde o conteúdo pode ser
// desenhado dentro desse quadro: um tubo esconde cerca de 7% de cada borda por
// overscan, então texto, réguas e faixas ficam na "área segura" e só o fundo
// sangra até o limite do raster (sem tarjas pretas).
//
// Compartilhado entre o firmware (src/main.cpp) e o simulador de telas (sim/),
// para que os dois concordem sobre a mesma geometria.
// ============================================================================

namespace crt {

constexpr int W = 320, H = 240;

// Margem de overscan reservada em cada borda.
constexpr int SAFE_X = 24, SAFE_Y = 18;
constexpr int SAFE_L = SAFE_X;      // 24
constexpr int SAFE_T = SAFE_Y;      // 18
constexpr int SAFE_R = W - SAFE_X;  // 296
constexpr int SAFE_B = H - SAFE_Y;  // 222
constexpr int SAFE_W = SAFE_R - SAFE_L; // 272
constexpr int SAFE_H = SAFE_B - SAFE_T; // 204

// Cabeçalho padrão das telas (título, régua e início do corpo).
constexpr int HEAD_Y = SAFE_T;           // 18
constexpr int HEAD_RULE_Y = SAFE_T + 28; // 46
constexpr int BODY_Y = SAFE_T + 36;      // 54

// Barra de legendas dos três botões, encostada na base da área segura.
constexpr int BAR_H = 20;
constexpr int BAR_Y = SAFE_B - BAR_H; // 202

// Faixa do OSD do player.
constexpr int OSD_H = 38;
constexpr int OSD_Y = SAFE_B - OSD_H; // 184

// Ticker da tela de previsão do tempo.
constexpr int TICKER_H = 16;
constexpr int TICKER_Y = SAFE_B - TICKER_H; // 206

} // namespace crt
