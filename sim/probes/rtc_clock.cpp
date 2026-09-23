// ============================================================================
// Bancada nativa da logica pura do include/RtcClock.h.
//
// O BM8563 nao existe no PC, entao o que da para testar aqui e exatamente o que
// pode estar errado sem ninguem perceber no aparelho:
//
//   1. conversao data/hora UTC -> epoch (substitui o timegm que o newlib nao
//      tem, e e o que semeia o settimeofday no boot);
//   2. o criterio de validade do RTC (ano < 2020 rejeitado, 2026 aceito);
//   3. o fuso: uma data UTC conhecida tem que render horario de Brasilia.
//
// Compilado em C++11, que e o padrao do firmware — o simulador usa C++17 e ja
// escondeu um constexpr com laco que so quebrava no aparelho.
//
//   g++ -std=gnu++11 -I../../include sim/probes/rtc_clock.cpp -o probe_rtc_clock
//   ./probe_rtc_clock
//
// Tambem sai junto com os demais em `make -C sim probes`.
// ============================================================================

#include "RtcClock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static int falhas = 0;
static int checagens = 0;

static void check(bool ok, const char *nome) {
  checagens++;
  if (!ok)
    falhas++;
  std::printf("%-58s %s\n", nome, ok ? "ok" : "FALHOU");
}

static void checkEq(long long got, long long want, const char *nome) {
  checagens++;
  const bool ok = got == want;
  if (!ok)
    falhas++;
  std::printf("%-58s %s", nome, ok ? "ok" : "FALHOU");
  if (!ok)
    std::printf("  (obtido %lld, esperado %lld)", got, want);
  std::printf("\n");
}

// ---------------------------------------------------------------------------
// 1. Conversao data/hora UTC -> epoch
// ---------------------------------------------------------------------------
static void testEpoch() {
  std::printf("\n-- epochFromUtc (UTC -> epoch) --\n");
  checkEq(rtcclock::epochFromUtc(1970, 1, 1, 0, 0, 0), 0LL, "1970-01-01 00:00:00Z = 0");
  checkEq(rtcclock::epochFromUtc(2000, 1, 1, 0, 0, 0), 946684800LL, "2000-01-01 00:00:00Z");
  checkEq(rtcclock::epochFromUtc(2020, 1, 1, 0, 0, 0), 1577836800LL, "2020-01-01 = EPOCH_MIN");
  checkEq(rtcclock::epochFromUtc(2024, 2, 29, 12, 34, 56), 1709210096LL, "2024-02-29 (bissexto)");
  checkEq(rtcclock::epochFromUtc(2026, 9, 23, 17, 05, 00), 1790183100LL, "2026-09-23 17:05:00Z");
  checkEq(rtcclock::epochFromUtc(2038, 1, 19, 3, 14, 7), 2147483647LL, "2038-01-19 (fim do int32)");
  checkEq(rtcclock::epochFromUtc(2100, 3, 1, 0, 0, 0), 4107542400LL, "2100-03-01 (2100 nao e bissexto)");

  // Ida e volta contra o gmtime_r do sistema, cobrindo 20 anos dia a dia.
  bool roundtrip = true;
  long long piorDiff = 0;
  for (long long t = 1577836800LL; t < 2208988800LL; t += 86399LL) { // passo primo-ish
    const time_t tt = (time_t)t;
    struct tm g;
    memset(&g, 0, sizeof(g));
    gmtime_r(&tt, &g);
    const long long back = (long long)rtcclock::epochFromUtc(
        g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    if (back != t) {
      roundtrip = false;
      piorDiff = back - t;
      std::printf("   divergencia em %lld: voltou %lld\n", t, back);
      break;
    }
  }
  (void)piorDiff;
  check(roundtrip, "ida e volta com gmtime_r, 2020..2040, passo 86399 s");
}

// ---------------------------------------------------------------------------
// 2. Validade do que sai do BM8563
// ---------------------------------------------------------------------------
static void testValidade() {
  std::printf("\n-- isPlausibleDate (rejeicao de RTC zerado/podre) --\n");
  // O caso real: BM8563 sem bateria devolve 2000-01-01 00:00:00.
  check(!rtcclock::isPlausibleDate(2000, 1, 1, 0, 0, 0), "2000-01-01 (bateria morta) rejeitado");
  check(!rtcclock::isPlausibleDate(1970, 1, 1, 0, 0, 0), "1970-01-01 rejeitado");
  check(!rtcclock::isPlausibleDate(2019, 12, 31, 23, 59, 59), "2019-12-31 rejeitado (ano < 2020)");
  check(rtcclock::isPlausibleDate(2020, 1, 1, 0, 0, 0), "2020-01-01 aceito (limite inferior)");
  check(rtcclock::isPlausibleDate(2026, 9, 23, 14, 5, 0), "2026-09-23 14:05:00 aceito");
  check(rtcclock::isPlausibleDate(2026, 12, 31, 23, 59, 59), "2026-12-31 23:59:59 aceito");

  std::printf("\n-- isPlausibleDate (campos fora de faixa) --\n");
  check(!rtcclock::isPlausibleDate(2026, 0, 10, 0, 0, 0), "mes 0 rejeitado");
  check(!rtcclock::isPlausibleDate(2026, 13, 10, 0, 0, 0), "mes 13 rejeitado");
  check(!rtcclock::isPlausibleDate(2026, 2, 29, 0, 0, 0), "2026-02-29 rejeitado (nao bissexto)");
  check(rtcclock::isPlausibleDate(2028, 2, 29, 0, 0, 0), "2028-02-29 aceito (bissexto)");
  check(!rtcclock::isPlausibleDate(2026, 4, 31, 0, 0, 0), "31 de abril rejeitado");
  check(!rtcclock::isPlausibleDate(2026, 1, 1, 24, 0, 0), "hora 24 rejeitada");
  check(!rtcclock::isPlausibleDate(2026, 1, 1, 0, 60, 0), "minuto 60 rejeitado");
  check(!rtcclock::isPlausibleDate(2026, 1, 1, -1, 0, 0), "hora negativa rejeitada (BCD invalido)");
  check(!rtcclock::isPlausibleDate(2100, 1, 1, 0, 0, 0), "2100 rejeitado (fora da janela)");

  std::printf("\n-- isPlausibleEpoch --\n");
  check(!rtcclock::isPlausibleEpoch(0), "epoch 0 rejeitado");
  check(!rtcclock::isPlausibleEpoch(946684800), "epoch de 2000 rejeitado");
  check(rtcclock::isPlausibleEpoch(1577836800), "epoch de 2020-01-01 aceito");
  check(rtcclock::isPlausibleEpoch(1790183100), "epoch de 2026 aceito");
}

// ---------------------------------------------------------------------------
// 3. Fuso horario: o ponto do exercicio todo
// ---------------------------------------------------------------------------
static void testFuso() {
  std::printf("\n-- TZ \"%s\" (UTC-3, sem horario de verao) --\n", rtcclock::TZ_BRASILIA);

  // Primeiro reproduz o que o firmware faz hoje: configTime(0, 0, ...) chama
  // setTimeZone(0, 0), que escreve exatamente esta string.
  setenv("TZ", "UTC0DST0", 1);
  tzset();
  const time_t t = 1790183100; // 2026-09-23 17:05:00Z
  struct tm antes;
  memset(&antes, 0, sizeof(antes));
  localtime_r(&t, &antes);
  std::printf("   com TZ=UTC0DST0 (o de hoje): %02d:%02d -> %s\n", antes.tm_hour, antes.tm_min,
              antes.tm_hour == 17 ? "e UTC, 3 h adiantado" : "?");
  checkEq(antes.tm_hour, 17, "TZ=UTC0DST0 mostra 17h (UTC), confirmando o bug");

  // Agora o fuso deste header.
  check(rtcclock::applyTimezone(), "applyTimezone() aceitou o fuso");
  std::printf("   TZ em vigor: \"%s\"\n", getenv("TZ") ? getenv("TZ") : "(nulo)");

  struct tm loc;
  memset(&loc, 0, sizeof(loc));
  localtime_r(&t, &loc);
  std::printf("   2026-09-23 17:05:00Z -> %04d-%02d-%02d %02d:%02d:%02d local\n",
              loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday, loc.tm_hour, loc.tm_min, loc.tm_sec);
  checkEq(loc.tm_year + 1900, 2026, "ano local");
  checkEq(loc.tm_mon + 1, 9, "mes local");
  checkEq(loc.tm_mday, 23, "dia local");
  checkEq(loc.tm_hour, 14, "hora local = 14 (17Z - 3)");
  checkEq(loc.tm_min, 5, "minuto local");

  // Virada de dia: 01:30Z do dia 1 e 22:30 do dia anterior em Brasilia.
  const time_t virada = rtcclock::epochFromUtc(2026, 1, 1, 1, 30, 0);
  struct tm v;
  memset(&v, 0, sizeof(v));
  localtime_r(&virada, &v);
  std::printf("   2026-01-01 01:30:00Z -> %04d-%02d-%02d %02d:%02d local\n", v.tm_year + 1900,
              v.tm_mon + 1, v.tm_mday, v.tm_hour, v.tm_min);
  checkEq(v.tm_year + 1900, 2025, "vira o ano para tras");
  checkEq(v.tm_mon + 1, 12, "mes 12");
  checkEq(v.tm_mday, 31, "dia 31");
  checkEq(v.tm_hour, 22, "hora 22");

  // Sem horario de verao: janeiro e julho tem o MESMO deslocamento. Se alguem
  // um dia colocar regra de DST aqui, este teste cai.
  const time_t janeiro = rtcclock::epochFromUtc(2026, 1, 15, 12, 0, 0);
  const time_t julho = rtcclock::epochFromUtc(2026, 7, 15, 12, 0, 0);
  struct tm tj, tl;
  memset(&tj, 0, sizeof(tj));
  memset(&tl, 0, sizeof(tl));
  localtime_r(&janeiro, &tj);
  localtime_r(&julho, &tl);
  checkEq(tj.tm_hour, 9, "15/jan 12:00Z -> 09:00 local");
  checkEq(tl.tm_hour, 9, "15/jul 12:00Z -> 09:00 local (sem DST)");
  check(tj.tm_isdst <= 0 && tl.tm_isdst <= 0, "tm_isdst nunca positivo");

  checkEq(rtcclock::utcOffsetSeconds(), -10800LL, "utcOffsetSeconds() = -10800");
  std::printf("   timezoneString() = \"%s\"\n", rtcclock::timezoneString());

  // O poll() rechama applyTimezone() de proposito: qualquer configTime() no
  // firmware reescreve TZ pelas costas. Simula esse sequestro e exige que o
  // fuso volte sozinho.
  setenv("TZ", "UTC0DST0", 1);
  tzset();
  struct tm sequestrado;
  memset(&sequestrado, 0, sizeof(sequestrado));
  localtime_r(&t, &sequestrado);
  checkEq(sequestrado.tm_hour, 17, "configTime() sequestra o TZ de volta para UTC");
  check(rtcclock::applyTimezone(), "applyTimezone() reconquista o fuso");
  struct tm devolta;
  memset(&devolta, 0, sizeof(devolta));
  localtime_r(&t, &devolta);
  checkEq(devolta.tm_hour, 14, "hora local volta a 14 depois do sequestro");

  // O caminho completo do boot: o que o BM8563 entrega -> epoch -> tela.
  const time_t doRtc = rtcclock::epochFromUtc(2026, 9, 23, 2, 30, 0); // guardado em UTC
  struct tm tela;
  memset(&tela, 0, sizeof(tela));
  localtime_r(&doRtc, &tela);
  char texto[32];
  snprintf(texto, sizeof(texto), "%02d/%02d/%04d  %02d:%02d:%02d", tela.tm_mday, tela.tm_mon + 1,
           tela.tm_year + 1900, tela.tm_hour, tela.tm_min, tela.tm_sec);
  std::printf("   drawHomeClock mostraria: \"%s\"\n", texto);
  check(strcmp(texto, "22/09/2026  23:30:00") == 0, "BM8563 02:30Z de 23/09 -> 23:30 de 22/09");
}

// ---------------------------------------------------------------------------
static void testStatus() {
  std::printf("\n-- rotulos da tela SISTEMA (ASCII) --\n");
  check(strcmp(rtcclock::sourceLabel(rtcclock::SOURCE_NONE), "SEM HORA") == 0, "SOURCE_NONE");
  check(strcmp(rtcclock::sourceLabel(rtcclock::SOURCE_RTC), "RTC") == 0, "SOURCE_RTC");
  check(strcmp(rtcclock::sourceLabel(rtcclock::SOURCE_NTP), "NTP") == 0, "SOURCE_NTP");
  bool ascii = true;
  const rtcclock::Source todos[3] = {rtcclock::SOURCE_NONE, rtcclock::SOURCE_RTC,
                                     rtcclock::SOURCE_NTP};
  for (int i = 0; i < 3; i++)
    for (const char *p = rtcclock::sourceLabel(todos[i]); *p; ++p)
      if ((unsigned char)*p > 0x7f)
        ascii = false;
  check(ascii, "nenhum byte acima de 0x7f nos rotulos");
  std::printf("   sizeof(rtcclock::State) = %zu bytes de SRAM\n", sizeof(rtcclock::State));
  check(sizeof(rtcclock::State) <= 40, "estado cabe em 40 bytes");
}

int main(void) {
  std::printf("== bancada do RtcClock.h (logica pura, sem BM8563) ==\n");
  testEpoch();
  testValidade();
  testFuso();
  testStatus();
  std::printf("\n%d checagens, %d falha(s)\n", checagens, falhas);
  return falhas ? 1 : 0;
}
