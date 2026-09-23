// ============================================================================
//  probe_audio_scope — bancada do include/AudioScope.h
//
//  Alimenta o analisador com sinais SINTETICOS de resposta conhecida e afirma
//  que ele acerta. Nada aqui depende de tocar musica de verdade:
//
//    1. orcamento de memoria (sizeof real de Tap e Scope)
//    2. tabelas: soma da janela de Hann, coeficientes dentro de int16_t
//    3. ponto fixo: a divisao nao injeta DC; o `>>` injetaria (medido)
//    4. silencio -> mostrador totalmente plano
//    5. tom de 1 kHz de fundo de escala -> energia na banda 5 e em mais
//       nenhuma (tabela em dB de todas as 12)
//    6. dois tons -> dois picos, nas bandas certas
//    7. fundo de escala e onda quadrada -> SEM estouro: o resultado em int32
//       bate bit a bit com uma referencia em int64
//    8. canal produtor/consumidor: publica, copia, sequencia anda, bloco curto
//       nao publica, e o consumidor sem dado novo conta `starved`
//    9. repintura incremental: quantos pixels cada modo escreve por
//       atualizacao, comparado com repintar o painel inteiro
//
//  E grava PNGs dos tres modos para olhar.
//
//    make -C sim probes && cd sim && ./build/probe_audio_scope
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "AudioScope.h"
#include "SafeArea.h"

using namespace crt;
namespace as = audioscope;

static lgfx::Panel_sdl panel;
static M5GFX rca;

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const char *what) {
  ++g_checks;
  printf("  [%s] %s\n", ok ? " ok " : "FALHA", what);
  if (!ok)
    ++g_fail;
}

// ---------------------------------------------------------------------------
//  Sinais sinteticos
// ---------------------------------------------------------------------------

static int16_t sat(double v) {
  if (v > 32767.0)
    return 32767;
  if (v < -32768.0)
    return -32768;
  return (int16_t)llround(v);
}

// Enche uma janela mono com um seno.
static void tone(int16_t *dst, double hz, double amp, double phase = 0.0) {
  for (int n = 0; n < as::N; ++n)
    dst[n] = sat(amp * sin(2.0 * M_PI * hz * n / (double)as::SAMPLE_RATE + phase));
}

static void toneAdd(int16_t *dst, double hz, double amp, double phase = 0.0) {
  for (int n = 0; n < as::N; ++n)
    dst[n] = sat((double)dst[n] + amp * sin(2.0 * M_PI * hz * n / (double)as::SAMPLE_RATE + phase));
}

// Onda quadrada de fundo de escala: o pior caso de energia por amostra.
static void square(int16_t *dst, double hz) {
  for (int n = 0; n < as::N; ++n)
    dst[n] = (sin(2.0 * M_PI * hz * n / (double)as::SAMPLE_RATE) >= 0.0) ? 32767 : -32768;
}

// PCM estereo intercalado, para exercitar o produtor de verdade.
static void stereoTone(int16_t *dst, int frames, double hzL, double hzR, double amp) {
  for (int n = 0; n < frames; ++n) {
    dst[2 * n] = sat(amp * sin(2.0 * M_PI * hzL * n / (double)as::SAMPLE_RATE));
    dst[2 * n + 1] = sat(amp * sin(2.0 * M_PI * hzR * n / (double)as::SAMPLE_RATE));
  }
}

// ---------------------------------------------------------------------------
//  Referencia: a MESMA recursao, mas com estado em int64_t. Se o estado em
//  int32_t do header estourasse, os dois resultados divergiriam.
// ---------------------------------------------------------------------------
static long long goertzelRef(const int16_t *windowed, int32_t coef, long long *maxState) {
  long long s1 = 0, s2 = 0, mx = 0;
  for (int n = 0; n < as::N; ++n) {
    const long long s0 = (long long)windowed[n] + (coef * s1) / 16384 - s2;
    s2 = s1;
    s1 = s0;
    const long long a = s0 < 0 ? -s0 : s0;
    if (a > mx)
      mx = a;
  }
  if (maxState && mx > *maxState)
    *maxState = mx;
  const long long cs = (coef * s1) / 16384;
  const long long p = s1 * s1 + s2 * s2 - cs * s2;
  return p < 0 ? 0 : p;
}

static double db(long long p, long long ref) {
  if (p <= 0)
    return -999.0;
  return 10.0 * log10((double)p / (double)ref);
}

// ---------------------------------------------------------------------------
//  PNG
// ---------------------------------------------------------------------------

static FILE *openOut(const char *name) {
  char path[192];
  // Funciona rodando de dentro de sim/ ou da raiz do repositorio. O fopen e
  // CONFERIDO: varios probes daqui ja quebraram por escrever em caminho
  // relativo inexistente.
  const char *dirs[2] = {"build", "sim/build"};
  for (int d = 0; d < 2; ++d) {
    snprintf(path, sizeof(path), "%s/scope_%s.png", dirs[d], name);
    FILE *f = fopen(path, "wb");
    if (f) {
      printf("    %s\n", path);
      return f;
    }
  }
  fprintf(stderr, "    NAO consegui abrir build/scope_%s.png (rode de dentro de sim/)\n", name);
  return nullptr;
}

static void shot(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, W, H);
  if (!png) {
    fprintf(stderr, "    createPng falhou\n");
    return;
  }
  FILE *f = openOut(name);
  if (f) {
    fwrite(png, 1, len, f);
    fclose(f);
  }
  free(png);
}

// Moldura no espirito da MUSIC_NOW_PLAYING, so para o PNG ter contexto.
static void backdrop(const char *sub) {
  rca.fillScreen(TFT_NAVY);
  rca.setTextDatum(top_left);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.setTextSize(2);
  rca.drawString("MUSICA", SAFE_L, HEAD_Y);
  rca.setTextSize(1);
  rca.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, as::ACCENT);
  rca.setTextColor(TFT_YELLOW, TFT_NAVY);
  rca.drawString("SINAL DE TESTE", SAFE_L, BODY_Y + 8);
  rca.setTextColor(as::ACCENT, TFT_NAVY);
  rca.drawString(sub, SAFE_L, BODY_Y + 26);
  // Area segura, so no PNG, para conferir que o painel nao escapa.
  rca.drawRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, TFT_DARKGREY);
}

// ===========================================================================
int main(int, char **) {
  panel.setScaling(2, 2);
  rca.setPanel(&panel);
  if (!rca.init()) {
    fprintf(stderr, "falha ao inicializar o painel SDL\n");
    return 1;
  }
  rca.setColorDepth(8); // RGB332, igual ao firmware na saida composta

  // -----------------------------------------------------------------------
  printf("\n1. ORCAMENTO DE MEMORIA (SRAM)\n");
  // -----------------------------------------------------------------------
  const size_t tapB = sizeof(as::Tap), scopeB = sizeof(as::Scope);
  printf("   sizeof(audioscope::Tap)   = %5zu B  (banco duplo 2x%dx int16 + picos + 2 atomicos)\n", tapB,
         as::N);
  printf("   sizeof(audioscope::Scope) = %5zu B  (work + prevTop/prevBot + niveis + controle)\n", scopeB);
  printf("   TOTAL                     = %5zu B\n", tapB + scopeB);
  printf("   flash (.rodata): Hann %d B + coeficientes %d B = %d B\n", as::N * 2, as::BANDS * 2,
         as::N * 2 + as::BANDS * 2);
  check(tapB + scopeB < 2048, "estado total abaixo de 2 KB de SRAM");
  check(tapB + scopeB < 38u * 1024u / 20u, "estado abaixo de 5% do menor heap livre medido (28 KB)");

  // -----------------------------------------------------------------------
  printf("\n2. TABELAS\n");
  // -----------------------------------------------------------------------
  {
    const int16_t *h = as::detail::hann();
    long sum = 0;
    int16_t mn = 32767, mx = -32768;
    for (int n = 0; n < as::N; ++n) {
      sum += h[n];
      if (h[n] < mn)
        mn = h[n];
      if (h[n] > mx)
        mx = h[n];
    }
    printf("   Hann: soma=%ld (=%0.4f em ganho coerente, esperado %0.1f)  min=%d  max=%d\n", sum,
           sum / 32768.0, as::N / 2.0, mn, mx);
    check(fabs(sum / 32768.0 - as::N / 2.0) < 0.01, "ganho coerente da janela de Hann = N/2");
    check(mn >= 0 && mx <= 32767, "janela sem valor negativo e sem estourar Q15");

    const int16_t *c = as::detail::bandCoeff();
    printf("   bandas (meia oitava):");
    for (int b = 0; b < as::BANDS; ++b)
      printf(" %u", (unsigned)as::detail::bandHz(b));
    printf(" Hz\n   coef Q14:");
    bool fits = true;
    for (int b = 0; b < as::BANDS; ++b) {
      printf(" %d", (int)c[b]);
      const double f = as::detail::bandHz(b);
      const double want = 16384.0 * 2.0 * cos(2.0 * M_PI * f / (double)as::SAMPLE_RATE);
      if (fabs(want - c[b]) > 20.0) // tolerancia: bandHz e arredondado
        fits = false;
    }
    printf("\n");
    check(fits, "coeficientes batem com 2*cos(2*pi*f/fs) em Q14");
    check(as::detail::bandHz(5) == 1000, "a banda 5 esta em 1000 Hz exatos");
  }

  // -----------------------------------------------------------------------
  printf("\n3. PONTO FIXO: A ARMADILHA DO DESLOCAMENTO\n");
  // -----------------------------------------------------------------------
  {
    // O que se mede aqui NAO e a soma do sinal (um seno truncado em 128
    // amostras nao tem media zero de qualquer jeito), e sim o ERRO DE
    // QUANTIZACAO introduzido pela aritmetica: a diferenca entre o resultado
    // inteiro e o mesmo calculo em ponto flutuante. Truncar para zero tem erro
    // medio ~0; arredondar para -infinito (`>>`) tem erro medio -0,5 LSB por
    // amostra, que e exatamente o DC que o gerador de 1 kHz deste repositorio
    // injetava.

    // 3a. Janelamento.
    int16_t a[as::N], b[as::N];
    tone(a, 1000.0, 30000.0);
    memcpy(b, a, sizeof(a));
    const int16_t *h = as::detail::hann();
    double ideal = 0.0;
    long shifted = 0;
    for (int n = 0; n < as::N; ++n) {
      ideal += (double)b[n] * h[n] / 32768.0;
      shifted += (int16_t)(((int32_t)b[n] * h[n]) >> 15); // o jeito ERRADO
    }
    as::detail::applyWindow(a); // implementacao do header (divisao)
    long divided = 0;
    for (int n = 0; n < as::N; ++n)
      divided += a[n];
    printf("   seno de 1 kHz janelado, soma das 128 amostras:\n");
    printf("     exata (ponto flutuante) = %10.2f\n", ideal);
    printf("     com DIVISAO (o header)  = %10ld   erro = %+7.2f LSB\n", divided, divided - ideal);
    printf("     com `>> 15` (o errado)  = %10ld   erro = %+7.2f LSB\n", shifted, shifted - ideal);
    printf("   -> o deslocamento injetaria %+.0f LSB de DC por janela, %+.0f por segundo a 20 Hz\n",
           shifted - ideal, (shifted - ideal) * 20.0);
    check(fabs(divided - ideal) < 8.0, "a divisao praticamente nao tem erro medio (< 8 LSB em 128)");
    check(shifted - ideal < -40.0, "o `>> 15` injeta DC negativo (armadilha reproduzida)");

    // 3b. Mistura L+R do produtor. L e R com fases diferentes, senao a soma e
    // sempre par e os dois metodos coincidem por acaso.
    as::Tap tap;
    static int16_t st[2 * as::N];
    for (int n = 0; n < as::N; ++n) {
      st[2 * n] = sat(30000.0 * sin(2.0 * M_PI * 1000.0 * n / (double)as::SAMPLE_RATE));
      st[2 * n + 1] = sat(21000.0 * sin(2.0 * M_PI * 1500.0 * n / (double)as::SAMPLE_RATE + 0.9));
    }
    check(tap.publish(st, 2 * as::N, 2), "publish() aceitou um bloco estereo cheio");
    int16_t win[as::N];
    uint16_t pl = 0, pr = 0;
    uint32_t sq = 0;
    check(tap.snapshot(win, pl, pr, sq), "snapshot() devolveu a janela publicada");
    double mixIdeal = 0.0;
    long mixDiv = 0, mixShift = 0;
    for (int n = 0; n < as::N; ++n) {
      mixIdeal += ((double)st[2 * n] + (double)st[2 * n + 1]) / 2.0;
      mixDiv += win[n];
      mixShift += (int16_t)(((int32_t)st[2 * n] + (int32_t)st[2 * n + 1]) >> 1);
    }
    printf("   mistura L+R: exata = %.1f | DIVISAO = %ld (erro %+.1f) | `>> 1` = %ld (erro %+.1f)\n",
           mixIdeal, mixDiv, mixDiv - mixIdeal, mixShift, mixShift - mixIdeal);
    check(fabs(mixDiv - mixIdeal) < 4.0, "a mistura do produtor nao introduz erro medio");
    check(mixShift - mixIdeal < -20.0, "o `>> 1` na mistura injetaria DC (armadilha reproduzida)");

    // 3c. Offset DC na ENTRADA. Janelar um bloco com nivel medio produz o
    // espectro da propria janela, que cai em cima das bandas graves. A remocao
    // de media do applyWindow existe para isso.
    as::Scope sc;
    as::reset(sc);
    for (int n = 0; n < as::N; ++n)
      sc.work[n] = 10000;
    as::analyzeSpectrum(sc);
    int maxBar = 0;
    for (int i = 0; i < as::BANDS; ++i)
      if (sc.target[i] > maxBar)
        maxBar = sc.target[i];
    printf("   entrada constante (+10000, DC puro): barra mais alta = %d px de %d\n", maxBar,
           as::BAR_MAX);
    check(maxBar == 0, "DC puro nao acende o mostrador");

    // E um tom de verdade com DC grudado por cima tem de ler igual ao tom limpo.
    as::reset(sc);
    tone(sc.work, 1000.0, 12000.0);
    as::analyzeSpectrum(sc);
    uint8_t clean[as::BANDS];
    memcpy(clean, sc.target, sizeof(clean));
    as::reset(sc);
    tone(sc.work, 1000.0, 12000.0);
    for (int n = 0; n < as::N; ++n)
      sc.work[n] = sat((double)sc.work[n] + 9000.0); // offset de -11 dBFS
    as::analyzeSpectrum(sc);
    int worst = 0;
    for (int i = 0; i < as::BANDS; ++i) {
      const int d = abs((int)clean[i] - (int)sc.target[i]);
      if (d > worst)
        worst = d;
    }
    printf("   tom de 1 kHz com +9000 de offset: maior diferenca = %d px\n", worst);
    check(worst <= 1, "o offset DC da fonte nao muda o mostrador");
  }

  // -----------------------------------------------------------------------
  printf("\n4. SILENCIO -> MOSTRADOR PLANO\n");
  // -----------------------------------------------------------------------
  {
    as::Scope sc;
    as::reset(sc);
    memset(sc.work, 0, sizeof(sc.work));
    as::analyzeSpectrum(sc);
    bool flat = true;
    for (int b = 0; b < as::BANDS; ++b)
      if (sc.target[b] != 0)
        flat = false;
    check(flat, "12 bandas em 0 px com entrada toda zero");
    // E com 1 LSB de ruido tambem: o piso da janela de 48 dB tem que cortar.
    for (int n = 0; n < as::N; ++n)
      sc.work[n] = (n & 1) ? 1 : -1;
    as::analyzeSpectrum(sc);
    int mx = 0;
    for (int b = 0; b < as::BANDS; ++b)
      if (sc.target[b] > mx)
        mx = sc.target[b];
    printf("   ruido de +-1 LSB: barra mais alta = %d px\n", mx);
    check(mx == 0, "ruido de 1 LSB fica abaixo do piso do mostrador");
  }

  // -----------------------------------------------------------------------
  printf("\n5. TOM DE 1 kHz DE FUNDO DE ESCALA\n");
  // -----------------------------------------------------------------------
  long long refPower = 0;
  {
    as::Scope sc;
    as::reset(sc);
    tone(sc.work, 1000.0, 32767.0);
    as::detail::applyWindow(sc.work);
    long long p[as::BANDS];
    for (int b = 0; b < as::BANDS; ++b)
      p[b] = as::bandPower(sc.work, b);
    long long peak = 0;
    int peakBand = -1;
    for (int b = 0; b < as::BANDS; ++b)
      if (p[b] > peak) {
        peak = p[b];
        peakBand = b;
      }
    refPower = peak;
    printf("   potencia de pico = %lld, log2 = %.3f (REF_LOG2Q8/256 = %d)\n", peak,
           log2((double)peak), as::REF_LOG2Q8 / 256);
    printf("   banda   f (Hz)     dB rel. pico\n");
    int above = 0;
    for (int b = 0; b < as::BANDS; ++b) {
      const double d = db(p[b], peak);
      printf("     %2d   %6u     %8.2f %s\n", b, (unsigned)as::detail::bandHz(b), d,
             b == peakBand ? "<-- pico" : "");
      if (b != peakBand && d > -20.0)
        ++above;
    }
    check(peakBand == 5, "o pico caiu na banda de 1000 Hz");
    check(above == 0, "nenhuma outra banda passa de -20 dB do pico");
    check(fabs(log2((double)peak) - as::REF_LOG2Q8 / 256.0) < 0.1,
          "a referencia de fundo de escala do header bate com a medida");

    // O mesmo tom mapeado para pixel tem de encostar no topo.
    as::reset(sc);
    tone(sc.work, 1000.0, 32767.0);
    as::analyzeSpectrum(sc);
    printf("   em pixels:");
    for (int b = 0; b < as::BANDS; ++b)
      printf(" %d", (int)sc.target[b]);
    printf("  (maximo %d)\n", as::BAR_MAX);
    check(sc.target[5] == as::BAR_MAX, "a barra de 1 kHz vai ao topo da escala");
  }

  // -----------------------------------------------------------------------
  printf("\n6. DOIS TONS -> DOIS PICOS\n");
  // -----------------------------------------------------------------------
  {
    as::Scope sc;
    as::reset(sc);
    // 250 Hz e 4000 Hz, os dois a metade da escala, ou seja bandas 1 e 9.
    tone(sc.work, 250.0, 16000.0);
    toneAdd(sc.work, 4000.0, 16000.0);
    as::analyzeSpectrum(sc);
    printf("   pixels por banda:");
    for (int b = 0; b < as::BANDS; ++b)
      printf(" %d", (int)sc.target[b]);
    printf("\n");
    // "Pico local" = maior que os dois vizinhos.
    int peaks = 0, pk[8] = {0};
    for (int b = 0; b < as::BANDS; ++b) {
      const int l = (b > 0) ? sc.target[b - 1] : -1;
      const int r = (b < as::BANDS - 1) ? sc.target[b + 1] : -1;
      if (sc.target[b] > l && sc.target[b] > r && sc.target[b] > 0) {
        if (peaks < 8)
          pk[peaks] = b;
        ++peaks;
      }
    }
    printf("   picos locais: %d (bandas", peaks);
    for (int i = 0; i < peaks && i < 8; ++i)
      printf(" %d", pk[i]);
    printf(")\n");
    check(peaks == 2, "exatamente dois picos locais");
    check(peaks == 2 && pk[0] == 1 && pk[1] == 9, "os picos estao em 250 Hz e 4000 Hz");
    check(sc.target[5] < sc.target[1] && sc.target[5] < sc.target[9],
          "o vale entre os dois tons fica abaixo dos dois");
  }

  // -----------------------------------------------------------------------
  printf("\n7. FUNDO DE ESCALA: SEM ESTOURO\n");
  // -----------------------------------------------------------------------
  {
    long long maxState = 0, maxPower = 0;
    int mismatches = 0, cases = 0;
    const double phases[3] = {0.0, 0.7, 1.9};
    for (int fb = 0; fb < as::BANDS; ++fb) {
      for (int ph = 0; ph < 3; ++ph) {
        int16_t w[as::N];
        tone(w, as::detail::bandHz(fb), 32767.0, phases[ph]);
        as::detail::applyWindow(w);
        for (int b = 0; b < as::BANDS; ++b) {
          ++cases;
          const long long got = as::bandPower(w, b);
          const long long want = goertzelRef(w, as::detail::bandCoeff()[b], &maxState);
          if (got != want)
            ++mismatches;
          if (got > maxPower)
            maxPower = got;
        }
      }
      // Onda quadrada de fundo de escala: pior caso de energia.
      int16_t q[as::N];
      square(q, as::detail::bandHz(fb));
      as::detail::applyWindow(q);
      for (int b = 0; b < as::BANDS; ++b) {
        ++cases;
        const long long got = as::bandPower(q, b);
        const long long want = goertzelRef(q, as::detail::bandCoeff()[b], &maxState);
        if (got != want)
          ++mismatches;
        if (got > maxPower)
          maxPower = got;
      }
    }
    printf("   %d casos (senos em 12 bandas x 3 fases + quadradas), todos em fundo de escala\n", cases);
    printf("   maior |estado| observado = %lld  (int32_t aguenta %d, folga de %.0fx)\n", maxState,
           2147483647, 2147483647.0 / (double)maxState);
    printf("   maior potencia = %lld (log2 = %.2f), int64_t aguenta log2 = 63\n", maxPower,
           log2((double)maxPower));
    check(mismatches == 0, "o estado em int32_t bate bit a bit com a referencia em int64_t");
    check(maxState < 2147483647LL, "o estado do Goertzel nao chega perto de estourar int32_t");
    check(maxPower > 0 && log2((double)maxPower) < 60.0, "a potencia nao chega perto de estourar int64_t");
  }

  // -----------------------------------------------------------------------
  printf("\n8. CANAL PRODUTOR (core 0) -> CONSUMIDOR (core 1)\n");
  // -----------------------------------------------------------------------
  {
    as::Tap tap;
    int16_t win[as::N];
    uint16_t pl = 0, pr = 0;
    uint32_t sq = 0;
    check(!tap.snapshot(win, pl, pr, sq), "snapshot() e falso antes de qualquer publicacao");

    static int16_t st[2 * as::N];
    stereoTone(st, as::N, 1000.0, 2000.0, 20000.0);
    check(!tap.publish(st, 2 * (as::N - 1), 2), "bloco curto NAO publica (janela ficaria com zeros)");
    check(tap.published() == 0, "e a sequencia nao andou");
    check(tap.publish(st, 2 * as::N, 2), "bloco cheio publica");
    check(tap.published() == 1, "a sequencia andou uma vez");
    check(tap.snapshot(win, pl, pr, sq) && sq == 1, "o consumidor leu a sequencia 1");
    bool same = true;
    for (int n = 0; n < as::N; ++n)
      if (win[n] != (int16_t)(((int32_t)st[2 * n] + (int32_t)st[2 * n + 1]) / 2))
        same = false;
    check(same, "a janela copiada e exatamente a mistura L+R do bloco");
    printf("   picos publicados: L=%u R=%u (amplitude de entrada 20000)\n", pl, pr);
    check(pl >= 19000 && pl <= 20000 && pr >= 19000 && pr <= 20000, "os picos L/R batem com a entrada");

    // Banco alternado: duas publicacoes seguidas caem em bancos diferentes, e
    // o consumidor sempre ve a mais recente inteira.
    stereoTone(st, as::N, 4000.0, 4000.0, 8000.0);
    tap.publish(st, 2 * as::N, 2);
    check(tap.snapshot(win, pl, pr, sq) && sq == 2, "a segunda publicacao foi vista");
    check(pl <= 8000 && pl >= 7500, "e trouxe o pico NOVO, nao o antigo");
    check(tap.dropped() == 0, "nenhuma copia descartada num teste sem concorrencia");

    // Sobrecarga do consumidor: sem janela nova, tick() conta `starved` e NAO
    // reanalisa. E o comportamento correto — perder analise, nunca o audio.
    as::Scope sc;
    as::reset(sc);
    sc.mode = as::BARS;
    uint32_t t = 1000;
    as::tick(&rca, 0, 0, sc, tap, t); // primeiro: fundo + analise
    const uint32_t f1 = sc.frames;
    for (int i = 0; i < 5; ++i) {
      t += as::UPDATE_MS;
      as::tick(&rca, 0, 0, sc, tap, t); // nada novo publicado
    }
    printf("   quadros analisados=%u, prazos sem dado novo=%u\n", (unsigned)sc.frames,
           (unsigned)sc.starved);
    check(sc.frames == f1, "sem janela nova, nenhuma analise nova foi feita");
    check(sc.starved == 5, "e os 5 prazos vazios foram contados");

    // Envelhecimento: passado STALE_MS o mostrador desce sozinho ate zero.
    for (int i = 0; i < 40; ++i) {
      t += as::UPDATE_MS;
      as::tick(&rca, 0, 0, sc, tap, t);
    }
    int resting = 0;
    for (int b = 0; b < as::BANDS; ++b)
      resting += sc.level[b] + sc.peak[b];
    check(resting == 0, "sem audio, o mostrador cai ate o repouso (nao congela imagem no tubo)");

    // E o prazo respeita o wrap de millis(): now perto de 2^32.
    as::reset(sc);
    uint32_t big = 0xFFFFFF00u;
    as::tick(&rca, 0, 0, sc, tap, big);
    const uint32_t before = sc.starved + sc.frames;
    big += as::UPDATE_MS; // passa de 2^32 e volta a zero
    as::tick(&rca, 0, 0, sc, tap, big);
    check(sc.starved + sc.frames == before + 1, "timeReached() atravessa o wrap de millis() sem travar");
  }

  // -----------------------------------------------------------------------
  printf("\n9. REPINTURA INCREMENTAL\n");
  // -----------------------------------------------------------------------
  {
    const uint32_t full = (uint32_t)(as::PANEL_W + 2) * (uint32_t)(as::PANEL_H + 2);
    printf("   painel inteiro = %u px\n", (unsigned)full);
    as::Tap tap;
    static int16_t st[2 * as::N];
    as::Scope sc;
    uint32_t t = 10000;

    for (int m = 0; m < as::MODE_COUNT; ++m) {
      as::reset(sc);
      as::setMode(sc, (as::Mode)m);
      stereoTone(st, as::N, 1000.0, 1000.0, 30000.0);
      tap.publish(st, 2 * as::N, 2);
      t += as::UPDATE_MS;
      as::tick(&rca, 0, 0, sc, tap, t); // primeiro quadro: fundo inteiro
      const uint32_t firstPx = sc.touched;

      // Regime permanente com o MESMO sinal: nada deveria mudar.
      uint32_t steady = 0;
      for (int i = 0; i < 8; ++i) {
        tap.publish(st, 2 * as::N, 2);
        t += as::UPDATE_MS;
        as::tick(&rca, 0, 0, sc, tap, t);
        steady += sc.touched;
      }
      // Sinal novo: mede o custo de uma mudanca grande.
      stereoTone(st, as::N, 4000.0, 250.0, 30000.0);
      tap.publish(st, 2 * as::N, 2);
      t += as::UPDATE_MS;
      as::tick(&rca, 0, 0, sc, tap, t);
      const uint32_t changePx = sc.touched;
      const as::Rect d = as::dirtyRect(sc);

      printf("   %-13s 1o quadro %5u px | 8 quadros parados %4u px (%.1f/quadro) | mudanca %5u px "
             "(%.1f%% do painel)\n",
             as::modeName((as::Mode)m), (unsigned)firstPx, (unsigned)steady, steady / 8.0,
             (unsigned)changePx, 100.0 * changePx / full);
      printf("                 dirty = (%d,%d %dx%d)\n", d.x, d.y, d.w, d.h);
      check(firstPx == full, "o primeiro quadro repinta o painel inteiro (uma vez so)");
      check(changePx < full, "uma mudanca grande escreve menos que o painel inteiro");
      if (m != as::SCOPE) // o traco do osciloscopio muda de fase a cada janela
        check(steady == 0, "sinal parado = ZERO pixels escritos (sem cintilacao no tubo)");
      // Area segura.
      check(d.w == 0 || (d.x >= SAFE_L && d.y >= SAFE_T && d.x + d.w <= SAFE_R && d.y + d.h <= SAFE_B),
            "o retangulo sujo cabe na area segura do tubo");
    }
  }

  // -----------------------------------------------------------------------
  printf("\n10. PNGs\n");
  // -----------------------------------------------------------------------
  {
    as::Tap tap;
    static int16_t st[2 * as::N];
    as::Scope sc;
    uint32_t t = 50000;

    // --- osciloscopio: 500 Hz, onda bem visivel ---
    as::reset(sc);
    as::setMode(sc, as::SCOPE);
    backdrop("MODO: OSCILOSCOPIO  (500 HZ)");
    stereoTone(st, as::N, 500.0, 500.0, 30000.0);
    tap.publish(st, 2 * as::N, 2);
    t += as::UPDATE_MS;
    as::tick(&rca, 0, 0, sc, tap, t);
    as::drawModeLabel(&rca, 0, 0, sc);
    shot("scope");

    // --- espectro: varios tons, para as 12 barras ficarem em alturas diferentes ---
    as::reset(sc);
    as::setMode(sc, as::BARS);
    backdrop("MODO: ESPECTRO  (MULTITOM)");
    {
      int16_t mono[as::N];
      memset(mono, 0, sizeof(mono));
      toneAdd(mono, 177.0, 9000.0);
      toneAdd(mono, 500.0, 6000.0);
      toneAdd(mono, 1000.0, 11000.0);
      toneAdd(mono, 2828.0, 4000.0);
      toneAdd(mono, 8000.0, 2000.0);
      for (int n = 0; n < as::N; ++n)
        st[2 * n] = st[2 * n + 1] = mono[n];
    }
    // Varias atualizacoes: as barras sobem no ataque e os picos assentam.
    for (int i = 0; i < 4; ++i) {
      tap.publish(st, 2 * as::N, 2);
      t += as::UPDATE_MS;
      as::tick(&rca, 0, 0, sc, tap, t);
    }
    as::drawModeLabel(&rca, 0, 0, sc);
    shot("bars");

    // --- espectro em decaimento: mostra o ponto de pico separado da barra ---
    backdrop("MODO: ESPECTRO  (PICOS RETIDOS)");
    as::drawStatic(&rca, 0, 0, sc);
    for (int b = 0; b < as::BANDS; ++b) {
      sc.level[b] = (uint8_t)(6 + b * 2);
      sc.peak[b] = (uint8_t)(sc.level[b] + 12 > as::BAR_MAX ? as::BAR_MAX : sc.level[b] + 12);
    }
    as::detail::drawBars(&rca, 0, 0, sc);
    as::drawModeLabel(&rca, 0, 0, sc);
    shot("bars_peak");

    // --- VU ---
    as::reset(sc);
    as::setMode(sc, as::VU);
    backdrop("MODO: VU  (L -3 DBFS / R -12 DBFS)");
    stereoTone(st, as::N, 1000.0, 1000.0, 1.0);
    for (int n = 0; n < as::N; ++n) {
      st[2 * n] = sat(23000.0 * sin(2.0 * M_PI * 1000.0 * n / (double)as::SAMPLE_RATE));
      st[2 * n + 1] = sat(8200.0 * sin(2.0 * M_PI * 1000.0 * n / (double)as::SAMPLE_RATE));
    }
    for (int i = 0; i < 3; ++i) {
      tap.publish(st, 2 * as::N, 2);
      t += as::UPDATE_MS;
      as::tick(&rca, 0, 0, sc, tap, t);
    }
    as::drawModeLabel(&rca, 0, 0, sc);
    printf("   VU: agulha L=%u px, R=%u px (de %d)\n", sc.vuL, sc.vuR, as::VU_LEN);
    check(sc.vuL > sc.vuR, "o canal mais alto tem a agulha mais longa");
    shot("vu");
  }

  // -----------------------------------------------------------------------
  printf("\n%d verificacoes, %d falha(s)\n\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
