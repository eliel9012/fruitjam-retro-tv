// ============================================================================
//  Bancada da carta de ajuste (include/TestPattern.h).
//
//  Grava os PNGs (barras SMPTE, chuvisco, cartaz, sem-sinal) e confere, por
//  pixel e por amostra, o que uma inspecao visual nao garante:
//
//    * a cor exata de cada uma das 7 barras, das 7 do meio e dos 6 blocos da
//      linha de baixo, lida de volta do framebuffer;
//    * as proporcoes 67 / 8 / 25 e as bordas das barras em 0..320;
//    * que o TEXTO (o unico elemento que nao pode sangrar) cabe na area segura,
//      mesmo com as barras ocupando o quadro inteiro de proposito;
//    * que o cartaz inteiro cabe na area segura;
//    * o tom de 1 kHz: frequencia por contagem de cruzamentos de zero em 1 s,
//      ausencia de offset DC, ausencia de corte e continuidade de fase entre
//      duas chamadas seguidas de fillPcm().
//
//    make -C sim probes && ./sim/build/probe_test_pattern
//
//  Sai com 1 se qualquer conferencia falhar, para servir de porta em CI.
//
//  A regra de probe do Makefile nao depende dos headers: depois de mexer em
//  include/TestPattern.h, um `touch sim/probes/test_pattern.cpp` antes do make.
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "SafeArea.h"
#include "TestPattern.h"

namespace tp = testpattern;

static lgfx::Panel_sdl panel;
static M5GFX rca;
static int failures = 0;

static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("  FALHA: ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  ++failures;
}

static void savePng(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, crt::W, crt::H);
  if (!png) {
    fail("nao gerou %s", name);
    return;
  }
  char path[256];
  snprintf(path, sizeof(path), "build/%s", name);
  FILE *f = fopen(path, "wb");
  fwrite(png, 1, len, f);
  fclose(f);
  free(png);
  printf("  gravado build/%s (%zu bytes)\n", name, len);
}

static void expectPixel(const char *what, int x, int y, uint16_t want) {
  const uint16_t got = (uint16_t)rca.readPixel(x, y);
  if (got != want)
    fail("%s em (%d,%d): 0x%04X, esperado 0x%04X", what, x, y, got, want);
}

// ---------------------------------------------------------------------------
//  Barras
// ---------------------------------------------------------------------------
static void checkBars() {
  printf("\n[barras SMPTE]\n");
  tp::drawBars(&rca, 0, 0);
  savePng("probe_test_pattern_bars.png");

  const uint16_t top[7] = {tp::GRAY75, tp::YELLOW75, tp::CYAN75,  tp::GREEN75,
                           tp::MAGENTA75, tp::RED75, tp::BLUE75};
  const uint16_t mid[7] = {tp::BLUE75, tp::BLACK, tp::MAGENTA75, tp::BLACK,
                           tp::CYAN75, tp::BLACK, tp::GRAY75};
  for (int i = 0; i < 7; ++i) {
    const int cx = (tp::barX(i) + tp::barX(i + 1)) / 2;
    expectPixel("barra de cima", cx, tp::BARS_H / 2, top[i]);
    expectPixel("barra do meio", cx, tp::MID_Y + tp::MID_H / 2, mid[i]);
  }

  // Linha de baixo, longe do rotulo (y = 185, o texto comeca em 194).
  const int yb = tp::BOT_Y + 5;
  expectPixel("-I", (tp::barX(0) + tp::barX(1)) / 2, yb, tp::MINUS_I);
  expectPixel("branco 100%", (tp::barX(1) + tp::barX(2)) / 2, yb, tp::WHITE100);
  expectPixel("+Q", (tp::barX(2) + tp::barX(3)) / 2, yb, tp::PLUS_Q);
  expectPixel("preto", (tp::barX(3) + tp::barX(5)) / 2, yb, tp::BLACK);
  const int p0 = tp::barX(5), p3 = tp::barX(6);
  const int p1 = p0 + (p3 - p0) / 3, p2 = p0 + ((p3 - p0) * 2) / 3;
  expectPixel("pluge -4 IRE", (p0 + p1) / 2, yb, tp::PLUGE_MINUS);
  expectPixel("pluge 0 IRE", (p1 + p2) / 2, yb, tp::BLACK);
  expectPixel("pluge +4 IRE", (p2 + p3) / 2, yb, tp::PLUGE_PLUS);
  expectPixel("preto final", (tp::barX(6) + tp::barX(7)) / 2, yb, tp::BLACK);

  // Bordas verticais das barras, varrendo uma linha do topo.
  int edges[16], n = 0;
  for (int x = 1; x < crt::W; ++x)
    if (rca.readPixel(x, 80) != rca.readPixel(x - 1, 80) && n < 16)
      edges[n++] = x;
  printf("  bordas das barras:");
  for (int i = 0; i < n; ++i)
    printf(" %d", edges[i]);
  printf("   (esperado 45 91 137 182 228 274)\n");
  const int want[6] = {45, 91, 137, 182, 228, 274};
  if (n != 6)
    fail("achou %d bordas, esperava 6", n);
  else
    for (int i = 0; i < 6; ++i)
      if (edges[i] != want[i])
        fail("borda %d em %d, esperado %d", i, edges[i], want[i]);

  // Proporcoes 67 / 8 / 25 lidas na coluna da primeira barra.
  int t1 = -1, t2 = -1;
  for (int y = 1; y < crt::H; ++y) {
    if (rca.readPixel(10, y) != rca.readPixel(10, y - 1)) {
      if (t1 < 0)
        t1 = y;
      else if (t2 < 0)
        t2 = y;
    }
  }
  printf("  faixas: topo 0..%d (%d px, %.1f%%), meio %d..%d (%d px, %.1f%%), base %d..240 (%d px, %.1f%%)\n",
         t1 - 1, t1, 100.0 * t1 / crt::H, t1, t2 - 1, t2 - t1, 100.0 * (t2 - t1) / crt::H, t2,
         crt::H - t2, 100.0 * (crt::H - t2) / crt::H);
  if (t1 != tp::BARS_H || t2 != tp::BOT_Y)
    fail("faixas em %d/%d, esperado %d/%d", t1, t2, tp::BARS_H, tp::BOT_Y);

  // O texto e o UNICO elemento que nao pode sangrar. O resto da carta ocupa o
  // quadro inteiro de proposito, entao a conferencia olha so a regiao preta da
  // linha de baixo: qualquer pixel nao-preto ali e tinta de rotulo.
  int minX = crt::W, minY = crt::H, maxX = -1, maxY = -1;
  for (int y = tp::BOT_Y; y < crt::H; ++y) {
    for (int x = tp::LABEL_X; x < tp::LABEL_X + tp::LABEL_W; ++x) {
      if ((uint16_t)rca.readPixel(x, y) == tp::BLACK)
        continue;
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
  }
  if (maxX < 0) {
    fail("rotulo nao foi desenhado");
  } else {
    printf("  rotulo: x %d..%d  y %d..%d  (area segura x %d..%d  y %d..%d)\n", minX, maxX, minY, maxY,
           crt::SAFE_L, crt::SAFE_R - 1, crt::SAFE_T, crt::SAFE_B - 1);
    if (minX < crt::SAFE_L || maxX >= crt::SAFE_R || minY < crt::SAFE_T || maxY >= crt::SAFE_B)
      fail("rotulo escapou da area segura");
  }

  // Carta limpa, sem rotulo: a regiao acima tem de ficar toda preta.
  tp::drawBarsLabeled(&rca, 0, 0, nullptr, nullptr);
  savePng("probe_test_pattern_bars_limpa.png");
  for (int y = tp::BOT_Y; y < crt::H; ++y)
    for (int x = tp::LABEL_X; x < tp::LABEL_X + tp::LABEL_W; ++x)
      if ((uint16_t)rca.readPixel(x, y) != tp::BLACK) {
        fail("carta sem rotulo ainda tem tinta em (%d,%d)", x, y);
        y = crt::H;
        break;
      }
}

// ---------------------------------------------------------------------------
//  Sem sinal
// ---------------------------------------------------------------------------
static void checkNoSignal() {
  printf("\n[sem sinal]\n");

  tp::sharedSnow().reset();
  tp::drawSnow(&rca, 0, 0);
  savePng("probe_test_pattern_snow.png");
  // Chuvisco tem de ser ruido, nao um campo chapado.
  long sum = 0;
  int distinct = 0;
  uint8_t seen[64] = {0};
  for (int y = 0; y < crt::H; y += 8)
    for (int x = 0; x < crt::W; x += 8) {
      const uint16_t v = (uint16_t)rca.readPixel(x, y);
      const int lum = (v >> 11) & 0x1F;
      sum += lum;
      if (!seen[lum]) {
        seen[lum] = 1;
        ++distinct;
      }
    }
  const double mean = sum / (double)(40 * 30);
  printf("  chuvisco: %d niveis distintos de luma (0..31), media %.1f\n", distinct, mean);
  if (distinct < 20)
    fail("chuvisco pouco aleatorio (%d niveis)", distinct);
  if (mean < 10.0 || mean > 21.0)
    fail("media do chuvisco fora do centro: %.1f", mean);

  // Dois quadros seguidos precisam diferir (senao a tela fica congelada).
  const uint16_t before = (uint16_t)rca.readPixel(0, 0);
  int changed = 0;
  tp::drawSnow(&rca, 0, 0);
  for (int y = 0; y < crt::H; y += 8)
    for (int x = 0; x < crt::W; x += 8)
      if ((uint16_t)rca.readPixel(x, y) != before)
        ++changed;
  if (!changed)
    fail("segundo quadro de chuvisco identico ao primeiro");

  tp::drawSlate(&rca, 0, 0);
  savePng("probe_test_pattern_slate.png");
  int minX = crt::W, minY = crt::H, maxX = -1, maxY = -1;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x) {
      if ((uint16_t)rca.readPixel(x, y) == tp::BLACK)
        continue;
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
  printf("  cartaz: x %d..%d  y %d..%d  (area segura x %d..%d  y %d..%d)\n", minX, maxX, minY, maxY,
         crt::SAFE_L, crt::SAFE_R - 1, crt::SAFE_T, crt::SAFE_B - 1);
  if (maxX < 0)
    fail("cartaz vazio");
  else if (minX < crt::SAFE_L || maxX >= crt::SAFE_R || minY < crt::SAFE_T || maxY >= crt::SAFE_B)
    fail("cartaz escapou da area segura");

  tp::drawSlateText(&rca, 0, 0, "FIM DA TRANSMISSAO", "SEM CARTAO SD");
  savePng("probe_test_pattern_slate_sd.png");

  tp::drawNoSignal(&rca, 0, 0);
  savePng("probe_test_pattern_nosignal.png");
}

// ---------------------------------------------------------------------------
//  Tom de 1 kHz
// ---------------------------------------------------------------------------
static void checkTone() {
  printf("\n[tom de 1 kHz]\n");
  printf("  tabela: %d amostras, %d bytes (%d ciclos exatos a %u Hz)\n", tp::TONE_TABLE_LEN,
         tp::TONE_TABLE_BYTES, 20, (unsigned)tp::SAMPLE_RATE);

  const int16_t *t = tp::toneTable();
  long dc = 0;
  int peak = 0;
  for (int i = 0; i < tp::TONE_TABLE_LEN; ++i) {
    dc += t[i];
    const int a = t[i] < 0 ? -t[i] : t[i];
    if (a > peak)
      peak = a;
  }
  printf("  tabela crua: soma (DC) = %ld, pico = %d\n", dc, peak);
  if (dc != 0)
    fail("tabela com offset DC de %ld", dc);
  if (peak > 32767)
    fail("tabela estoura int16 (%d)", peak);

  // Um segundo de PCM estereo pelo caminho publico.
  tp::Tone1k tone;
  tone.setAmplitude(tp::AMP_MINUS20DB);
  const size_t frames = tp::SAMPLE_RATE;
  std::vector<int16_t> pcm(frames * 2, 0);

  // Em dois blocos, de propósito: e assim que o audioTask consome (AUDIO_CHUNK
  // por vez), e e o que prova que a fase emenda entre chamadas.
  const size_t cut = 4096; // valores int16, par
  size_t w1 = tone.fillPcm(pcm.data(), cut);
  size_t w2 = tone.fillPcm(pcm.data() + w1, pcm.size() - w1);
  if (w1 + w2 != pcm.size())
    fail("fillPcm devolveu %zu+%zu, esperado %zu", w1, w2, pcm.size());

  long sum = 0;
  int maxAbs = 0, crossings = 0, mism = 0;
  for (size_t i = 0; i < frames; ++i) {
    const int16_t l = pcm[i * 2], r = pcm[i * 2 + 1];
    if (l != r && mism < 4) {
      fail("canais diferentes no quadro %zu: %d != %d", i, l, r);
      ++mism;
    }
    // Referencia independente: tabela indexada pela propria fase esperada.
    // /256, nao >>8: o deslocamento arredonda para -infinito e injetava -218
    // de DC por periodo (-10900 em 1 s). O fillPcm() foi corrigido para dividir;
    // esta referencia tem de dividir igual, senao acusa erro de 1 LSB em toda
    // amostra negativa.
    const int16_t ref = (int16_t)(((int32_t)t[i % tp::TONE_TABLE_LEN] * tp::AMP_MINUS20DB) / 256);
    if (l != ref && mism < 4) {
      fail("quadro %zu: %d, esperado %d (fase quebrou entre blocos?)", i, l, ref);
      ++mism;
    }
    sum += l;
    const int a = l < 0 ? -l : l;
    if (a > maxAbs)
      maxAbs = a;
    // Ciclico: o material tem 50 periodos INTEIROS de tabela, entao a amostra
    // anterior a de indice 0 e a ultima. Pular i==0 perdia um cruzamento e
    // media 999 Hz num tom que e exatamente 1000.
    const int16_t prev = i ? pcm[(i - 1) * 2] : pcm[(frames - 1) * 2];
    if (prev < 0 && l >= 0)
      ++crossings; // cruzamento de zero subindo = um ciclo
  }
  const double freq = crossings; // exatamente 1 s de material
  printf("  1 s de PCM: %d cruzamentos de zero subindo -> %.3f Hz\n", crossings, freq);
  printf("  DC = %ld (media %.4f LSB), pico = %d de 32767 (%.2f dBFS)\n", sum, sum / (double)frames,
         maxAbs, 20.0 * log10(maxAbs / 32767.0));
  if (crossings != 1000)
    fail("frequencia medida %d Hz, esperado 1000 Hz", crossings);
  if (sum != 0)
    fail("offset DC de %ld em 1 s", sum);
  if (maxAbs >= 32767)
    fail("tom cortando: pico %d", maxAbs);

  // Amplitude cheia tambem nao pode cortar.
  tp::Tone1k loud;
  loud.setAmplitude(tp::AMP_FULL);
  std::vector<int16_t> hot(tp::TONE_TABLE_LEN * 2, 0);
  loud.fillPcm(hot.data(), hot.size());
  int hotPeak = 0;
  for (size_t i = 0; i < hot.size(); ++i) {
    const int a = hot[i] < 0 ? -hot[i] : hot[i];
    if (a > hotPeak)
      hotPeak = a;
  }
  printf("  amplitude cheia (%d/256): pico %d (%.2f dBFS)\n", tp::AMP_FULL, hotPeak,
         20.0 * log10(hotPeak / 32767.0));
  if (hotPeak >= 32767)
    fail("amplitude cheia corta: %d", hotPeak);

  // Primeiras amostras, para inspecao a olho.
  printf("  primeiras 16 amostras (-20 dBFS):");
  for (int i = 0; i < 16; ++i)
    printf(" %d", pcm[i * 2]);
  printf("\n");
}

int main(int, char **) {
  panel.setScaling(2, 2);
  rca.setPanel(&panel);
  if (!rca.init())
    return 1;
  rca.setColorDepth(16); // RGB565, igual ao rca.setColorDepth(16) do main.cpp

  checkBars();
  checkNoSignal();
  checkTone();

  printf("\n%s (%d falha%s)\n", failures ? "REPROVADO" : "APROVADO", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
