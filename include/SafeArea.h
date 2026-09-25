#pragma once
// ============================================================================
// Geometria do quadro de vídeo do Retro TV.
//
// O quadro lógico é 320x240. No Core2 original ele saía esticado sobre toda a
// área ativa do NTSC (composto); no Fruit Jam sai dobrado para 640x480 no DVI.
// O que estas constantes definem é onde o conteúdo pode ser desenhado dentro
// desse quadro: texto, réguas e faixas ficam na "área segura" e só o fundo
// sangra até o limite do raster (sem tarjas pretas).
//
// Por que a caixa segura continua a mesma no DVI: um monitor de computador
// mostra os 640x480 inteiros, sem overscan, e ali a margem é só respiro. Mas
// muita TV pela HDMI aplica overscan por padrão (o mesmo ~5-7% de borda do
// tubo, herdado da transmissão), e um conversor HDMI->RCA leva o quadro de
// volta a um tubo de verdade. Conteúdo dentro da caixa aparece nos três casos;
// encolher a margem só ganharia pixels no monitor e cortaria texto na TV.
// E como o BurnIn.h tira dessa margem a folga da deriva, mexer aqui mexe lá.
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

// Ticker da tela de previsão do tempo.
constexpr int TICKER_H = 16;
constexpr int TICKER_Y = SAFE_B - TICKER_H; // 206

} // namespace crt
