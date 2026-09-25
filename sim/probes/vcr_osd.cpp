// ============================================================================
//  Probe do OSD estilo videocassete (include/VcrOsd.h).
//
//  Pinta um "quadro de video" falso com regioes claras, escuras e de alto
//  contraste — o pior caso para texto sem tarja de fundo — e joga o OSD por
//  cima em varios estados. Gera PNGs em sim/build/ e, no fim, faz a conferencia
//  por pixel de que nenhum traco do OSD cai fora da area segura do tubo.
//
//    make -C sim probes && ./sim/build/probe_vcr_osd
//
//  A regra de probe do Makefile nao depende dos headers: depois de mexer em
//  include/VcrOsd.h, um `touch sim/probes/vcr_osd.cpp` antes do make.
// ============================================================================

#include "fj/Gfx.h" // LovyanGFX + backend SDL, a mesma porta de entrada do firmware
#include "SimPanel.h" // painel SDL em 320x240, não no 240x320 padrão da LovyanGFX

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SafeArea.h"
#include "VcrOsd.h"

static lgfx::Panel_sdl panel;
static lgfx::LGFX_Device tv;

// ---------------------------------------------------------------------------
//  Cena de fundo
// ---------------------------------------------------------------------------

// Quadro sintetico: ceu claro em cima, chao escuro embaixo, barras coloridas
// saturadas e um retangulo branco estourado exatamente sob a linha do contador.
// Se o contorno preto do OSD funciona aqui, funciona em qualquer cena.
static void fakeVideoFrame() {
  for (int y = 0; y < crt::H; ++y) {
    const int v = 255 - (y * 200) / crt::H;
    tv.drawFastHLine(0, y, crt::W, tv.color565(v / 3, v / 2, v));
  }
  tv.fillRect(0, 150, crt::W, 90, tv.color565(20, 60, 25)); // chao escuro

  const uint16_t bars[] = {tv.color565(255, 255, 255), tv.color565(255, 255, 0), tv.color565(0, 255, 255),
                           tv.color565(0, 255, 0),     tv.color565(255, 0, 255), tv.color565(255, 0, 0),
                           tv.color565(0, 0, 255),     tv.color565(0, 0, 0)};
  for (int i = 0; i < 8; ++i)
    tv.fillRect(i * 40, 96, 40, 40, bars[i]);

  // Estouro de branco bem debaixo do contador e do titulo.
  tv.fillRect(150, 155, 170, 60, tv.color565(255, 255, 255));
  tv.fillCircle(70, 200, 26, tv.color565(255, 240, 120));
}

static void savePng(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)tv.createPng(&len, 0, 0, crt::W, crt::H);
  if (!png) {
    printf("falha ao gerar %s\n", name);
    return;
  }
  char path[256];
  snprintf(path, sizeof(path), "build/%s", name);
  FILE *f = fopen(path, "wb");
  fwrite(png, 1, len, f);
  fclose(f);
  free(png);
  printf("gravado %s (%zu bytes)\n", path, len);
}

static void shot(const char *name, const vcr::State &state) {
  fakeVideoFrame();
  vcr::draw(&tv, state);
  savePng(name);
}

// ---------------------------------------------------------------------------
//  Conferencia por pixel da area segura
// ---------------------------------------------------------------------------

// Pinta o quadro inteiro de uma cor unica e desenha o OSD: qualquer pixel que
// deixou de ser essa cor e traco do OSD. Assim da para localizar a caixa exata
// do desenho sem depender de conhecer as coordenadas internas do header.
static int checkSafeArea(const vcr::State &state) {
  tv.fillRect(0, 0, crt::W, crt::H, tv.color565(0, 128, 0)); // verde: nao aparece no OSD
  // A referencia e lida do proprio buffer, nao do literal 565: assim o teste
  // vale em qualquer profundidade (era RGB332 no Core2; aqui e RGB565).
  const uint16_t probeBg = tv.readPixel(0, 0);
  vcr::draw(&tv, state);

  int minX = crt::W, minY = crt::H, maxX = -1, maxY = -1, outside = 0;
  for (int y = 0; y < crt::H; ++y) {
    for (int x = 0; x < crt::W; ++x) {
      if (tv.readPixel(x, y) == probeBg)
        continue;
      if (x < minX)
        minX = x;
      if (y < minY)
        minY = y;
      if (x > maxX)
        maxX = x;
      if (y > maxY)
        maxY = y;
      if (x < crt::SAFE_L || x >= crt::SAFE_R || y < crt::SAFE_T || y >= crt::SAFE_B) {
        if (outside < 8)
          printf("  FORA DA AREA SEGURA: (%d,%d)\n", x, y);
        ++outside;
      }
    }
  }
  if (maxX < 0) {
    printf("  nada desenhado\n");
    return 0;
  }
  printf("  caixa do OSD: x %d..%d  y %d..%d  (segura: x %d..%d  y %d..%d)  fora=%d\n", minX, maxX, minY,
         maxY, crt::SAFE_L, crt::SAFE_R - 1, crt::SAFE_T, crt::SAFE_B - 1, outside);
  return outside;
}

int main(int, char **) {
  panel.setScaling(3, 3);
  sim::configure(panel);
  tv.setPanel(&panel);
  if (!tv.init())
    return 1;
  tv.setColorDepth(16); // RGB565, igual ao canvas `tv` do Fruit Jam

  vcr::State playing;
  playing.transport = vcr::Transport::Play;
  playing.speed = vcr::Speed::SP;
  playing.seconds = 754; // 0:12:34
  playing.title = "CIDADE MARAVILHOSA 1993";
  playing.buttonLeft = "ANTERIOR";
  playing.buttonCenter = "PAUSA";
  playing.buttonRight = "PROXIMO";
  shot("probe_vcr_osd_play.png", playing);

  vcr::State paused = playing;
  paused.transport = vcr::Transport::Pause;
  paused.buttonCenter = "PLAY";
  paused.seconds = 3 * 3600 + 5 * 60 + 9; // 3:05:09
  shot("probe_vcr_osd_pause.png", paused);

  vcr::State longTitle = playing;
  longTitle.title = "ESPECIAL DE FIM DE ANO - GRAVADO DA TV ABERTA EM DEZEMBRO DE 1994 - FITA 02";
  longTitle.speed = vcr::Speed::EP;
  shot("probe_vcr_osd_long.png", longTitle);

  vcr::State ff = playing;
  ff.transport = vcr::Transport::Ff;
  ff.speed = vcr::Speed::LP;
  ff.title = nullptr;
  shot("probe_vcr_osd_ff.png", ff);

  vcr::State rew = playing;
  rew.transport = vcr::Transport::Rew;
  rew.title = "REBOBINANDO";
  shot("probe_vcr_osd_rew.png", rew);

  vcr::State stop = playing;
  stop.transport = vcr::Transport::Stop;
  stop.showCounter = false;
  stop.speed = vcr::Speed::None; // sem fita nao ha velocidade de gravacao
  stop.title = "SEM FITA";
  shot("probe_vcr_osd_stop.png", stop);

  // Titulo acentuado vindo de meta.json: tem que sair normalizado ("SAO
  // JOAO..."), nunca com buracos no lugar das letras acentuadas.
  vcr::State accented = playing;
  accented.title = "S\xc3\xa3o Jo\xc3\xa3o - Can\xc3\xa7\xc3\xa3o";
  shot("probe_vcr_osd_accents.png", accented);

  // Pausa com o simbolo apagado: metade do ciclo de pisca que o firmware conduz.
  vcr::State blink = paused;
  blink.blinkOn = false;
  shot("probe_vcr_osd_blink_off.png", blink);

  printf("\nconferencia da area segura:\n");
  int outside = 0;
  printf(" play:\n");
  outside += checkSafeArea(playing);
  printf(" pausa:\n");
  outside += checkSafeArea(paused);
  printf(" titulo longo:\n");
  outside += checkSafeArea(longTitle);
  printf(" stop sem contador:\n");
  outside += checkSafeArea(stop);
  // A sombra diagonal cresce para baixo e para a direita, o lado oposto ao do
  // contorno — precisa da mesma conferencia.
  printf(" titulo acentuado:\n");
  outside += checkSafeArea(accented);
  printf("\n%s\n", outside ? "FALHOU: OSD escapa da area segura" : "OK: OSD inteiro dentro da area segura");
  return outside ? 1 : 0;
}
