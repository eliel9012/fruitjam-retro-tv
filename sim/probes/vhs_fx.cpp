// ============================================================================
//  probe_vhs_fx — bancada do include/VhsFx.h
//
//  Alimenta o filtro exatamente como o firmware faz: uma imagem sintetica de
//  240x176 (barras de cor + detalhe fino, o pior caso para o sangramento de
//  croma) entregue em tiras de 128x16, que e o tamanho que o JPEGDEC usa de
//  verdade — MAX_BUFFERED_PIXELS (2048) / (16*16 de um MCU 4:2:0) = 8 MCUs,
//  ou 128 px de largura por 16 de altura (JPEGDEC.h:64, jpeg.inl:5059).
//
//  Grava PNGs dos tres niveis de desgaste, mais uma sequencia de quadros
//  durante uma rajada de tracking, e confere os invariantes baratos:
//
//    1. o PRNG e reproduzivel: mesma semente e mesmo relogio => quadro
//       identico bit a bit;
//    2. a faixa de troca de cabeca cai onde foi anunciada (ultimas linhas da
//       IMAGEM, nao do raster);
//    3. nada e escrito fora do retangulo do video no eixo vertical, e nada
//       fora do raster;
//    4. os tres niveis de desgaste produzem quadros distintos entre si.
//
//    make -C sim probes && cd sim && ./build/probe_vhs_fx
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SafeArea.h"
#include "VhsFx.h"

using namespace crt;

// Geometria do video: o mesmo formato estreito que o firmware reproduz, ja
// centralizado no quadro de 320x240 pelo jpegDraw().
static const int VW = 240, VH = 176;
static const int VX = (W - VW) / 2, VY = (H - VH) / 2;

// Tiras do JPEGDEC.
static const int STRIP_W = 128, STRIP_H = 16;

static lgfx::Panel_sdl panel;
static M5GFX rca;

// Cinza-sentinela fora da imagem: qualquer pixel diferente disto acima ou
// abaixo do video denuncia escrita fora do lugar.
static const uint16_t kSentinel = 0x2104; // cinza bem escuro, luma-only

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  [%s] %s\n", ok ? "ok " : "FALHA", what);
  if (!ok)
    ++failures;
}

// ---------------------------------------------------------------------------
//  Imagem de teste: barras de cor saturadas (para o croma ter o que borrar),
//  uma grade fina (para o tremor de linha aparecer) e um degrade de cinza.
// ---------------------------------------------------------------------------

static uint16_t source[VW * VH];

static uint16_t rgb(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void buildSource() {
  static const uint16_t bars[8] = {
      rgb(192, 192, 192), rgb(192, 192, 0), rgb(0, 192, 192), rgb(0, 192, 0),
      rgb(192, 0, 192),   rgb(192, 0, 0),   rgb(0, 0, 192),   rgb(24, 24, 24),
  };
  for (int y = 0; y < VH; ++y) {
    for (int x = 0; x < VW; ++x) {
      uint16_t c;
      if (y < VH / 2) {
        c = bars[(x * 8) / VW]; // barras de cor
      } else if (y < (VH * 3) / 4) {
        // Xadrez de 2 px: detalhe fino, o pior caso do sangramento de croma.
        c = (((x >> 1) + (y >> 1)) & 1) ? rgb(255, 255, 255) : rgb(16, 16, 16);
      } else {
        const int v = (x * 255) / (VW - 1); // degrade de luminancia
        c = rgb(v, v, v);
      }
      source[y * VW + x] = c;
    }
  }
  // Reguas verticais de 2 px a cada 24: e nelas que o tremor horizontal de
  // linha fica visivel a olho nu num PNG parado.
  for (int y = 0; y < VH; ++y)
    for (int x = 0; x < VW; x += 24) {
      source[(size_t)y * VW + x] = rgb(255, 255, 255);
      if (x + 1 < VW)
        source[(size_t)y * VW + x + 1] = rgb(0, 0, 0);
    }
  // Linha de registro no centro, para o rasgo e o deslocamento ficarem obvios.
  for (int x = 0; x < VW; ++x)
    source[(size_t)(VH / 2) * VW + x] = rgb(255, 0, 0);
}

// ---------------------------------------------------------------------------
//  Um quadro: limpa, entrega as tiras ao filtro e pinta as faixas de ruido.
//  Reproduz a ordem exata do firmware (beginFrame -> pushBlock por tira ->
//  drawOverlay).
// ---------------------------------------------------------------------------

static uint16_t strip[STRIP_W * STRIP_H];

static void renderAt(vhs::Filter &f, uint32_t nowMs, int px, int py, int pw, int ph) {
  rca.fillScreen(kSentinel);
  f.beginFrame(nowMs, px, py, pw, ph);
  for (int by = 0; by < ph; by += STRIP_H) {
    const int h = (by + STRIP_H > ph) ? ph - by : STRIP_H;
    for (int bx = 0; bx < pw; bx += STRIP_W) {
      const int w = (bx + STRIP_W > pw) ? pw - bx : STRIP_W;
      // O JPEGDEC entrega a tira num buffer proprio, reescrito a cada chamada;
      // copiar da origem aqui reproduz isso (e por isso pushBlock pode alterar
      // o buffer no lugar sem corromper o quadro seguinte).
      for (int r = 0; r < h; ++r)
        for (int x = 0; x < w; ++x)
          strip[(size_t)r * w + x] = source[(size_t)((by + r) % VH) * VW + ((bx + x) % VW)];
      f.pushBlock(&rca, px + bx, py + by, w, h, strip);
    }
  }
  f.drawOverlay(&rca);
}

static void renderFrame(vhs::Filter &f, uint32_t nowMs) { renderAt(f, nowMs, VX, VY, VW, VH); }

// ---------------------------------------------------------------------------
//  PNG
// ---------------------------------------------------------------------------

static FILE *openOut(const char *name) {
  char path[192];
  // O probe pode ser rodado de dentro de sim/ ou da raiz; o fopen e conferido
  // nos dois casos porque um fopen sem checagem aqui ja virou segfault.
  const char *dirs[2] = {"build", "sim/build"};
  for (int d = 0; d < 2; ++d) {
    snprintf(path, sizeof(path), "%s/vhs_%s.png", dirs[d], name);
    FILE *f = fopen(path, "wb");
    if (f) {
      printf("    %s\n", path);
      return f;
    }
  }
  fprintf(stderr, "    nao consegui gravar vhs_%s.png (rode de dentro de sim/)\n", name);
  ++failures;
  return nullptr;
}

static void shot(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, W, H);
  if (!png) {
    fprintf(stderr, "    createPng falhou\n");
    ++failures;
    return;
  }
  FILE *f = openOut(name);
  if (f) {
    fwrite(png, 1, len, f);
    fclose(f);
  }
  free(png);
}

// ---------------------------------------------------------------------------
//  Leitura do framebuffer para os invariantes.
// ---------------------------------------------------------------------------

static uint16_t grab[W * H];

// O Panel_sdl devolve o RGB565 com os bytes trocados (ordem de rede), ao
// contrario do que pushImage aceita. Desfazer aqui deixa grab[] comparavel com
// source[] e com kSentinel; sem isso todo pixel "parece" diferente.
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void grabFrame(uint16_t *dst) {
  for (int y = 0; y < H; ++y) {
    rca.readRect(0, y, W, 1, dst + (size_t)y * W);
    for (int x = 0; x < W; ++x)
      dst[(size_t)y * W + x] = bswap16(dst[(size_t)y * W + x]);
  }
}

static uint32_t hashFrame(const uint16_t *p) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < W * H; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

static int rowsDifferingFromSentinel(const uint16_t *p, int y0, int y1) {
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < W; ++x)
      if (p[(size_t)y * W + x] != kSentinel) {
        ++n;
        break;
      }
  return n;
}

// ---------------------------------------------------------------------------

struct Budget {
  uint32_t pushes, noiseRows, smearedPixels;
};

static Budget runWear(vhs::Wear wear, const char *name, uint32_t seed, int frames, uint32_t startMs,
                      uint32_t stepMs, const char *shotName) {
  vhs::Filter f;
  f.configure(vhs::presetFor(wear));
  f.begin(seed);
  uint32_t t = startMs;
  for (int i = 0; i < frames; ++i) {
    f.resetStats();
    renderFrame(f, t);
    t += stepMs;
  }
  if (shotName)
    shot(shotName);
  Budget b = {f.pushes(), f.noiseRows(), f.smearedPixels()};
  printf("  %-6s pushImage=%-4u linhas de ruido=%-3u px com croma=%-6u  (ultimo quadro)\n", name,
         (unsigned)b.pushes, (unsigned)b.noiseRows, (unsigned)b.smearedPixels);
  return b;
}

int main(int, char **) {
  panel.setScaling(2, 2);
  rca.setPanel(&panel);
  if (!rca.init()) {
    fprintf(stderr, "falha ao inicializar o painel SDL\n");
    return 1;
  }
  // O firmware esta em RGB565 por padrao (rca.setColorDepth(16)); o probe usa a
  // mesma profundidade para que os PNGs mostrem o ruido com a mesma quantizacao.
  // Nada em VhsFx.h depende disso: tudo sai por pushImage de rgb565_t.
  rca.setColorDepth(16);
  buildSource();

  printf("video %dx%d em (%d,%d) dentro de %dx%d; tiras de %dx%d\n\n", VW, VH, VX, VY, W, H, STRIP_W,
         STRIP_H);

  // -------------------------------------------------------------------------
  //  0. Referencia: filtro desligado.
  // -------------------------------------------------------------------------
  printf("referencia (filtro desligado)\n");
  vhs::Filter off;
  off.configure(vhs::presetFor(vhs::Wear::Off));
  off.begin(1);
  off.resetStats();
  renderFrame(off, 0);
  shot("00_desligado");
  static uint16_t clean[W * H];
  grabFrame(clean);
  const uint32_t hClean = hashFrame(clean);
  printf("  pushImage=%u (esperado %d: uma chamada por tira)\n\n", (unsigned)off.pushes(),
         ((VW + STRIP_W - 1) / STRIP_W) * ((VH + STRIP_H - 1) / STRIP_H));
  check(off.pushes() == (uint32_t)(((VW + STRIP_W - 1) / STRIP_W) * ((VH + STRIP_H - 1) / STRIP_H)),
        "desligado nao acrescenta nenhuma chamada de blit");
  check(off.noiseRows() == 0 && off.smearedPixels() == 0, "desligado nao toca em pixel nenhum");

  // -------------------------------------------------------------------------
  //  1. Os tres niveis de desgaste. 40 quadros a 42 ms (23,5 fps) para o
  //     tracking ter tempo de disparar no GASTA e no RUIM.
  // -------------------------------------------------------------------------
  printf("\nniveis de desgaste (40 quadros a 42 ms)\n");
  runWear(vhs::Wear::Nova, "NOVA", 0xC0FFEE, 40, 0, 42, "01_nova");
  grabFrame(grab);
  const uint32_t hNova = hashFrame(grab);
  runWear(vhs::Wear::Gasta, "GASTA", 0xC0FFEE, 40, 0, 42, "02_gasta");
  grabFrame(grab);
  const uint32_t hGasta = hashFrame(grab);
  runWear(vhs::Wear::Ruim, "RUIM", 0xC0FFEE, 40, 0, 42, "03_ruim");
  grabFrame(grab);
  const uint32_t hRuim = hashFrame(grab);

  printf("\ninvariantes\n");
  check(hNova != hClean && hGasta != hClean && hRuim != hClean,
        "todo nivel de desgaste altera a imagem");
  check(hNova != hGasta && hGasta != hRuim && hNova != hRuim,
        "os tres niveis produzem quadros distintos");

  // -------------------------------------------------------------------------
  //  2. Reprodutibilidade: mesma semente + mesmo relogio => quadro identico.
  // -------------------------------------------------------------------------
  {
    vhs::Filter a, b;
    a.configure(vhs::presetFor(vhs::Wear::Ruim));
    b.configure(vhs::presetFor(vhs::Wear::Ruim));
    a.begin(0xBEEF0001);
    b.begin(0xBEEF0001);
    uint32_t ha = 0, hb = 0;
    for (int i = 0; i < 25; ++i)
      renderFrame(a, (uint32_t)i * 42);
    grabFrame(grab);
    ha = hashFrame(grab);
    for (int i = 0; i < 25; ++i)
      renderFrame(b, (uint32_t)i * 42);
    grabFrame(grab);
    hb = hashFrame(grab);
    check(ha == hb && a.rngState() == b.rngState(), "PRNG reproduzivel a partir da semente");

    vhs::Filter c;
    c.configure(vhs::presetFor(vhs::Wear::Ruim));
    c.begin(0xBEEF0002);
    for (int i = 0; i < 25; ++i)
      renderFrame(c, (uint32_t)i * 42);
    grabFrame(grab);
    check(hashFrame(grab) != ha, "semente diferente produz execucao diferente");
  }

  // -------------------------------------------------------------------------
  //  3. A faixa de troca de cabeca cai nas ultimas linhas da IMAGEM.
  // -------------------------------------------------------------------------
  {
    vhs::Filter f;
    vhs::Config cfg = vhs::presetFor(vhs::Wear::Ruim);
    cfg.tracking = false; // isola a troca de cabeca
    cfg.jitter = false;
    cfg.chroma = false;
    f.configure(cfg);
    f.begin(0x5EED);
    renderFrame(f, 0);
    shot("04_head_switch");
    const int top = f.headNoiseTop(), rows = f.headNoiseRows();
    printf("  faixa de troca de cabeca: linhas %d..%d (%d linhas); base da imagem = %d\n", top,
           top + rows - 1, rows, VY + VH - 1);
    check(rows > 0 && top + rows == VY + VH, "a faixa termina exatamente na ultima linha do video");
    check(rows <= vhs::limits::kMaxHeadRows, "a faixa respeita kMaxHeadRows");
    check(f.noiseRows() == (uint32_t)rows, "uma linha de ruido por linha anunciada");

    grabFrame(grab);
    // Acima da faixa de rasgo a imagem tem de estar intacta: sem jitter, sem
    // croma e sem tracking, nada mais pode ter mexido nela.
    int dirty = 0;
    for (int y = VY; y < top - 6; ++y)
      for (int x = 0; x < VW; ++x)
        if (grab[(size_t)y * W + VX + x] != source[(size_t)(y - VY) * VW + x])
          ++dirty;
    check(dirty == 0, "so a faixa de troca de cabeca foi alterada");

    // Nada acima nem abaixo do retangulo do video.
    check(rowsDifferingFromSentinel(grab, 0, VY) == 0, "nada escrito acima do video");
    check(rowsDifferingFromSentinel(grab, VY + VH, H) == 0, "nada escrito abaixo do video");
  }

  // -------------------------------------------------------------------------
  //  4. Tracking: a faixa rola, fica dentro do raster e some no fim da rajada.
  // -------------------------------------------------------------------------
  {
    vhs::Filter f;
    vhs::Config cfg = vhs::presetFor(vhs::Wear::Gasta);
    cfg.trackingGapMs = 200; // dispara logo, para o probe nao esperar 8 s
    f.configure(cfg);
    f.begin(0x7A9C);
    int active = 0, outOfRange = 0, shots = 0, lastTop = -9999, rolled = 0;
    char name[32];
    for (int i = 0; i < 60; ++i) {
      renderFrame(f, (uint32_t)i * 42);
      if (f.trackingRows() > 0) {
        ++active;
        const int t = f.trackingTop();
        if (t < 0 || t + f.trackingRows() > H)
          ++outOfRange;
        if (lastTop != -9999 && t < lastTop)
          ++rolled;
        lastTop = t;
        if (shots < 3 && (active % 4) == 1) {
          snprintf(name, sizeof(name), "05_tracking_%d", shots);
          shot(name);
          ++shots;
        }
      }
      grabFrame(grab);
      if (rowsDifferingFromSentinel(grab, 0, VY) || rowsDifferingFromSentinel(grab, VY + VH, H))
        ++outOfRange;
    }
    printf("  quadros com rajada de tracking: %d de 60; faixa subiu em %d deles\n", active, rolled);
    check(active > 0, "a rajada de tracking dispara");
    check(active < 60, "a rajada de tracking termina (nao fica presa ligada)");
    check(rolled > 0, "a faixa de tracking rola pela imagem");
    check(outOfRange == 0, "a faixa de tracking nunca sai do raster nem do retangulo do video");
  }

  // -------------------------------------------------------------------------
  //  5. Deslocamento de linha: dentro do limite, e o recorte do LovyanGFX
  //     impede escrita fora do raster mesmo com a imagem colada na borda.
  // -------------------------------------------------------------------------
  {
    vhs::Filter f;
    f.configure(vhs::presetFor(vhs::Wear::Ruim));
    f.begin(0x1111);
    int worst = 0, shifted = 0, outside = 0;
    for (int i = 0; i < 30; ++i) {
      f.beginFrame((uint32_t)i * 42, VX, VY, VW, VH);
      for (int y = 0; y < H; ++y) {
        const int s = f.lineShift(y);
        if (s > worst)
          worst = s;
        if (-s > worst)
          worst = -s;
        if (s)
          ++shifted;
        if (s && (y < VY || y >= VY + VH))
          ++outside;
      }
    }
    check(outside == 0, "nenhuma linha fora do retangulo do video e deslocada");
    printf("  deslocamento maximo observado: %d px (limite %d); %d linhas deslocadas em 30 quadros\n",
           worst, vhs::limits::kMaxShift, shifted);
    check(worst <= vhs::limits::kMaxShift, "o deslocamento respeita kMaxShift");
    check(shifted > 0, "ha linhas deslocadas");

    // Video ocupando o raster inteiro: o deslocamento agora empurra pixels para
    // fora dos dois lados e tem de ser aparado sem estourar nada.
    vhs::Filter g;
    g.configure(vhs::presetFor(vhs::Wear::Ruim));
    g.begin(0x2222);
    renderAt(g, 0, 0, 0, W, H);
    shot("06_tela_cheia");
    printf("  tela cheia com deslocamento: sem estouro (o recorte do LovyanGFX aparou as bordas)\n");
  }

  // -------------------------------------------------------------------------
  //  6. Croma isolado: os tres niveis tocam 1/4, 1/2 e todas as linhas.
  // -------------------------------------------------------------------------
  printf("\nsangramento de croma isolado\n");
  for (int lvl = 1; lvl <= 3; ++lvl) {
    vhs::Filter f;
    vhs::Config cfg;
    cfg.enabled = true;
    cfg.headSwitch = cfg.jitter = cfg.tracking = false;
    cfg.chroma = true;
    cfg.chromaLevel = (uint8_t)lvl;
    f.configure(cfg);
    f.begin(0x3333);
    f.resetStats();
    renderFrame(f, 0);
    char name[24];
    snprintf(name, sizeof(name), "07_croma_n%d", lvl);
    shot(name);
    const uint32_t total = (uint32_t)VW * VH;
    const int step = lvl >= 3 ? 1 : (lvl == 2 ? 2 : 4);
    printf("  nivel %d: %u px de %u (1 linha a cada %d)\n", lvl, (unsigned)f.smearedPixels(),
           (unsigned)total, step);
    check(f.smearedPixels() > 0 && f.smearedPixels() <= total, "o croma toca uma fracao valida");
    check(f.pushes() == (uint32_t)(((VW + STRIP_W - 1) / STRIP_W) * ((VH + STRIP_H - 1) / STRIP_H)),
          "o croma nao acrescenta chamada de blit");
  }

  // -------------------------------------------------------------------------
  //  7. Custo: converte os contadores no orcamento em milissegundos, com as
  //     constantes medidas no aparelho.
  // -------------------------------------------------------------------------
  printf("\norcamento estimado por quadro (blit medido: 12,3 ms / 76800 px = 0,160 us/px;\n"
         "setup de pushImage estimado em 6 us; croma em 14 ciclos/px a 240 MHz;\n"
         "geracao de uma linha de ruido em ~7 us)\n");
  static const vhs::Wear wears[3] = {vhs::Wear::Nova, vhs::Wear::Gasta, vhs::Wear::Ruim};
  static const char *names[3] = {"NOVA", "GASTA", "RUIM"};
  for (int fmt = 0; fmt < 2; ++fmt) {
    const int pw = fmt ? W : VW, ph = fmt ? H : VH;
    const int pxx = fmt ? 0 : VX, pyy = fmt ? 0 : VY;
    const int baseline = ((pw + STRIP_W - 1) / STRIP_W) * ((ph + STRIP_H - 1) / STRIP_H);
    printf("  --- video %dx%d (%d tiras de MCU por quadro) ---\n", pw, ph, baseline);
    for (int i = 0; i < 3; ++i) {
      // Pior caso ao longo de 120 quadros (5 s), pegando o maximo de cada
      // contador — inclusive um quadro em plena rajada de tracking.
      vhs::Filter f;
      f.configure(vhs::presetFor(wears[i]));
      f.begin(0xC0FFEE);
      uint32_t maxPush = 0, maxNoise = 0, maxSmear = 0;
      for (int k = 0; k < 120; ++k) {
        f.resetStats();
        renderAt(f, (uint32_t)k * 42, pxx, pyy, pw, ph);
        if (f.pushes() > maxPush)
          maxPush = f.pushes();
        if (f.noiseRows() > maxNoise)
          maxNoise = f.noiseRows();
        if (f.smearedPixels() > maxSmear)
          maxSmear = f.smearedPixels();
      }
      const double extraCalls = (double)(maxPush - baseline - maxNoise) * 6.0 / 1000.0;
      const double noiseMs = maxNoise * (pw * 0.160 + 7.0 + 6.0) / 1000.0;
      const double chromaMs = maxSmear * 14.0 / 240000.0;
      const double total = extraCalls + noiseMs + chromaMs;
      printf("  %-6s jitter/rasgo %.2f ms + ruido %.2f ms + croma %.2f ms = %.2f ms"
             "  (23,5 fps -> %.1f fps)\n",
             names[i], extraCalls, noiseMs, chromaMs, total, 1000.0 / (1000.0 / 23.5 + total));
    }
  }

  printf("\n%s (%d falha(s))\n", failures ? "COM FALHAS" : "TUDO OK", failures);
  return failures ? 1 : 0;
}
