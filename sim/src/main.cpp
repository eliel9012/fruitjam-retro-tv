// ============================================================================
//  Simulador de telas da saída de vídeo — Fruit Jam Retro TV
//
//  Roda no Mac/Linux usando o backend SDL da LovyanGFX (lgfx Panel_sdl), o
//  MESMO rasterizador e as MESMAS fontes bitmap do firmware (ver fj/Gfx.h).
//  Serve para conferir diagramação — sobretudo se algo escapa da área segura —
//  sem ligar o Fruit Jam na TV.
//
//  O que NÃO é simulado: Wi-Fi (ESP32-C6), cartão SD, áudio (TLV320), a saída
//  DVI em si, decodificação MJPEG, FreeRTOS e os botões físicos. Os dados
//  exibidos são de exemplo, fixos.
//
//  Teclas:  ESQ/DIR ou 1..8 troca de tela
//           G liga/desliga a guia de overscan
//           ESC fecha
// ============================================================================

#include "fj/Gfx.h" // LovyanGFX + backend SDL, a mesma porta de entrada do firmware
#include "SimPanel.h" // painel SDL em 320x240, não no 240x320 padrão da LovyanGFX

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "SafeArea.h"

// Mesmo acento do firmware (RCA_ACCENT em src/main.cpp). NAO usar ACCENT:
// ciano saturado provoca dot crawl na saida composta, e foi a causa do glitch
// que aparecia so no item selecionado do menu. Contraste por luminancia, nao
// por saturacao.
static constexpr uint16_t ACCENT = 0x96BC;
#include "VcrOsd.h"
#include "WeatherIcons.h"
#include "TestPattern.h"
#include "RadioScreen.h"

using namespace crt;

// ---------------------------------------------------------------------------
//  Painel SDL com a geometria do quadro lógico (320x240, exibido em 4:3). No
//  aparelho esse quadro sai dobrado para 640x480 no DVI; aqui a janela é que
//  amplia (setScaling).
//
//  O painel é alocado e nunca liberado de propósito: o ~Panel_sdl() mexe numa
//  lista global da LovyanGFX, e a ordem de destruição de estáticos entre
//  unidades de tradução não é garantida — com ela invertida, o --png gravava
//  tudo e caía em segfault na saída. O Makefile também linka a biblioteca
//  antes, mas isto aqui não depende do linker.
// ---------------------------------------------------------------------------
static lgfx::Panel_sdl &panel = *new lgfx::Panel_sdl;
static lgfx::LGFX_Device tv;

// Guia de overscan: marca a área segura e a borda que um tubo esconde.
static bool showGuide = true;
static int screenIndex = 0;

static void drawGuide() {
  if (!showGuide)
    return;
  // Retângulo da área segura + cantos, em magenta (cor que não aparece nas telas).
  const uint16_t guide = tv.color565(255, 0, 255);
  tv.drawRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, guide);
  for (int i = 0; i < 8; ++i) {
    tv.drawPixel(SAFE_L - 2 - i, SAFE_T, guide);
    tv.drawPixel(SAFE_R + 1 + i, SAFE_T, guide);
    tv.drawPixel(SAFE_L - 2 - i, SAFE_B - 1, guide);
    tv.drawPixel(SAFE_R + 1 + i, SAFE_B - 1, guide);
  }
}

// ---------------------------------------------------------------------------
//  Blocos reaproveitados das telas (mesmas coordenadas do firmware).
// ---------------------------------------------------------------------------
static void header(const char *title) {
  tv.setTextDatum(top_left);
  tv.setTextSize(2);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString(title, SAFE_L, HEAD_Y);
  tv.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, ACCENT);
  tv.setTextSize(1);
}

static void controllerLabels(const char *left, const char *center, const char *right) {
  tv.fillRect(0, BAR_Y, W, BAR_H, TFT_NAVY);
  tv.setTextDatum(middle_center);
  tv.setTextSize(1);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString(std::string("[ ").append(left).append(" ]  [ ").append(center)
                     .append(" ]  [ ").append(right).append(" ]").c_str(),
                 W / 2, BAR_Y + BAR_H / 2);
}

// ---------------------------------------------------------------------------
//  Telas
// ---------------------------------------------------------------------------
static void screenHome() {
  // Mesma ordem e mesmo layout de drawHome() no firmware: 11 itens em duas
  // colunas de 6 e 5. Em coluna unica terminariam em 246 px, alem da barra.
  const char *items[] = {"VIDEOS",          "MUSICA",  "FOTOS",
                         "RADIO",           "TEMPO",   "TRAFEGO AEREO",
                         "PADRAO DE TESTE", "TRANSFERIR ARQUIVOS", "CONFIGURACOES",
                         "SISTEMA",         "DESLIGAR"};
  tv.fillScreen(TFT_NAVY);
  header("M5 RETRO TV");
  tv.setTextSize(1);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("QUA 23 SET   14:32:07", SAFE_L, crt::HEAD_RULE_Y + 4);
  for (int i = 0; i < 11; ++i) {
    const int x = SAFE_L + 4 + (i / 6) * 124;
    const int y = crt::HEAD_RULE_Y + 24 + (i % 6) * 16;
    tv.setTextColor(i == 0 ? ACCENT : TFT_WHITE, TFT_NAVY);
    tv.drawString((std::string(i == 0 ? ">" : " ") + items[i]).c_str(), x, y);
  }
  controllerLabels("ACIMA", "OK", "ABAIXO");
}

static void screenLibrary() {
  const char *progs[] = {"Primeiro Teste", "Filme Retro", "Desenho Anos 80", "Jornal 1987"};
  tv.fillScreen(TFT_NAVY);
  header("VIDEOS");
  for (int row = 0; row < 4; ++row) {
    const int y = BODY_Y + 14 + row * 32;
    const bool sel = (row == 0);
    tv.fillRoundRect(SAFE_L, y - 2, SAFE_W, 28, 4, sel ? ACCENT : TFT_NAVY);
    tv.setTextColor(sel ? TFT_NAVY : TFT_WHITE, sel ? ACCENT : TFT_NAVY);
    tv.drawString((std::string(sel ? "> " : "  ") + progs[row]).c_str(), SAFE_L + 8, y + 6);
  }
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("1 / 4", SAFE_R - 80, HEAD_Y);
  controllerLabels("ANTERIOR", "PLAY", "PROXIMO");
}

static void screenPlayback() {
  // Vídeo 240x160 centralizado no quadro, como o jpegDraw() faz.
  const int vw = 240, vh = 160;
  tv.fillScreen(TFT_BLACK);
  tv.fillRect((W - vw) / 2, (H - vh) / 2, vw, vh, tv.color565(40, 60, 90));
  tv.setTextDatum(middle_center);
  tv.setTextColor(TFT_DARKGREY, tv.color565(40, 60, 90));
  tv.drawString("(quadro MJPEG 240x160)", W / 2, H / 2);

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
  vcr::draw(&tv, osd);
}

static void screenRadar() {
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(top_left);
  tv.setTextSize(1);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("M5 RETRO TV", SAFE_L, SAFE_T);
  tv.drawString("TRAFEGO AEREO", SAFE_L, SAFE_T + 15);

  const int cx = 96, cy = 118, R = 58;
  tv.drawCircle(cx, cy, R, ACCENT);
  tv.drawCircle(cx, cy, R / 2, TFT_DARKCYAN);
  tv.drawFastHLine(cx - R, cy, 2 * R, TFT_DARKCYAN);
  tv.drawFastVLine(cx, cy - R, 2 * R, TFT_DARKCYAN);
  tv.setTextDatum(middle_center);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("N", cx, cy - R - 8);
  tv.drawString("S", cx, cy + R + 8);
  tv.drawString("W", cx - R - 8, cy);
  tv.drawString("E", cx + R + 8, cy);
  tv.setTextDatum(top_left);
  tv.drawString("120 km", cx - R, cy + R + 4);

  const int PX = SAFE_L + 144;
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("TAM3476", PX, SAFE_T + 18);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("ALT FL370", PX, SAFE_T + 40);
  tv.drawString("VEL 452 KT", PX, SAFE_T + 58);
  tv.drawString("DIST 68 km  PROA 214", PX, SAFE_T + 76);
  tv.setTextColor(TFT_DARKCYAN, TFT_NAVY);
  tv.drawString("120 km de alcance", PX, SAFE_T + 142);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("ATUALIZADO 12:04", PX, SAFE_T + 158);
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
    tv.fillRect(0, oy + start, W, y - start, color);
    start = y;
    color = c;
  }
  tv.fillRect(0, oy + start, W, H - start, color);
}

static void weatherTicker(int tickerOffset);

static void weatherHeader(const char *title) {
  tv.setFont(&fonts::Font4);
  tv.setTextDatum(top_center);
  tv.setTextSize(1);
  tv.setTextColor(TFT_YELLOW);
  tv.drawString(title, W / 2, SAFE_T);
  tv.drawFastHLine(SAFE_L, SAFE_T + 30, SAFE_W, ACCENT);
  // Igual ao firmware: a régua do ticker vive no cabeçalho, senão some na
  // primeira transição.
  tv.drawFastHLine(SAFE_L, TICKER_Y - 4, SAFE_W, ACCENT);
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
    tv.setFont(&fonts::Font2);
    tv.setTextSize(1);
    tv.setTextDatum(top_center);
    tv.setTextColor(ACCENT);
    tv.drawString(dias[i], cx, SAFE_T + 42);
    wx::drawWeatherIcon(&tv, cx, SAFE_T + 96, 56, wx::iconFromWmo(codigo[i]));
    tv.setTextColor(TFT_YELLOW);
    snprintf(buf, sizeof(buf), "%d", tmax[i]);
    tv.drawString(buf, cx, SAFE_T + 128);
    tv.setTextColor(TFT_WHITE);
    snprintf(buf, sizeof(buf), "%d", tmin[i]);
    tv.drawString(buf, cx, SAFE_T + 148);
  }
  tv.setFont(&fonts::Font2);
  tv.setTextColor(ACCENT);
  tv.drawString("MAXIMA / MINIMA EM GRAUS C", W / 2, SAFE_T + 166);
  weatherTicker(0);
}

static void screenWeather(uint32_t ms, int tickerOffset) {
  (void)ms;
  weatherPaintBackground(0);
  weatherHeader("FRANCA - SP");
  wx::drawWeatherIcon(&tv, SAFE_L + 46, SAFE_T + 96, 76, wx::iconFromWmo(2));
  const int col = SAFE_L + 176;
  tv.setFont(&fonts::Font2);
  tv.setTextSize(1);
  tv.setTextDatum(top_center);
  tv.setTextColor(TFT_WHITE);
  tv.drawString("PARCIAL NUBLADO", col, SAFE_T + 46);
  tv.setFont(&fonts::Font4);
  tv.setTextSize(2);
  tv.setTextColor(TFT_YELLOW);
  tv.drawString("26 C", col, SAFE_T + 70);
  tv.setFont(&fonts::Font2);
  tv.setTextSize(1);
  tv.setTextColor(TFT_WHITE);
  tv.drawString("UMIDADE  62%", W / 2, SAFE_T + 140);
  tv.drawString("VENTO  12 KM/H  SO", W / 2, SAFE_T + 158);
  tv.drawFastHLine(SAFE_L, TICKER_Y - 4, SAFE_W, ACCENT);

  weatherTicker(tickerOffset);
}

// O ticker existe nas duas páginas: no firmware ele é um sprite empurrado a
// 30 Hz, independente de qual página está desenhada.
static void weatherTicker(int tickerOffset) {
  static const char *payload = "SEG 26/15C    TER 27/14C    QUA 25/12C      ";
  tv.fillRect(0, TICKER_Y, W, TICKER_H, TFT_BLACK);
  tv.setClipRect(0, TICKER_Y, W, TICKER_H);
  tv.setTextDatum(top_left);
  tv.setTextColor(ACCENT, TFT_BLACK);
  const int payloadW = tv.textWidth(payload);
  for (int x = -tickerOffset; x < W; x += payloadW)
    tv.drawString(payload, x, TICKER_Y);
  tv.clearClipRect();
  tv.setFont(&fonts::Font0);
}

static void screenSettings() {
  // Mostra a saída de áudio, que é o rótulo que o port mudou: "RCA" e
  // "INTERNO" viraram "TV (P2)" e "ALTO-FALANTE" (PORTING.md 3.4). O item
  // CORES saiu — o painel agora é sempre RGB565.
  tv.fillScreen(TFT_NAVY);
  header("CONFIGURACOES");
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("> SAIDA AUDIO", SAFE_L, BODY_Y + 26);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("TV (P2)", SAFE_L + 10, BODY_Y + 58);
  controllerLabels("-", "SALVAR", "+");
}

static void screenInfo() {
  tv.fillScreen(TFT_NAVY);
  header("SISTEMA");
  const int y0 = BODY_Y;
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("MEMORIA LIVRE", SAFE_L, y0);
  tv.drawString("PSRAM LIVRE", SAFE_L, y0 + 26);
  tv.drawString("VERSAO", SAFE_L, y0 + 52);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("301056 bytes", SAFE_L + 116, y0);
  tv.drawString("8126464 bytes", SAFE_L + 116, y0 + 26);
  tv.drawString("fruitjam", SAFE_L + 116, y0 + 52);
  controllerLabels("ANTERIOR", "DETALHES", "PROXIMO");
}

static void screenMusicPlaying() {
  tv.fillScreen(TFT_NAVY);
  header("MUSICA");
  const int coverY = BODY_Y + 8, metaX = SAFE_L + 96;
  tv.drawRect(SAFE_L, coverY, 88, 88, TFT_DARKCYAN);
  tv.setTextDatum(top_left);
  tv.setTextColor(TFT_YELLOW, TFT_NAVY);
  tv.drawString("Nao Chores Mais", metaX, coverY);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("Gilberto Gil", metaX, coverY + 30);
  tv.drawString("Realce", metaX, coverY + 50);
  tv.drawString("1979", metaX, coverY + 70);
  tv.setTextColor(TFT_DARKCYAN, TFT_NAVY);
  tv.drawString("3/12 192K", SAFE_R - 96, HEAD_RULE_Y + 6);
  tv.drawRect(SAFE_L, 160, SAFE_W, 6, ACCENT);
  tv.fillRect(SAFE_L + 1, 161, 120, 4, TFT_YELLOW);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("PLAY 01:48 / 04:05  SHUFFLE", SAFE_L, 174);
  controllerLabels("ANTERIOR", "PAUSAR", "PROXIMO");
}


// Página 1 do SISTEMA (espelha drawInfo com infoPage = 1).
static void screenInfoNetwork() {
  tv.fillScreen(TFT_NAVY);
  header("SISTEMA");
  const int y0 = BODY_Y;
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("STATUS DA REDE", SAFE_L, y0);
  tv.drawString("SINAL", SAFE_L, y0 + 26);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("CONECTADO", SAFE_L + 144, y0);
  tv.drawString("-58 dBm", SAFE_L + 144, y0 + 26);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.drawString("ENDERECO IP", SAFE_L, y0 + 52);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("192.168.0.42", SAFE_L + 144, y0 + 52);
  controllerLabels("ANTERIOR", "DETALHES", "PROXIMO");
}

// Navegador de pastas da MUSICA (espelha drawMusicBrowser).
static void screenMusicBrowser() {
  static const char *itens[4] = {"Gilberto Gil/", "Realce/", "Nao Chores Mais.mp3",
                                 "Toda Menina Baiana.mp3"};
  static const bool pasta[4] = {true, true, false, false};
  tv.fillScreen(TFT_NAVY);
  header("MUSICA");
  tv.setTextDatum(top_left);
  tv.setTextColor(TFT_DARKCYAN, TFT_NAVY);
  tv.drawString("/Gilberto Gil/Realce", SAFE_L, HEAD_RULE_Y + 6);
  for (int row = 0; row < 4; ++row) {
    const int y = BODY_Y + 14 + row * 32;
    const bool sel = (row == 2);
    tv.fillRoundRect(SAFE_L, y - 2, SAFE_W, 28, 4, sel ? ACCENT : TFT_NAVY);
    tv.setTextColor(sel ? TFT_NAVY : (pasta[row] ? ACCENT : TFT_WHITE), sel ? ACCENT : TFT_NAVY);
    tv.drawString((std::string(sel ? "> " : "  ") + itens[row]).c_str(), SAFE_L + 8, y + 6);
  }
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("3 / 4", SAFE_R - 80, HEAD_Y);
  controllerLabels("ACIMA", "OK", "ABAIXO");
}

// Portal de configuracao de rede (espelha drawSetupPortal sobre o dualText).
static void screenPortal() {
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(middle_center);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.setTextSize(2);
  tv.drawString("CONFIGURACAO", W / 2, 94);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.setTextSize(1);
  tv.drawString("WI-FI: M5RETRO-SETUP", W / 2, 130);
  tv.setTextDatum(top_left);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString("SENHA: retro1988", SAFE_L, 144);
  tv.drawString("ABRA: 192.168.4.1", SAFE_L, 162);
  tv.drawString("AGUARDANDO CELULAR...", SAFE_L, 180);
}

// Tela de erro (espelha setError, que passa pelo dualText).
static void screenError() {
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(middle_center);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.setTextSize(2);
  tv.drawString("ERRO DO SISTEMA", W / 2, 94);
  tv.setTextColor(ACCENT, TFT_NAVY);
  tv.setTextSize(1);
  tv.drawString("CARTAO SD NAO ENCONTRADO", W / 2, 130);
}

// As duas telas abaixo chamam os MODULOS DE VERDADE, nao uma reimplementacao:
// foi uma copia divergente da disposicao que custou o tools/render_screens.py.
// O que sair aqui e literalmente o que o firmware desenha.
static void screenTestPattern() {
  testpattern::drawBars(&tv, 0, 0);
}

static void screenRadio() {
  radioui::State st;
  st.hour = 14; st.minute = 32; st.second = 7;
  st.day = 23; st.month = 9; st.year = 2026;
  st.weekday = 3; // quarta
  st.timeValid = true;
  st.stationName = "DIARIO FM";
  st.statusText = "NO AR";
  st.title = "";
  radioui::drawBackground(&tv, 0, 0);
  radioui::draw(&tv, 0, 0, st);
}

// A apresentacao de fotos precisa do JPEGDEC, que o simulador nao linka: aqui
// so a moldura (legenda, contador e rodape) e real, e a "foto" e um degrade
// sintetico ocupando o lugar dela.
static void screenPhotos() {
  tv.fillScreen(TFT_BLACK);
  for (int y = 0; y < crt::H; ++y) {
    const int v = 255 * y / crt::H;
    tv.drawFastHLine(0, y, crt::W, tv.color565(v / 3, v / 2, 200 - v / 2));
  }
  tv.setTextDatum(top_left);
  tv.setTextSize(1);
  tv.fillRect(SAFE_L, crt::SAFE_B - 34, SAFE_W, 16, TFT_BLACK);
  tv.setTextColor(TFT_WHITE, TFT_BLACK);
  tv.drawString("PADOVA_1988.JPG", SAFE_L + 2, crt::SAFE_B - 32);
  tv.setTextColor(ACCENT, TFT_BLACK);
  tv.drawString("7 / 42", crt::SAFE_R - 40, crt::SAFE_B - 32);
  controllerLabels("ANTERIOR", "TEMPO", "PROXIMO");
}

// ---------------------------------------------------------------------------
static const char *SCREEN_NAMES[] = {"INICIO",        "BIBLIOTECA", "PLAYER",
                                     "RADAR",         "PREVISAO",   "PREVISAO3DIAS",
                                     "CONFIGURACOES", "SISTEMA",    "SISTEMA_REDE",
                                     "MUSICA",        "MUSICA_TOCANDO", "PORTAL",
                                     "FOTOS",         "RADIO",      "PADRAO_TESTE",
                                     "ERRO"};
constexpr int SCREEN_COUNT = 16;

static void drawCurrent(uint32_t ms, int tickerOffset) {
  switch (screenIndex) {
  case 0: screenHome(); break;
  case 1: screenLibrary(); break;
  case 2: screenPlayback(); break;
  case 3: screenRadar(); break;
  case 4: screenWeather(ms, tickerOffset); break;
  case 5: screenForecast(); break;
  case 6: screenSettings(); break;
  case 7: screenInfo(); break;
  case 8: screenInfoNetwork(); break;
  case 9: screenMusicBrowser(); break;
  case 10: screenMusicPlaying(); break;
  case 11: screenPortal(); break;
  case 12: screenPhotos(); break;
  case 13: screenRadio(); break;
  case 14: screenTestPattern(); break;
  default: screenError(); break;
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
  // Só 1..9 têm tecla própria; o resto se alcança pelas setas.
  const int mapped = SCREEN_COUNT < 9 ? SCREEN_COUNT : 9;
  for (int i = 0; i < mapped; ++i)
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
  // A guia magenta é ferramenta de depuração, não pertence a uma captura que
  // vai para a documentação.
  showGuide = false;
  for (screenIndex = 0; screenIndex < SCREEN_COUNT; ++screenIndex) {
    drawCurrent(0, 0);
    size_t len = 0;
    uint8_t *png = (uint8_t *)tv.createPng(&len, 0, 0, W, H);
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
  panel.setWindowTitle("RETRO TV - Fruit Jam (320x240, 4:3)");
  panel.setScaling(3, 3); // janela de 960x720; o quadro em si segue 320x240
  sim::configure(panel);
  tv.setPanel(&panel);
  if (!tv.init()) {
    fprintf(stderr, "falha ao iniciar o painel SDL\n");
    return 1;
  }
  tv.setColorDepth(16);
  registerKeys();

  if (argc >= 2 && !strcmp(argv[1], "--png"))
    return exportPng(argc >= 3 ? argv[2] : ".");

  printf("[sim] ESQ/DIR ou 1..8 troca de tela, G liga/desliga a guia de overscan, ESC fecha.\n");
  printf("[sim] tela: %s\n", SCREEN_NAMES[screenIndex]);
  return lgfx::Panel_sdl::main(simLoop, 33);
}
