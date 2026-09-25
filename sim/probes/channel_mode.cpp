// ============================================================================
//  Bancada do modo canal e do timer de soneca (include/ChannelMode.h).
//
//  Metade logica, metade desenho:
//
//    * avanco sequencial e a volta no fim da lista;
//    * modo desligado (o comportamento antigo tem de continuar valendo);
//    * sorteio: cada rodada e uma permutacao (ninguem repete antes de todos
//      tocarem), nao ha repeticao imediata na virada de rodada e a mesma
//      semente produz a mesma ordem;
//    * a volta do millis() (~49 dias) na vinheta E no timer de soneca — o caso
//      que um teste ingenuo nunca pega porque a bancada roda com now pequeno;
//    * cartao de programas quebrados: o canal desiste em vez de girar a lista
//      para sempre;
//    * a vinheta e o indicador SLEEP desenhados, com conferencia pixel a pixel
//      de que nada de conteudo escapou da area segura do tubo.
//
//    make -C sim probes && ./sim/build/probe_channel_mode
//
//  Sai com 1 se algo falhar, para servir de porta em CI.
//
//  A regra de probe do Makefile nao depende dos headers: depois de mexer em
//  include/ChannelMode.h, um `touch sim/probes/channel_mode.cpp` antes do make.
// ============================================================================

#include "fj/Gfx.h" // LovyanGFX + backend SDL, a mesma porta de entrada do firmware
#include "SimPanel.h" // painel SDL em 320x240, não no 240x320 padrão da LovyanGFX

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ChannelMode.h"
#include "SafeArea.h"

static lgfx::Panel_sdl panel;
static lgfx::LGFX_Device tv;

static int failures = 0;

static void check(bool ok, const char *what) {
  printf("  %s %s\n", ok ? "ok  " : "FALHA", what);
  if (!ok)
    ++failures;
}

// ---------------------------------------------------------------------------
//  Biblioteca falsa: o Channel so guarda indices, entao a bancada nao precisa
//  de cartao SD nenhum.
// ---------------------------------------------------------------------------

static const char *kLib[] = {
    "/M5RETRO/videos/jornal-1987", "/M5RETRO/videos/desenho-da-tarde",
    "/M5RETRO/videos/filme-de-domingo", "/M5RETRO/videos/vinheta",
    "/M5RETRO/videos/show-de-calouros", "/M5RETRO/videos/novela",
    "/M5RETRO/videos/comercial", "/M5RETRO/videos/encerramento",
};
static int kLibCount = 8;

static const char *pathAdapter(int index, void *) {
  return (index >= 0 && index < kLibCount) ? kLib[index] : 0;
}

// Roda uma troca de programa inteira: fim do programa -> vinheta -> proximo.
// Devolve o indice que entrou no ar, ou -1 se o canal recusou continuar.
static int nextProgram(channel::Channel &ch, uint32_t &now) {
  if (!ch.finished(now))
    return -1;
  if (ch.ready(now) >= 0) {
    printf("  FALHA vinheta liberou antes da hora\n");
    ++failures;
  }
  now += ch.bumperMs();
  const int n = ch.ready(now);
  if (n >= 0)
    ch.started(n, now);
  return n;
}

// ---------------------------------------------------------------------------
//  1. Sequencial: avanca e da a volta
// ---------------------------------------------------------------------------
static void testSequential() {
  printf("\n[1] sequencial e volta no fim\n");
  channel::Channel ch;
  ch.attach(4, pathAdapter);
  ch.setMode(channel::Mode::Sequential);
  uint32_t now = 10000;
  ch.started(0, now);

  int seen[6];
  for (int i = 0; i < 6; ++i) {
    now += 1000;
    seen[i] = nextProgram(ch, now);
  }
  const int want[6] = {1, 2, 3, 0, 1, 2};
  bool ok = true;
  for (int i = 0; i < 6; ++i)
    ok = ok && seen[i] == want[i];
  printf("  ordem: %d %d %d %d %d %d (esperado 1 2 3 0 1 2)\n", seen[0], seen[1], seen[2], seen[3],
         seen[4], seen[5]);
  check(ok, "avanco sequencial com volta em count=4");

  // Lista de um item so: a volta cai nele mesmo, sem divisao por zero.
  channel::Channel one;
  one.attach(1, pathAdapter);
  one.setMode(channel::Mode::Sequential);
  uint32_t t = 500;
  one.started(0, t);
  check(nextProgram(one, t) == 0, "count=1 repete o unico programa");

  // Lista vazia: o canal recusa e o chamador volta ao comportamento antigo.
  channel::Channel empty;
  empty.attach(0, pathAdapter);
  empty.setMode(channel::Mode::Sequential);
  uint32_t t2 = 700;
  check(!empty.finished(t2), "lista vazia devolve false (volta para a biblioteca)");
}

// ---------------------------------------------------------------------------
//  2. Modo desligado preserva o comportamento atual
// ---------------------------------------------------------------------------
static void testOff() {
  printf("\n[2] modo desligado\n");
  channel::Channel ch;
  ch.attach(kLibCount, pathAdapter);
  uint32_t now = 1234;
  ch.started(2, now);
  check(ch.mode() == channel::Mode::Off, "padrao e OFF");
  check(!ch.finished(now + 10), "finished() devolve false: o chamador para no fim");
  check(ch.phase() == channel::Phase::Idle, "sem vinheta agendada");

  check(ch.cycleMode() == channel::Mode::Sequential, "ciclo OFF -> EM ORDEM");
  check(ch.cycleMode() == channel::Mode::Shuffle, "ciclo EM ORDEM -> ALEATORIO");
  check(ch.cycleMode() == channel::Mode::Off, "ciclo ALEATORIO -> OFF");
}

// ---------------------------------------------------------------------------
//  3. Sorteio: permutacao sem repetir, determinista
// ---------------------------------------------------------------------------
static void runShuffle(uint32_t seed, int rounds, int *out) {
  channel::Channel ch;
  ch.attach(kLibCount, pathAdapter);
  ch.setMode(channel::Mode::Shuffle);
  ch.seed(seed);
  uint32_t now = 77;
  int produced = 0;
  for (int r = 0; r < rounds; ++r)
    for (int i = 0; i < kLibCount; ++i) {
      now += 5000;
      out[produced++] = nextProgram(ch, now);
    }
}

static void testShuffle() {
  printf("\n[3] sorteio\n");
  const int rounds = 3;
  int a[3 * 8], b[3 * 8];
  runShuffle(0xC0FFEEu, rounds, a);
  runShuffle(0xC0FFEEu, rounds, b);

  bool deterministic = true;
  for (int i = 0; i < rounds * kLibCount; ++i)
    deterministic = deterministic && a[i] == b[i];
  check(deterministic, "mesma semente produz a mesma ordem");

  bool permutation = true, immediateRepeat = false;
  for (int r = 0; r < rounds; ++r) {
    int seen[8];
    for (int i = 0; i < kLibCount; ++i)
      seen[i] = 0;
    printf("  rodada %d:", r + 1);
    for (int i = 0; i < kLibCount; ++i) {
      const int v = a[r * kLibCount + i];
      printf(" %d", v);
      if (v < 0 || v >= kLibCount || seen[v])
        permutation = false;
      else
        seen[v] = 1;
    }
    printf("\n");
  }
  for (int i = 1; i < rounds * kLibCount; ++i)
    if (a[i] == a[i - 1])
      immediateRepeat = true;
  check(permutation, "cada rodada e uma permutacao (ninguem repete antes de esgotar)");
  check(!immediateRepeat, "nao ha repeticao imediata na virada de rodada");

  int c[3 * 8];
  runShuffle(0x12345678u, rounds, c);
  bool different = false;
  for (int i = 0; i < rounds * kLibCount; ++i)
    different = different || c[i] != a[i];
  check(different, "sementes diferentes dao ordens diferentes");

  // Escolha manual no meio da rodada nao pode fazer o titulo tocar duas vezes.
  channel::Channel ch;
  ch.attach(kLibCount, pathAdapter);
  ch.setMode(channel::Mode::Shuffle);
  ch.seed(99);
  uint32_t now = 1000;
  now += 1000;
  int first = nextProgram(ch, now);
  ch.started(5, now); // o usuario abriu o indice 5 a dedo na biblioteca
  int seen[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  seen[5] = 1;
  bool ok = first >= 0;
  for (int i = 0; i < kLibCount - 1; ++i) {
    now += 1000;
    const int v = nextProgram(ch, now);
    if (v < 0 || seen[v])
      ok = false;
    else
      seen[v] = 1;
  }
  check(ok, "escolha manual realinha a rodada sem repetir ninguem");
}

// ---------------------------------------------------------------------------
//  4. A volta do millis()
// ---------------------------------------------------------------------------
static void testWrap() {
  printf("\n[4] volta do millis()\n");
  channel::Channel ch;
  ch.attach(4, pathAdapter);
  ch.setMode(channel::Mode::Sequential);
  ch.setBumperMs(3000);

  // Falta 1 s para o contador virar: o prazo da vinheta cai do outro lado do
  // zero. Com `now >= deadline` cru isto liberaria na hora — errado.
  uint32_t now = 0xFFFFFFFFu - 1000u;
  ch.started(0, now);
  check(ch.finished(now), "vinheta agendada em cima da virada");
  check(ch.ready(now) < 0, "nada liberado no instante zero da vinheta");

  now += 1500; // ja passou da virada; ainda faltam 1500 ms de vinheta
  check(now < 1000u, "o contador realmente deu a volta");
  check(ch.ready(now) < 0, "depois da virada, a vinheta ainda segura");
  printf("  falta %u ms, decorrido %u ms\n", (unsigned)ch.bumperRemainingMs(now),
         (unsigned)ch.bumperElapsedMs(now));
  check(ch.bumperRemainingMs(now) == 1500u, "restante correto atravessando a virada");
  check(ch.bumperElapsedMs(now) == 1500u, "decorrido correto atravessando a virada");

  now += 1500;
  check(ch.ready(now) == 1, "libera o proximo indice depois do prazo");

  // Timer de soneca do outro lado da virada.
  sleeptimer::SleepTimer st;
  uint32_t t = 0xFFFFFFFFu - 60000u; // 1 min antes de virar
  st.setMinutes(15, t);
  check(st.active() && st.minutes() == 15, "SLEEP 15 armado");
  check(!st.expired(t + 14u * 60000u), "nao vence aos 14 min (ja do outro lado da virada)");
  check(st.remainingMinutes(t + 14u * 60000u) == 1, "resta 1 min");
  check(st.expired(t + 15u * 60000u), "vence aos 15 min mesmo tendo dado a volta");
  check(st.remainingMs(t + 15u * 60000u) == 0, "restante zera no vencimento (nao estoura)");
  check(st.expired(t + 20u * 60000u), "continua vencido depois do prazo");
}

// ---------------------------------------------------------------------------
//  5. Programas quebrados: desistir em vez de girar
// ---------------------------------------------------------------------------
static void testAllBroken() {
  printf("\n[5] cartao de pastas quebradas\n");
  channel::Channel ch;
  ch.attach(4, pathAdapter);
  ch.setMode(channel::Mode::Sequential);
  ch.setBumperMs(0); // vinheta instantanea para nao alongar o teste
  uint32_t now = 100;
  ch.started(0, now);
  int attempts = 0;
  bool alive = ch.finished(now);
  while (alive && attempts < 50) {
    const int n = ch.ready(now);
    if (n < 0) {
      now += 10;
      continue;
    }
    ++attempts;
    alive = ch.failed(now); // startProgram() falhou em todas
  }
  printf("  tentativas ate desistir: %d\n", attempts);
  check(!alive, "o canal desiste (sem laco apertado)");
  check(attempts <= 6, "desistiu dentro de uma volta da lista");
}

// ---------------------------------------------------------------------------
//  6. Timer de soneca: degraus e contagem
// ---------------------------------------------------------------------------
static void testSleepTimer() {
  printf("\n[6] timer de soneca\n");
  sleeptimer::SleepTimer st;
  uint32_t now = 5000;
  check(!st.active(), "comeca desligado");
  check(!st.expired(now + 999999u), "desligado nunca vence");

  const uint16_t want[6] = {15, 30, 60, 90, 120, 0};
  bool ok = true;
  printf("  ciclo:");
  for (int i = 0; i < 6; ++i) {
    const uint16_t m = st.cycle(now);
    printf(" %u", (unsigned)m);
    ok = ok && m == want[i];
  }
  printf(" (esperado 15 30 60 90 120 0)\n");
  check(ok, "ciclo OFF/15/30/60/90/120 com volta");

  st.setMinutes(30, now);
  check(st.remainingMinutes(now) == 30, "30 min recem-armado");
  check(st.remainingMinutes(now + 29u * 60000u + 59000u) == 1, "arredonda para cima: 1 min");
  check(!st.expired(now + 30u * 60000u - 1u), "nao vence 1 ms antes");
  check(st.expired(now + 30u * 60000u), "vence no instante exato");

  st.setMinutes(30, now);
  st.restart(now + 20u * 60000u); // usuario mexeu num botao
  check(!st.expired(now + 40u * 60000u), "restart adia o desligamento");
  check(st.expired(now + 50u * 60000u), "mas o novo prazo vale");

  st.setMinutes(45, now); // valor fora da tabela
  check(!st.active(), "valor invalido desliga o timer");
}

// ---------------------------------------------------------------------------
//  7. Desenho: vinheta e indicador SLEEP dentro da area segura
// ---------------------------------------------------------------------------

static void savePng(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)tv.createPng(&len, 0, 0, crt::W, crt::H);
  if (!png) {
    printf("  falha ao gerar %s\n", name);
    ++failures;
    return;
  }
  char path[256];
  snprintf(path, sizeof(path), "build/%s", name);
  FILE *f = fopen(path, "wb");
  fwrite(png, 1, len, f);
  fclose(f);
  free(png);
  printf("  gravado %s (%zu bytes)\n", path, len);
}

// "Conteudo" e todo pixel que nao ficou da cor de fundo; nenhum pode estar fora
// da caixa segura. A referencia vem do framebuffer, nao do literal RGB565, para
// o teste valer em qualquer profundidade (era RGB332 no Core2).
static void checkSafeArea(const char *what) {
  const uint16_t bg = tv.readPixel(0, 0);
  int minX = crt::W, minY = crt::H, maxX = -1, maxY = -1, outside = 0;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x) {
      if (tv.readPixel(x, y) == bg)
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
          printf("    FORA DA AREA SEGURA: (%d,%d)\n", x, y);
        ++outside;
      }
    }
  if (maxX < 0) {
    printf("  %-26s nada desenhado\n", what);
    ++failures;
    return;
  }
  printf("  %-26s caixa x %d..%d  y %d..%d  (segura x %d..%d y %d..%d)  fora=%d\n", what, minX, maxX,
         minY, maxY, crt::SAFE_L, crt::SAFE_R - 1, crt::SAFE_T, crt::SAFE_B - 1, outside);
  if (outside)
    ++failures;
}

static void testDrawing() {
  printf("\n[7] desenho\n");

  channel::Channel ch;
  ch.attach(kLibCount, pathAdapter);
  ch.setMode(channel::Mode::Sequential);
  ch.setBumperMs(3000);
  uint32_t now = 20000;
  ch.started(0, now);
  ch.finished(now);

  // Titulo curto, no meio da vinheta.
  now += 1500;
  tv.fillScreen(TFT_BLACK);
  ch.drawBumper(&tv, 0, 0, 0, now);
  checkSafeArea("vinheta 50%");
  savePng("probe_channel_bumper_50.png");

  // Quase no fim: a barra so cresce, entao repintar nao apaga nada.
  now += 1200;
  ch.drawBumper(&tv, 0, 0, 0, now);
  checkSafeArea("vinheta 90% (repinte)");
  savePng("probe_channel_bumper_90.png");

  // Pior caso de texto: titulo longo (tem de cair para a fonte fina e truncar
  // dentro da area segura) e com acento (que TEM de sair sem buracos).
  channel::Channel longCh;
  longCh.attach(kLibCount, pathAdapter);
  longCh.setMode(channel::Mode::Sequential);
  uint32_t t = 900;
  longCh.started(0, t);
  longCh.finished(t);
  tv.fillScreen(TFT_BLACK);
  longCh.drawBumper(&tv, 0, 0, "SESSAO DA TARDE COM UM TITULO ENORME", t + 500);
  checkSafeArea("vinheta titulo longo");
  savePng("probe_channel_bumper_longo.png");

  // Titulo acentuado em UTF-8 cru (como vem do cartao). Tem de sair EXATAMENTE
  // igual ao equivalente ja sem acento: se a normalizacao nao acontecesse, cada
  // letra acentuada seriam dois bytes sem glifo — dois buracos, nao um.
  tv.fillScreen(TFT_BLACK);
  longCh.invalidateBumperPaint();
  longCh.drawBumper(&tv, 0, 0, "CINE MARAVILHA \xC3\x87\xC3\x83O", t + 500); // "CINE MARAVILHA CAO"
  checkSafeArea("vinheta titulo acentuado");
  savePng("probe_channel_bumper_acento.png");
  uint32_t accented = 0;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x)
      accented = accented * 31u + tv.readPixel(x, y);

  tv.fillScreen(TFT_BLACK);
  longCh.invalidateBumperPaint();
  longCh.drawBumper(&tv, 0, 0, "CINE MARAVILHA CAO", t + 500);
  uint32_t plain = 0;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x)
      plain = plain * 31u + tv.readPixel(x, y);
  check(accented == plain, "titulo acentuado sai identico ao sem acento (normalizado)");

  // Titulo derivado do caminho: o header tira o ultimo segmento e normaliza.
  char title[32];
  channel::Channel names;
  names.attach(kLibCount, pathAdapter);
  names.titleOf(1, title, sizeof(title));
  printf("  titulo do indice 1: \"%s\"\n", title);
  check(strcmp(title, "DESENHO-DA-TARDE") == 0, "titulo derivado do caminho, em caixa alta");

  // Indicador SLEEP sozinho (o caso real e por cima do filme, sem tarja).
  sleeptimer::SleepTimer st;
  st.setMinutes(30, 1000);
  tv.fillScreen(TFT_BLACK);
  st.drawBadge(&tv, 0, 0, 1000);
  checkSafeArea("indicador SLEEP 30");
  savePng("probe_channel_sleep_badge.png");

  st.setMinutes(120, 1000);
  tv.fillScreen(TFT_BLACK);
  st.drawBadge(&tv, 0, 0, 1000);
  checkSafeArea("indicador SLEEP 120");
}

int main(int, char **) {
  panel.setScaling(2, 2);
  sim::configure(panel);
  tv.setPanel(&panel);
  if (!tv.init()) {
    printf("falha ao iniciar o painel SDL\n");
    return 1;
  }
  tv.setColorDepth(16); // RGB565, igual ao canvas `tv` do Fruit Jam

  printf("tamanho do Channel: %zu bytes   SleepTimer: %zu bytes\n", sizeof(channel::Channel),
         sizeof(sleeptimer::SleepTimer));

  testSequential();
  testOff();
  testShuffle();
  testWrap();
  testAllBroken();
  testSleepTimer();
  testDrawing();

  printf("\n%s (%d falha(s))\n", failures ? "REPROVADO" : "APROVADO", failures);
  return failures ? 1 : 0;
}
