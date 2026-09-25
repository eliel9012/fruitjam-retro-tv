// ============================================================================
//  Probe da abertura (include/BootSplash.h).
//
//  Pinta a abertura com o nome do dono, um nome longo (para conferir a escala
//  automática) e um com acento (para conferir a normalização), grava PNGs em
//  sim/build/ e confere por pixel que nada além do fundo sai da área segura.
//
//    make -C sim probes && (cd sim && ./build/probe_boot_splash)
// ============================================================================

#include "fj/Gfx.h"
#include "SimPanel.h"

#include <cstdio>
#include <cstdlib>

#include "BootSplash.h"

static lgfx::Panel_sdl panel;
static lgfx::LGFX_Device tv;

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
  if (!f) {
    // Rodado fora de sim/ o caminho relativo não existe (AGENTS.md, armadilha 14).
    printf("nao abriu %s: rode de dentro de sim/\n", path);
    free(png);
    return;
  }
  fwrite(png, 1, len, f);
  fclose(f);
  free(png);
  printf("gravado %s (%zu bytes)\n", path, len);
}

// Conta pixels fora da caixa segura que não são o fundo em degradê.
static int inkOutsideSafe() {
  int outside = 0;
  for (int y = 0; y < crt::H; ++y) {
    const uint16_t bg = bootsplash::backgroundRow(y);
    for (int x = 0; x < crt::W; ++x) {
      if (x >= crt::SAFE_L && x < crt::SAFE_R && y >= crt::SAFE_T && y < crt::SAFE_B)
        continue;
      if (tv.readPixel(x, y) != bg)
        ++outside;
    }
  }
  return outside;
}

static int check(const char *png, const char *owner) {
  bootsplash::draw(&tv, owner, "FRUIT JAM RETRO TV", "VERIFICANDO CARTAO SD", "v0.1");
  savePng(png);
  const int out = inkOutsideSafe();
  printf("%-28s escala %d, tinta fora da area segura: %d\n", owner, bootsplash::fitScale(owner), out);
  return out;
}

int main(int, char **) {
  panel.setScaling(3, 3);
  sim::configure(panel);
  tv.setPanel(&panel);
  if (!tv.init())
    return 1;
  tv.setColorDepth(16); // RGB565, igual ao canvas `tv` do Fruit Jam

  int falhas = 0;
  falhas += check("probe_boot_splash.png", "ELIEL") != 0;
  falhas += check("probe_boot_splash_longo.png", "UM NOME BEM MAIS COMPRIDO") != 0;
  falhas += check("probe_boot_splash_acento.png", "Joao Conceicao") != 0;
  // O status é repintado sozinho a cada etapa do boot.
  bootsplash::draw(&tv, "ELIEL", "FRUIT JAM RETRO TV", "VERIFICANDO CARTAO SD", "v0.1");
  bootsplash::drawStatus(&tv, "Sistema pronto");
  savePng("probe_boot_splash_pronto.png");
  falhas += inkOutsideSafe() != 0;
  printf(falhas ? "FALHOU: %d cenas\n" : "ok: abertura dentro da area segura\n", falhas);
  return falhas ? 1 : 0;
}
