#pragma once
// ============================================================================
// Relogio do Fruit Jam Retro TV: NTP do ESP32-C6 + fuso do Brasil.
//
// O QUE MUDOU EM RELACAO AO M5 RETRO TV
//
// No Core2 havia um BM8563 com bateria: a hora sobrevivia sem Wi-Fi e o NTP so
// a corrigia. O Fruit Jam nao tem RTC nenhum (PORTING.md 1). A unica fonte de
// hora e o NTP que o proprio ESP32-C6 faz sozinho depois de conectar (o
// firmware NINA chama esp_sntp_init com 0/1/2.pool.ntp.org) e que chega aqui
// por net::ntpEpoch() -> WiFi.getTime().
//
// Consequencias:
//
//  1. Sem Wi-Fi a hora e DESCONHECIDA. Nada de chutar 1970 ou 2000: a fonte
//     fica SOURCE_NONE, synced()/hasTime() devolvem false e a interface mostra
//     "--:--". O time() do sistema continua abaixo de 2024, e os testes do
//     main.cpp (`time(nullptr) >= 1704067200`) seguem valendo sem mudanca.
//  2. Os servidores passados a configTimeBrazil() sao IGNORADOS: o NINA nao
//     expoe como troca-los. A assinatura fica para o main.cpp nao mudar.
//  3. A consulta ao NINA passa pelo SPI1, dividido com o radio, a previsao e o
//     radar sob net::Lock. Uma consulta HTTPS de outra tarefa segura esse lock
//     por segundos. Se o loop() perguntasse a hora direto, congelaria junto.
//     Por isso a pergunta roda numa tarefa curta (RTC_NTP) que consulta,
//     publica por std::atomic e morre — o mesmo padrao das tarefas
//     WEATHER_HTTP e RADAR_HTTPS (AGENTS.md 3.2). O loop() so le o atomico.
//
// COMO FUNCIONA
//
//   setup  -> begin()             aplica o fuso; nao ha hora ainda
//   rede   -> configTimeBrazil()  aplica o fuso e LIGA as consultas
//   loop   -> poll()              no maximo 1x/s: aplica a hora que a tarefa
//                                 trouxe (settimeofday) ou dispara outra
//
// Enquanto o NINA ainda nao sincronizou (WiFi.getTime() devolve 0), as
// consultas seguem com backoff de 2, 4, 8, 16, 32 e 60 s — nunca laco apertado
// criando tarefa (armadilha 4 do AGENTS.md). Depois do primeiro acerto, uma
// nova consulta a cada 6 h corrige a deriva do cristal do RP2350.
//
// O fuso e aplicado pela libc (newlib do arduino-pico), via TZ + tzset(). A
// conversao data->epoch continua em aritmetica pura (algoritmo de dias civis),
// sem mktime e sem mexer em TZ no meio do caminho.
//
// CUSTO
//
// Estado num unico `static` de funcao (< 40 bytes de SRAM) mais tres atomicos.
// A tarefa RTC_NTP existe so durante uma consulta (3 KB de pilha, transitorios).
//
// C++11 na parte pura (sim/probes/rtc_clock.cpp). ASCII puro no texto de tela.
// ============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(ARDUINO)
#define RTCCLOCK_HARDWARE 1
#include <Arduino.h>
#include <sys/time.h>

#include <atomic>

#include "UiLogic.h"     // timeReached(): millis() da a volta em ~49 dias
#include "fj/Net.h"      // net::ntpEpoch(), net::radioReady()
#include "fj/Platform.h" // xTaskCreatePinnedToCore com pilha em bytes
#else
// Compilacao nativa (sim/probes): so a logica pura fica disponivel.
#define RTCCLOCK_HARDWARE 0
#endif

namespace rtcclock {

// --------------------------------------------------------------------------
// Fuso horario
// --------------------------------------------------------------------------

// Horario de Brasilia, UTC-3, SEM horario de verao. O Brasil revogou o horario
// de verao em 2019 (Decreto 9.772/2019), entao nao existe regra de transicao a
// implementar — e inventar uma deixaria o relogio uma hora errado meio ano.
//
// Formato POSIX: o nome entre <> e obrigatorio porque "-03" comeca com sinal, e
// o numero apos o nome e quanto somar a hora LOCAL para chegar em UTC. Logo
// "3" significa UTC-3, nao UTC+3. Sem virgula depois = sem horario de verao.
//
// A forma entre <> e suportada pelo newlib (o parser de `_tzset_unlocked_r`
// compara com '<' e '>'), pelo glibc e pelo tzcode do macOS. Ainda assim
// `applyTimezone()` confere o resultado e cai para "BRT3", que e ASCII
// alfabetico puro, se o parser de alguma versao nao aceitar.
static const char TZ_BRASILIA[] = "<-03>3";
static const char TZ_BRASILIA_FALLBACK[] = "BRT3";

// Deslocamento esperado: -3 h. Serve de criterio do autoteste de TZ.
static const long UTC_OFFSET_SECONDS = -3L * 3600L;

// --------------------------------------------------------------------------
// Janela de plausibilidade
// --------------------------------------------------------------------------

// 2020-01-01T00:00:00Z. Abaixo disto a data e lixo: um ESP32-C6 que ainda nao
// sincronizou marca algo perto de 1970, e o NINA so zera abaixo de 2000. O
// firmware usa 1704067200 (2024) como "houve NTP"; aqui basta rejeitar o lixo.
static const time_t EPOCH_MIN = 1577836800; // 2020-01-01
static const time_t EPOCH_MAX = 4102444800; // 2100-01-01

// De quanto em quanto tempo reconsultar o NINA depois do primeiro acerto. O
// cristal do RP2350 (12 MHz, algumas dezenas de ppm) deriva menos de 1 s nesse
// intervalo; mais vezes so gastaria SPI1.
static const uint32_t RESYNC_INTERVAL_MS = 6UL * 3600UL * 1000UL; // 6 h
// Nome antigo (era a reescrita periodica do BM8563). Fica por compatibilidade.
static const uint32_t REWRITE_INTERVAL_MS = RESYNC_INTERVAL_MS;

// Tolerancia para reaplicar a hora numa ressincronizacao. O NINA devolve
// segundos inteiros; reaplicar abaixo disto so faria o relogio da tela pular
// para tras ate 1 s sem ganho nenhum.
static const int32_t STEP_TOLERANCE_S = 2;

// Backoff das consultas enquanto o NINA ainda nao tem hora: 2, 4, 8, 16, 32 e
// depois 60 s para sempre. C++11: constexpr de um unico return.
static const uint32_t QUERY_BACKOFF_MIN_MS = 2000;
static const uint32_t QUERY_BACKOFF_MAX_MS = 60000;
constexpr uint32_t queryBackoffMs(uint32_t attempt) {
  return attempt >= 5 ? QUERY_BACKOFF_MAX_MS : (QUERY_BACKOFF_MIN_MS << attempt);
}

// --------------------------------------------------------------------------
// Logica pura — testavel no PC (sim/probes/rtc_clock.cpp)
// --------------------------------------------------------------------------

inline int daysInMonth(int year, int month) {
  if (month < 1 || month > 12)
    return 0;
  if (month == 2)
    return ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 29 : 28;
  const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return table[month - 1];
}

// Dias civis desde 1970-01-01 (algoritmo days_from_civil de Howard Hinnant).
// Calendario gregoriano proleptico, sem tabela e sem laco.
inline int64_t daysFromCivil(int year, int month, int day) {
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = int64_t(year) - era * 400;                       // [0, 399]
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

// Data/hora **em UTC** -> epoch. Equivale a `timegm()`, que o newlib nao tem.
inline time_t epochFromUtc(int year, int month, int day, int hour, int minute, int second) {
  return time_t(daysFromCivil(year, month, day) * 86400LL + int64_t(hour) * 3600LL +
                int64_t(minute) * 60LL + int64_t(second));
}

// Criterio de uma data vinda de fora. Era o filtro da leitura do BM8563; fica
// porque a bancada nativa o exercita e porque qualquer fonte futura (GPS, RTC
// externo no STEMMA QT) precisa do mesmo crivo.
inline bool isPlausibleDate(int year, int month, int day, int hour, int minute, int second) {
  if (year < 2020 || year > 2099)
    return false;
  if (month < 1 || month > 12)
    return false;
  if (day < 1 || day > daysInMonth(year, month))
    return false;
  if (hour < 0 || hour > 23)
    return false;
  if (minute < 0 || minute > 59)
    return false;
  if (second < 0 || second > 60) // 60 cobre segundo bissexto
    return false;
  return true;
}

inline bool isPlausibleEpoch(time_t t) { return t >= EPOCH_MIN && t < EPOCH_MAX; }

// --------------------------------------------------------------------------
// Fonte da hora, para a tela SISTEMA
// --------------------------------------------------------------------------

enum Source : uint8_t {
  SOURCE_NONE = 0, // nao ha hora confiavel: a tela mostra --:--
  SOURCE_RTC = 1,  // nunca acontece no Fruit Jam (sem RTC); fica pelo rotulo
  SOURCE_NTP = 2   // acertada pelo NTP do ESP32-C6
};

// ASCII sem acento: vai direto para a tela.
inline const char *sourceLabel(Source s) {
  switch (s) {
  case SOURCE_RTC:
    return "RTC";
  case SOURCE_NTP:
    return "NTP";
  default:
    return "SEM HORA";
  }
}

// --------------------------------------------------------------------------
// Estado (num static de funcao inline: um por programa)
// --------------------------------------------------------------------------

struct State {
  time_t baseEpoch;     // epoch do ultimo acerto
  const char *tz;       // string de TZ que de fato vingou (ponteiro para literal)
  uint32_t baseMs;      // millis() correspondente
  uint32_t lastPollMs;
  uint32_t nextQueryMs; // quando disparar a proxima consulta ao NINA
  uint8_t attempt;      // consultas seguidas sem hora (indice do backoff)
  Source source;
  bool requested; // configTimeBrazil() ja foi chamado: pode consultar
  bool tzApplied;
};

inline State &state() {
  static State s = {0, NULL, 0, 0, 0, 0, SOURCE_NONE, false, false};
  return s;
}

inline Source source() { return state().source; }
inline const char *sourceLabel() { return sourceLabel(state().source); }
inline bool synced() { return state().source != SOURCE_NONE; }
// "Tem hora valida?" — o que as telas perguntam antes de desenhar HH:MM. Falso
// ate o primeiro acerto do NTP nesta sessao: sem RTC, reiniciar zera a hora.
inline bool hasTime() { return synced(); }
// Sem RTC na placa. Ficam para a tela SISTEMA e o `diag status` nao mudarem.
inline bool present() { return false; }
inline bool batteryLow() { return false; }
inline long utcOffsetSeconds() { return UTC_OFFSET_SECONDS; }
// String de TZ em vigor, para a tela SISTEMA e para o `diag status`.
inline const char *timezoneString() { return state().tz ? state().tz : TZ_BRASILIA; }

// --------------------------------------------------------------------------
// TZ
// --------------------------------------------------------------------------

// Confere se o TZ em vigor de fato coloca o horario local 3 h atras do UTC.
// Usa uma data de janeiro so para nao depender de nada: sem horario de verao o
// deslocamento e o mesmo o ano inteiro.
inline bool timezoneOffsetOk() {
  const time_t probe = 1767225600; // 2026-01-01T00:00:00Z
  struct tm local;
  memset(&local, 0, sizeof(local));
  if (!localtime_r(&probe, &local))
    return false;
  const time_t asUtc = epochFromUtc(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                    local.tm_hour, local.tm_min, local.tm_sec);
  return (long)(asUtc - probe) == UTC_OFFSET_SECONDS;
}

// Impoe o fuso do Brasil. Idempotente e barata: so mexe no ambiente se o TZ
// atual for outro (alguma biblioteca pode reescrever TZ pelas costas; o poll()
// rechama isto de proposito).
inline bool applyTimezone() {
  State &s = state();
  const char *current = getenv("TZ");
  // Caminho rapido, que e o do loop(): se o TZ do ambiente ainda e o que nos
  // instalamos, nao ha nada a fazer e o custo e um strcmp de 6 bytes.
  if (s.tzApplied && current && s.tz && strcmp(current, s.tz) == 0)
    return true;

  setenv("TZ", TZ_BRASILIA, 1);
  tzset();
  if (timezoneOffsetOk()) {
    s.tz = TZ_BRASILIA;
    s.tzApplied = true;
    return true;
  }
  // Parser de TZ sem suporte a <...>: cai para a forma alfabetica.
  setenv("TZ", TZ_BRASILIA_FALLBACK, 1);
  tzset();
  s.tz = TZ_BRASILIA_FALLBACK;
  s.tzApplied = timezoneOffsetOk();
  return s.tzApplied;
}

// --------------------------------------------------------------------------
// Aparelho: NTP do ESP32-C6 pelo NINA
// --------------------------------------------------------------------------

#if RTCCLOCK_HARDWARE

namespace detail {

// Handoff tarefa -> loop(). `phase` diz em que pe esta a consulta; `epoch` e
// `atMs` so valem quando phase == NTP_DONE (a tarefa grava os dois ANTES de
// publicar a fase, com release; o loop le a fase com acquire).
enum : uint8_t { NTP_IDLE = 0, NTP_RUNNING = 1, NTP_DONE = 2 };

struct Handoff {
  std::atomic<uint8_t> phase;
  std::atomic<uint32_t> epoch; // 0 = o NINA ainda nao tem hora
  std::atomic<uint32_t> atMs;  // millis() do instante da consulta
};

inline Handoff &handoff() {
  static Handoff h = {{NTP_IDLE}, {0}, {0}};
  return h;
}

// Pilha em BYTES (fj/Platform.h). net::ntpEpoch() e uma transacao SPI curta e
// nao aloca nada grande; 3 KB deixam folga para o driver do WiFiNINA.
static const uint32_t NTP_TASK_STACK = 3072;

inline void ntpTask(void *) {
  Handoff &h = handoff();
  // net::ntpEpoch() toma o net::Lock sozinho. Se outra tarefa estiver no meio
  // de um HTTPS, e AQUI que se espera — nunca no loop().
  const uint32_t e = net::ntpEpoch();
  h.atMs.store(millis(), std::memory_order_relaxed);
  h.epoch.store(e, std::memory_order_relaxed);
  h.phase.store(NTP_DONE, std::memory_order_release);
  vTaskDelete(nullptr);
}

inline void rebase(time_t epoch) {
  State &s = state();
  s.baseEpoch = epoch;
  s.baseMs = millis();
}

inline void scheduleRetry(uint32_t nowMs) {
  State &s = state();
  s.nextQueryMs = nowMs + queryBackoffMs(s.attempt);
  if (s.attempt < 5)
    ++s.attempt;
}

// Aplica o que a tarefa trouxe. Devolve true se o relogio do sistema mudou.
inline bool consume(uint32_t nowMs) {
  State &s = state();
  Handoff &h = handoff();
  const uint32_t e = h.epoch.load(std::memory_order_relaxed);
  const uint32_t at = h.atMs.load(std::memory_order_relaxed);
  h.phase.store(NTP_IDLE, std::memory_order_relaxed);

  if (!isPlausibleEpoch((time_t)e)) {
    // O NINA ainda nao sincronizou (ou a rede caiu antes): tenta de novo mais
    // tarde, com backoff. A hora que ja havia, se havia, continua valendo.
    if (s.source == SOURCE_NONE)
      scheduleRetry(nowMs);
    else
      s.nextQueryMs = nowMs + QUERY_BACKOFF_MAX_MS;
    return false;
  }
  s.attempt = 0;
  s.nextQueryMs = nowMs + RESYNC_INTERVAL_MS;

  // O epoch foi lido `nowMs - at` ms atras; desconta o atraso ate este poll().
  const uint32_t lag = uint32_t(nowMs - at);
  const time_t exact = (time_t)e + (time_t)(lag / 1000u);
  if (s.source == SOURCE_NTP) {
    const int64_t drift = int64_t(time(NULL)) - int64_t(exact);
    if (drift <= STEP_TOLERANCE_S && drift >= -STEP_TOLERANCE_S) {
      rebase(exact);
      return false; // ja estava certo: nao faz o relogio da tela tremer
    }
  }
  struct timeval tv;
  tv.tv_sec = exact;
  tv.tv_usec = (suseconds_t)((lag % 1000u) * 1000u);
  settimeofday(&tv, NULL);
  rebase(exact);
  s.source = SOURCE_NTP;
  return true;
}

} // namespace detail

// Chamar uma vez no setup(). Sem RTC nao ha o que ler: so aplica o fuso para
// que qualquer localtime_r ja saia em horario de Brasilia. Devolve false porque
// a hora continua desconhecida ate o NTP.
inline bool begin() {
  applyTimezone();
  return false;
}

// Nao ha RTC para gravar. Fica para nao quebrar quem chamava.
inline bool saveToRtc() { return false; }

// Chamado pelo main.cpp quando a rede sobe (era o substituto de configTime).
// Os servidores sao ignorados: quem faz NTP e o firmware NINA, com os servidores
// dele (pool.ntp.org). Nao bloqueia: so aplica o fuso e agenda a primeira
// consulta para o proximo poll().
inline void configTimeBrazil(const char *server1 = "pool.ntp.org",
                             const char *server2 = "time.cloudflare.com",
                             const char *server3 = NULL) {
  (void)server1;
  (void)server2;
  (void)server3;
  State &s = state();
  applyTimezone();
  if (!s.requested) {
    s.requested = true;
    s.attempt = 0;
    // Primeira consulta ja na proxima volta. O NINA costuma levar alguns
    // segundos depois da associacao para acertar; o backoff cobre isso.
    s.nextQueryMs = millis();
  }
}

// Chamar a cada volta do loop(). Fora da janela de 1 s custa um millis(). Nunca
// toca no SPI1: so le o que a tarefa RTC_NTP publicou, ou dispara uma nova.
// Devolve true quando acabou de acertar o relogio do sistema.
inline bool poll() {
  State &s = state();
  const uint32_t nowMs = millis();
  if (uint32_t(nowMs - s.lastPollMs) < 1000u)
    return false;
  s.lastPollMs = nowMs;

  applyTimezone();
  if (!s.requested)
    return false;

  detail::Handoff &h = detail::handoff();
  const uint8_t phase = h.phase.load(std::memory_order_acquire);
  if (phase == detail::NTP_DONE)
    return detail::consume(nowMs);
  if (phase == detail::NTP_RUNNING)
    return false; // a tarefa esta esperando o SPI1; paciencia
  if (!timeReached(nowMs, s.nextQueryMs))
    return false;

  // Coprocessador ainda nao respondeu ao boot: consultar agora so criaria uma
  // tarefa para receber zero.
  if (!net::radioReady()) {
    detail::scheduleRetry(nowMs);
    return false;
  }
  h.phase.store(detail::NTP_RUNNING, std::memory_order_relaxed);
  // Nucleo 1, prioridade 1: fora do nucleo do loop() e do DVI, e acima das
  // tarefas ociosas para nao ficar para tras de um laco de prioridade 0.
  if (xTaskCreatePinnedToCore(detail::ntpTask, "RTC_NTP", detail::NTP_TASK_STACK, nullptr, 1,
                              nullptr, 1) != pdPASS) {
    h.phase.store(detail::NTP_IDLE, std::memory_order_relaxed);
    detail::scheduleRetry(nowMs); // sem memoria para a pilha: tenta depois
  }
  return false;
}

#else // !RTCCLOCK_HARDWARE — compilacao nativa: so a logica pura

inline bool begin() { return false; }
inline bool saveToRtc() { return false; }
inline bool poll() { return false; }
inline void configTimeBrazil(const char * = "pool.ntp.org", const char * = "time.cloudflare.com",
                             const char * = NULL) {
  applyTimezone();
}

#endif // RTCCLOCK_HARDWARE

} // namespace rtcclock
