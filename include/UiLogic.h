#pragma once
#include <stdint.h>
enum UiState {
  BOOT,
  HOME,
  VIDEO_LIBRARY,
  VIDEO_PLAYBACK,
  AIRCRAFT_RADAR,
  SETTINGS,
  SYSTEM_INFO,
  SETUP_PORTAL,
  WEATHER,
  MUSIC_BROWSER,
  MUSIC_NOW_PLAYING,
  FILE_TRANSFER,
  PHOTO_SHOW,
  RADIO,
  TEST_PATTERN,
  EMULATORS,
  ERROR_SCREEN
};

// Itens do menu inicial, em duas colunas. Uma constante so para desenho,
// navegacao e toque: ja houve divergencia entre os tres e o resultado foi o
// ultimo item ficar inalcancavel.
constexpr int HOME_ITEM_COUNT = 12;
constexpr int HOME_ROWS = 6; // 6 na coluna da esquerda, 6 na direita
inline UiState homeTarget(int selection) {
  // A ultima posicao (DESLIGAR) nao tem tela: quem navega trata antes de
  // chamar isto, e cair aqui devolve HOME. EMULADORES (penultima posicao) e
  // a mesma UiState nas duas builds (standalone e fruitjam-launcher); quem
  // muda de comportamento entre elas e enterEmulators(), em main.cpp.
  const UiState targets[] = {VIDEO_LIBRARY, MUSIC_BROWSER,  PHOTO_SHOW,    RADIO,
                             WEATHER,       AIRCRAFT_RADAR, TEST_PATTERN,  FILE_TRANSFER,
                             SETTINGS,      SYSTEM_INFO,    EMULATORS};
  const int n = (int)(sizeof(targets) / sizeof(targets[0]));
  return selection >= 0 && selection < n ? targets[selection] : HOME;
}

// Coluna (0 ou 1) e linha de um item do menu inicial.
inline int homeColumn(int index) { return index / HOME_ROWS; }
inline int homeRow(int index) { return index % HOME_ROWS; }

// homeHit, touchButton, isPlayerAudioButton e isBackButton sao do touch do
// Core2. O Fruit Jam nao tem tela local nem toque, entao o firmware deixou de
// chama-las; ficam porque tests/test_core.cpp as cobre e o fork ainda puxa
// correcoes do upstream.

// Item sob um toque no menu inicial, ou -1 fora da area dos itens.
// `x0`/`y0` sao o canto do primeiro item, `colW`/`step` a largura da coluna e a
// altura da linha.
inline int homeHit(int x, int y, int x0, int y0, int colW, int step) {
  if (y < y0 || x < x0)
    return -1;
  const int col = (x - x0) / colW, row = (y - y0) / step;
  if (col < 0 || col > 1 || row < 0 || row >= HOME_ROWS)
    return -1;
  const int index = col * HOME_ROWS + row;
  return index < HOME_ITEM_COUNT ? index : -1;
}
inline int touchButton(int x, int y) {
  if (x < 0 || x >= 320 || y < 184 || y >= 240)
    return -1;
  return x < 107 ? 0 : x < 214 ? 1 : 2;
}
inline bool timeReached(uint32_t now, uint32_t deadline) {
  return int32_t(now - deadline) >= 0;
}

inline bool isPlayerAudioButton(int x, int y) {
  return x >= 232 && x < 316 && y >= 160 && y < 184;
}

inline bool isBackButton(int x, int y) {
  return x >= 274 && x < 320 && y >= 0 && y < 40;
}
