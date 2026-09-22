// ============================================================================
//  Simulador de telas da saída RCA — M5 RETRO TV
//
//  Roda no Mac/Linux usando o backend SDL do M5GFX (lgfx Panel_sdl), o MESMO
//  rasterizador e as MESMAS fontes bitmap do firmware. Serve para conferir
//  diagramação — sobretudo se algo escapa da área segura do tubo — sem ligar o
//  Core2 na TV.
//
//  O que NÃO é simulado: Wi-Fi, cartão SD, I2S/áudio, decodificação MJPEG,
//  FreeRTOS, touch e tudo do M5Unified (o M5Unified 0.2.10 não tem backend SDL).
//  Os dados exibidos são de exemplo, fixos.
//
//  Teclas:  ESQ/DIR ou 1..8 troca de tela
//           G liga/desliga a guia de overscan
//           ESC fecha
// ============================================================================

#include <SDL2/SDL.h> // precisa vir antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "SafeArea.h"
#include "VcrOsd.h"
#include "WeatherIcons.h"

using namespace crt;

// ---------------------------------------------------------------------------
//  Painel SDL com a geometria do quadro composto (320x240, exibido em 4:3).
// ---------------------------------------------------------------------------
static lgfx::Panel_sdl panel;
static M5GFX rca;

// Guia de overscan: marca a área segura e a borda que um tubo esconde.
static bool showGuide = true;
static int screenIndex = 0;

static void drawGuide() {
  if (!showGuide)
    return;
  // Retângulo da área segura + cantos, em magenta (cor que não aparece nas telas).
  const uint16_t guide = rca.color565(255, 0, 255);
  rca.drawRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, guide);
  for (int i = 0; i < 8; ++i) {
    rca.drawPixel(SAFE_L - 2 - i, SAFE_T, guide);
    rca.drawPixel(SAFE_R + 1 + i, SAFE_T, guide);
    rca.drawPixel(SAFE_L - 2 - i, SAFE_B - 1, guide);
    rca.drawPixel(SAFE_R + 1 + i, SAFE_B - 1, guide);
  }
}

// ---------------------------------------------------------------------------
//  Blocos reaproveitados das telas (mesmas coordenadas do firmware).
// ---------------------------------------------------------------------------
static void header(const char *title) {
  rca.setTextDatum(top_left);
  rca.setTextSize(2);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(title, SAFE_L, HEAD_Y);
  rca.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, TFT_CYAN);
  rca.setTextSize(1);
}

static void controllerLabels(const char *left, const char *center, const char *right) {
  rca.fillRect(0, BAR_Y, W, BAR_H, TFT_NAVY);
  rca.setTextDatum(middle_center);
  rca.setTextSize(1);
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString(std::string("[ ").append(left).append(" ]  [ ").append(center)
                     .append(" ]  [ ").append(right).append(" ]").c_str(),
                 W / 2, BAR_Y + BAR_H / 2);
}

// ---------------------------------------------------------------------------
//  Telas
// ---------------------------------------------------------------------------
static void screenHome() {
  const char *items[] = {"VIDEOS",  "MUSICA", "TRAFEGO AEREO", "CONFIGURACOES",
                         "SISTEMA", "TEMPO",  "DESLIGAR"};
  rca.fillScreen(TFT_NAVY);
  header("M5 RETRO TV");
  for (int i = 0; i < 7; ++i) {
    rca.setTextColor(i == 0 ? TFT_CYAN : TFT_WHITE, TFT_NAVY);
    rca.drawString((std::string(i == 0 ? "> " : "  ") + items[i]).c_str(), SAFE_L + 4,
                   BODY_Y + i * 20);
  }
  controllerLabels("ACIMA", "OK", "ABAIXO");
}

static void screenLibrary() {
  const char *progs[] = {"Primeiro Teste", "Filme Retro", "Desenho Anos 80", "Jornal 1987"};
  rca.fillScreen(TFT_NAVY);
  header("VIDEOS");
  for (int row = 0; row < 4; ++row) {
    const int y = BODY_Y + 14 + row * 32;
    const bool sel = (row == 0);
    rca.fillRoundRect(SAFE_L, y - 2, SAFE_W, 28, 4, sel ? TFT_CYAN : TFT_NAVY);
    rca.setTextColor(sel ? TFT_NAVY : TFT_WHITE, sel ? TFT_CYAN : TFT_NAVY);
    rca.drawString((std::string(sel ? "> " : "  ") + progs[row]).c_str(), SAFE_L + 8, y + 6);
  }
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString("1 / 4", SAFE_R - 80, HEAD_Y);
  controllerLabels("ANTERIOR", "PLAY", "PROXIMO");
}

static void screenPlayback() {
  // Vídeo 240x160 centralizado no quadro, como o jpegDraw() faz.
  const int vw = 240, vh = 160;
  rca.fillScreen(TFT_BLACK);
  rca.fillRect((W - vw) / 2, (H - vh) / 2, vw, vh, rca.color565(40, 60, 90));
  rca.setTextDatum(middle_center);
  rca.setTextColor(TFT_DARKGREY, rca.color565(40, 60, 90));
  rca.drawString("(quadro MJPEG 240x160)", W / 2, H / 2);

  // Chama o MESMO OSD do firmware (include/VcrOsd.h) em vez de imitá-lo: era
  // daqui que vinha a divergência com a tela real do aparelho.
  vcr::State osd;
  osd.transport = vcr::Transport::Play;
  osd.speed = vcr::Speed::SP;
  osd.seconds = 42;
  osd.title = "CIDADE MARAVILHOSA 1988";
  osd.buttonLeft = "ANTERIOR";
  osd.buttonCenter = "PAUSA";
  osd.buttonRight = "PROXIMO";
  vcr::draw(&rca, osd);
}

static void screenRadar() {
  rca.fillScreen(TFT_NAVY);
  rca.setTextDatum(top_left);
  rca.setTextSize(1);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString("M5 RETRO TV", SAFE_L, SAFE_T);
  rca.drawString("TRAFEGO AEREO", SAFE_L, SAFE_T + 15);

  const int cx = 96, cy = 118, R = 58;
  rca.drawCircle(cx, cy, R, TFT_CYAN);
  rca.drawCircle(cx, cy, R / 2, TFT_DARKCYAN);
  rca.drawFastHLine(cx - R, cy, 2 * R, TFT_DARKCYAN);
  rca.drawFastVLine(cx, cy - R, 2 * R, TFT_DARKCYAN);
  rca.setTextDatum(middle_center);
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString("N", cx, cy - R - 8);
  rca.drawString("S", cx, cy + R + 8);
  rca.drawString("W", cx - R - 8, cy);
  rca.drawString("E", cx + R + 8, cy);
  rca.setTextDatum(top_left);
  rca.drawString("120 km", cx - R, cy + R + 4);

  const int PX = SAFE_L + 144;
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString("TAM3476", PX, SAFE_T + 18);
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString("ALT FL370", PX, SAFE_T + 40);
  rca.drawString("VEL 452 KT", PX, SAFE_T + 58);
  rca.drawString("DIST 68 km  PROA 214", PX, SAFE_T + 76);
  rca.setTextColor(TFT_DARKCYAN, TFT_NAVY);
  rca.drawString("120 km de alcance", PX, SAFE_T + 142);
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString("ATUALIZADO 12:04", PX, SAFE_T + 158);
  controllerLabels("ANTERIOR", "DETALHES", "PROXIMO");
}

// Mesmo gradiente do firmware (weatherPaintBackground): azul estável, sem o
// ciclo de matiz que existia antes e deixava a tela verde/rosa no tubo.
static constexpr uint16_t WX_TOP = 0x0010, WX_BOTTOM = 0x18BF;

static inline uint16_t weatherGradientRow(int y) {
  const int r0 = (WX_TOP >> 11) & 0x1F, g0 = (WX_TOP >> 5) & 0x3F, b0 = WX_TOP & 0x1F;
  const int r1 = (WX_BOTTOM >> 11) & 0x1F, g1 = (WX_BOTTOM >> 5) & 0x3F, b1 = WX_BOTTOM & 0x1F;
  const int r = r0 + (r1 - r0) * y / (H - 1);
  const int g = g0 + (g1 - g0) * y / (H - 1);
  const int b = b0 + (b1 - b0) * y / (H - 1);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Espelha o firmware: 16 faixas, não 240 linhas.
static void weatherPaintBackground(int oy) {
  int start = 0;
  uint16_t color = weatherGradientRow(0);
  for (int y = 1; y < H; ++y) {
    const uint16_t c = weatherGradientRow(y);
    if (c == color)
      continue;
    rca.fillRect(0, oy + start, W, y - start, color);
    start = y;
    color = c;
  }
  rca.fillRect(0, oy + start, W, H - start, color);
}

static void weatherHeader(const char *title) {
  rca.setFont(&fonts::Font4);
  rca.setTextDatum(top_center);
  rca.setTextSize(1);
  rca.setTextColor(TFT_YELLOW);
  rca.drawString(title, W / 2, SAFE_T);
  rca.drawFastHLine(SAFE_L, SAFE_T + 30, SAFE_W, TFT_CYAN);
}

// Página 1 da previsão: três dias em colunas, com ícone por dia.
static void screenForecast() {
  static const char *dias[3] = {"SEG", "TER", "QUA"};
  static const int codigo[3] = {3, 61, 95};
  static const int tmax[3] = {26, 27, 25}, tmin[3] = {15, 14, 12};
  weatherPaintBackground(0);
  weatherHeader("PREVISAO 3 DIAS");
  const int colW = SAFE_W / 3;
  char buf[24];
  for (int i = 0; i < 3; ++i) {
    const int cx = SAFE_L + colW * i + colW / 2;
    rca.setFont(&fonts::Font2);
    rca.setTextSize(1);
    rca.setTextDatum(top_center);
    rca.setTextColor(TFT_CYAN);
    rca.drawString(dias[i], cx, SAFE_T + 42);
    wx::drawWeatherIcon(&rca, cx, SAFE_T + 96, 56, wx::iconFromWmo(codigo[i]));
    rca.setTextColor(TFT_YELLOW);
    snprintf(buf, sizeof(buf), "%d", tmax[i]);
    rca.drawString(buf, cx, SAFE_T + 128);
    rca.setTextColor(TFT_WHITE);
    snprintf(buf, sizeof(buf), "%d", tmin[i]);
    rca.drawString(buf, cx, SAFE_T + 148);
  }
  rca.setFont(&fonts::Font2);
  rca.setTextColor(TFT_CYAN);
  rca.drawString("MAXIMA / MINIMA EM GRAUS C", W / 2, SAFE_T + 172);
}

static void screenWeather(uint32_t ms, int tickerOffset) {
  (void)ms;
  weatherPaintBackground(0);
  weatherHeader("FRANCA - SP");
  wx::drawWeatherIcon(&rca, SAFE_L + 46, SAFE_T + 96, 76, wx::iconFromWmo(2));
  const int col = SAFE_L + 176;
  rca.setFont(&fonts::Font2);
  rca.setTextSize(1);
  rca.setTextDatum(top_center);
  rca.setTextColor(TFT_WHITE);
  rca.drawString("PARCIAL NUBLADO", col, SAFE_T + 46);
  rca.setFont(&fonts::Font4);
  rca.setTextSize(2);
  rca.setTextColor(TFT_YELLOW);
  rca.drawString("26 C", col, SAFE_T + 70);
  rca.setFont(&fonts::Font2);
  rca.setTextSize(1);
  rca.setTextColor(TFT_WHITE);
  rca.drawString("UMIDADE  62%", W / 2, SAFE_T + 140);
  rca.drawString("VENTO  12 KM/H  SO", W / 2, SAFE_T + 158);
  rca.drawFastHLine(SAFE_L, TICKER_Y - 4, SAFE_W, TFT_CYAN);

  // Ticker rolante, como o sprite do firmware.
  static const char *payload = "SEG 26/15C    TER 27/14C    QUA 25/12C      ";
  rca.fillRect(0, TICKER_Y, W, TICKER_H, TFT_BLACK);
  rca.setClipRect(0, TICKER_Y, W, TICKER_H);
  rca.setTextDatum(top_left);
  rca.setTextColor(TFT_CYAN, TFT_BLACK);
  const int payloadW = rca.textWidth(payload);
  for (int x = -tickerOffset; x < W; x += payloadW)
    rca.drawString(payload, x, TICKER_Y);
  rca.clearClipRect();
  rca.setFont(&fonts::Font0);
}

static void screenSettings() {
  rca.fillScreen(TFT_NAVY);
  header("CONFIGURACOES");
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString("> VIDEO", SAFE_L, BODY_Y + 26);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString("NTSC", SAFE_L + 10, BODY_Y + 58);
  controllerLabels("-", "SALVAR", "+");
}

static void screenInfo() {
  rca.fillScreen(TFT_NAVY);
  header("SISTEMA");
  const int y0 = BODY_Y;
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString("MEMORIA LIVRE", SAFE_L, y0);
  rca.drawString("PSRAM LIVRE", SAFE_L, y0 + 26);
  rca.drawString("VERSAO", SAFE_L, y0 + 52);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString("98504 bytes", SAFE_L + 116, y0);
  rca.drawString("4423480 bytes", SAFE_L + 116, y0 + 26);
  rca.drawString("core2", SAFE_L + 116, y0 + 52);
  controllerLabels("ANTERIOR", "DETALHES", "PROXIMO");
}

static void screenMusicPlaying() {
  rca.fillScreen(TFT_NAVY);
  header("MUSICA");
  const int coverY = BODY_Y + 8, metaX = SAFE_L + 96;
  rca.drawRect(SAFE_L, coverY, 88, 88, TFT_DARKCYAN);
  rca.setTextDatum(top_left);
  rca.setTextColor(TFT_YELLOW, TFT_NAVY);
  rca.drawString("Nao Chores Mais", metaX, coverY);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString("Gilberto Gil", metaX, coverY + 30);
  rca.drawString("Realce", metaX, coverY + 50);
  rca.drawString("1979", metaX, coverY + 70);
  rca.setTextColor(TFT_DARKCYAN, TFT_NAVY);
  rca.drawString("3/12 192K", SAFE_R - 96, HEAD_RULE_Y + 6);
  rca.drawRect(SAFE_L, 160, SAFE_W, 6, TFT_CYAN);
  rca.fillRect(SAFE_L + 1, 161, 120, 4, TFT_YELLOW);
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString("PLAY 01:48 / 04:05  SHUFFLE", SAFE_L, 174);
  controllerLabels("ANTERIOR", "PAUSAR", "PROXIMO");
}

// ---------------------------------------------------------------------------
static const char *SCREEN_NAMES[] = {"INICIO",   "BIBLIOTECA",    "PLAYER",  "RADAR",
                                     "PREVISAO", "CONFIGURACOES", "SISTEMA", "MUSICA",
                                     "PREVISAO3DIAS"};
constexpr int SCREEN_COUNT = 9;

static void drawCurrent(uint32_t ms, int tickerOffset) {
  switch (screenIndex) {
  case 0: screenHome(); break;
  case 1: screenLibrary(); break;
  case 2: screenPlayback(); break;
  case 3: screenRadar(); break;
  case 4: screenWeather(ms, tickerOffset); break;
  case 5: screenSettings(); break;
  case 6: screenInfo(); break;
  case 7: screenMusicPlaying(); break;
  default: screenForecast(); break;
  }
  drawGuide();
}

// O Panel_sdl bombeia os eventos na thread principal (obrigatório no Cocoa) e
// roda esta função numa thread secundária, então aqui NÃO se chama nada do SDL.
// A comunicação é pelos "GPIOs" emulados: o Panel_sdl derruba o pino no keydown
// e levanta no keyup, e esta thread só lê o nível com gpio_in().
static constexpr uint8_t PIN_PREV = 39;  // seta esquerda (padrão do Panel_sdl)
static constexpr uint8_t PIN_NEXT = 37;  // seta direita  (padrão do Panel_sdl)
static constexpr uint8_t PIN_GUIDE = 60; // tecla G
static constexpr uint8_t PIN_QUIT = 61;  // tecla ESC
static constexpr uint8_t PIN_SCREEN0 = 64; // teclas 1..8 -> 64..71

static void registerKeys() {
  lgfx::Panel_sdl::addKeyCodeMapping(SDLK_g, PIN_GUIDE);
  lgfx::Panel_sdl::addKeyCodeMapping(SDLK_ESCAPE, PIN_QUIT);
  for (int i = 0; i < SCREEN_COUNT; ++i)
    lgfx::Panel_sdl::addKeyCodeMapping((SDL_KeyCode)(SDLK_1 + i), PIN_SCREEN0 + i);
}

// Nível baixo = tecla pressionada. Devolve true só na borda de descida.
static bool pressed(uint8_t pin, bool *prev) {
  const bool down = !lgfx::gpio_in(pin);
  const bool edge = down && !*prev;
  *prev = down;
  return edge;
}

// Chamada em laço pelo Panel_sdl::main(). Retorna 0 para continuar.
static int simLoop(bool *running) {
  static uint32_t tick = 0;
  static int tickerOffset = 0;
  static bool wasPrev = false, wasNext = false, wasGuide = false, wasQuit = false;
  static bool wasScreen[SCREEN_COUNT] = {};

  bool changed = false;
  if (pressed(PIN_QUIT, &wasQuit)) {
    *running = false;
    return 0;
  }
  if (pressed(PIN_GUIDE, &wasGuide)) {
    showGuide = !showGuide;
    changed = true;
  }
  if (pressed(PIN_NEXT, &wasNext)) {
    screenIndex = (screenIndex + 1) % SCREEN_COUNT;
    changed = true;
  }
  if (pressed(PIN_PREV, &wasPrev)) {
    screenIndex = (screenIndex + SCREEN_COUNT - 1) % SCREEN_COUNT;
    changed = true;
  }
  for (int i = 0; i < SCREEN_COUNT; ++i) {
    if (pressed(PIN_SCREEN0 + i, &wasScreen[i])) {
      screenIndex = i;
      changed = true;
    }
  }
  if (changed)
    printf("[sim] tela: %s   guia de overscan: %s\n", SCREEN_NAMES[screenIndex],
           showGuide ? "ligada" : "desligada");

  tick += 33;
  tickerOffset = (tickerOffset + 2) % 512;
  drawCurrent(tick, tickerOffset);
  return 0;
}

// Modo headless: desenha cada tela e grava um PNG, sem abrir o laço da janela.
// Útil para conferir a diagramação em CI ou para gerar os renders da documentação
// com o rasterizador real, em vez dos mockups em Python.
static int exportPng(const char *dir) {
  char path[512];
  for (screenIndex = 0; screenIndex < SCREEN_COUNT; ++screenIndex) {
    drawCurrent(0, 0);
    size_t len = 0;
    uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, W, H);
    if (!png) {
      fprintf(stderr, "falha ao gerar PNG de %s\n", SCREEN_NAMES[screenIndex]);
      return 1;
    }
    snprintf(path, sizeof(path), "%s/rca_%d_%s.png", dir, screenIndex + 1,
             SCREEN_NAMES[screenIndex]);
    FILE *f = fopen(path, "wb");
    if (!f) {
      fprintf(stderr, "nao consegui escrever %s\n", path);
      free(png);
      return 1;
    }
    fwrite(png, 1, len, f);
    fclose(f);
    free(png);
    printf("[sim] %s (%zu bytes)\n", path, len);
  }
  return 0;
}

int main(int argc, char **argv) {
  panel.setWindowTitle("M5 RETRO TV - saida RCA (320x240, 4:3)");
  panel.setScaling(3, 3); // janela de 960x720; o quadro em si segue 320x240
  rca.setPanel(&panel);
  if (!rca.init()) {
    fprintf(stderr, "falha ao iniciar o painel SDL\n");
    return 1;
  }
  rca.setColorDepth(16);
  registerKeys();

  if (argc >= 2 && !strcmp(argv[1], "--png"))
    return exportPng(argc >= 3 ? argv[2] : ".");

  printf("[sim] ESQ/DIR ou 1..8 troca de tela, G liga/desliga a guia de overscan, ESC fecha.\n");
  printf("[sim] tela: %s\n", SCREEN_NAMES[screenIndex]);
  return lgfx::Panel_sdl::main(simLoop, 33);
}
