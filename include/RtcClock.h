#pragma once
// ============================================================================
// Relogio do M5 RETRO TV: BM8563 (RTC com bateria) + NTP + fuso do Brasil.
//
// O PROBLEMA QUE ISTO RESOLVE
//
// Antes deste header o relogio vinha so do NTP, por `configTime(0, 0, ...)`.
// Duas consequencias:
//
//  1. Sem Wi-Fi o epoch fica em 1970 e a tela inicial mostra o aviso de nao
//     sincronizado, mesmo com o BM8563 da placa marcando a hora certa desde o
//     boot anterior.
//  2. `configTime(0, 0, ...)` chama `setTimeZone(0, 0)`, que escreve
//     TZ="UTC0DST0" e roda `tzset()` (ver esp32-hal-time.c do arduino-esp32
//     2.x). Ou seja, `localtime_r()` devolvia **UTC**: o relogio da tela
//     adiantava 3 horas em relacao ao horario de Brasilia.
//
// COMO FUNCIONA
//
// Convencao: o BM8563 guarda **UTC**, nunca hora local. E a mesma convencao do
// M5Unified (`RTC_Class::setSystemTimeFromRtc()` forca GMT0 no `mktime`), entao
// quem usar a outra API continua enxergando a mesma coisa. Toda a conversao
// para horario de Brasilia e feita pela libc, via TZ.
//
//   boot   -> begin()  le o BM8563, valida e semeia o relogio com settimeofday
//   loop   -> poll()   detecta o acerto do NTP e devolve a hora ao BM8563
//
// POR QUE NAO USAR `M5.Rtc.setSystemTimeFromRtc()` DIRETO
//
// O `M5.begin()` ja chama essa funcao (M5Unified.cpp, `_begin_rtc_imu`), mas
// ela nao serve sozinha aqui:
//
//  - nao valida nada: BM8563 zerado (placa nova ou bateria morta) vira
//    01/01/2000 e o firmware passaria a exibir uma data falsa com a mesma
//    confianca de uma data boa;
//  - ela troca TZ para "GMT0" para forcar `mktime` em UTC, restaura o valor
//    antigo com `setenv`/`unsetenv` e **nao chama `tzset()` de novo**. O estado
//    interno da libc fica em GMT0 ate alguem chamar `tzset()`;
//  - `getenv("TZ")` seguido de `setenv("TZ", ...)` pode invalidar o ponteiro
//    devolvido pelo `getenv`.
//
// Aqui a conversao data->epoch e aritmetica pura (algoritmo de dias civis), sem
// `mktime` e sem mexer em TZ no meio do caminho.
//
// CUSTO
//
// Nenhuma alocacao. O estado mora num unico `static` de funcao: 24 bytes de
// SRAM. Nada aqui bloqueia o boot — leitura e escrita do BM8563 sao quatro
// transacoes I2C curtas no barramento interno, e qualquer falha so faz a fonte
// ficar SOURCE_NONE.
//
// C++11 (-std=gnu++11). ASCII puro no texto de interface.
// ============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#define RTCCLOCK_HARDWARE 1
#include <Arduino.h>
#include <M5Unified.h>
#include <sys/time.h>
#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#define RTCCLOCK_HAS_SNTP 1
#endif
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
// A forma entre <> e suportada pelo newlib do toolchain xtensa-esp32 (o parser
// de `_tzset_unlocked_r` compara com '<' e '>'), pelo glibc e pelo tzcode do
// macOS. Ainda assim `applyTimezone()` confere o resultado e cai para "BRT3",
// que e ASCII alfabetico puro, se algum dia o parser mudar.
static const char TZ_BRASILIA[] = "<-03>3";
static const char TZ_BRASILIA_FALLBACK[] = "BRT3";

// Deslocamento esperado: -3 h. Serve de criterio do autoteste de TZ.
static const long UTC_OFFSET_SECONDS = -3L * 3600L;

// --------------------------------------------------------------------------
// Janela de plausibilidade
// --------------------------------------------------------------------------

// 2020-01-01T00:00:00Z. Abaixo disto a data e lixo: um BM8563 zerado marca
// 2000-01-01 e um relogio sem `settimeofday` marca 1970-01-01. O firmware ja
// usa 1704067200 (2024) como "houve NTP"; aqui o limiar e mais baixo de
// proposito, porque uma data de 2021 vinda do RTC ainda e melhor que nada.
static const time_t EPOCH_MIN = 1577836800; // 2020-01-01
static const time_t EPOCH_MAX = 4102444800; // 2100-01-01

// De quanto em quanto tempo reescrever o BM8563 enquanto o NTP estiver vivo.
static const uint32_t REWRITE_INTERVAL_MS = 6UL * 3600UL * 1000UL; // 6 h

// Tolerancia para considerar que alguem pulou o relogio (o NTP acertou).
static const int32_t STEP_TOLERANCE_S = 2;

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

// Uma leitura do BM8563 so e aceita se passar por aqui. Um chip sem bateria
// devolve 2000-01-01, e um chip com o barramento ruim devolve campos fora de
// faixa; os dois casos caem fora.
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
  if (second < 0 || second > 60) // 60 cobre segundo bissexto lido do chip
    return false;
  return true;
}

inline bool isPlausibleEpoch(time_t t) { return t >= EPOCH_MIN && t < EPOCH_MAX; }

// --------------------------------------------------------------------------
// Fonte da hora, para a tela SISTEMA
// --------------------------------------------------------------------------

enum Source : uint8_t {
  SOURCE_NONE = 0, // nao ha hora confiavel
  SOURCE_RTC = 1,  // semeada pelo BM8563 no boot
  SOURCE_NTP = 2   // acertada pela rede (e ja devolvida ao BM8563)
};

// ASCII sem acento: vai direto para a tela composta.
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
// Estado (24 bytes de SRAM, num static de funcao inline: um por programa)
// --------------------------------------------------------------------------

struct State {
  time_t baseEpoch;   // epoch da ultima vez que sabiamos a hora
  const char *tz;     // string de TZ que de fato vingou (ponteiro para literal)
  uint32_t baseMs;    // millis() correspondente, para detectar o pulo do NTP
  uint32_t lastPollMs;
  uint32_t lastWriteMs;
  Source source;
  bool present;    // o BM8563 respondeu ao I2C
  bool batteryLow; // flag VL do BM8563: a hora guardada pode ter sido perdida
  bool tzApplied;
};

inline State &state() {
  static State s = {0, NULL, 0, 0, 0, SOURCE_NONE, false, false, false};
  return s;
}

inline Source source() { return state().source; }
inline const char *sourceLabel() { return sourceLabel(state().source); }
inline bool synced() { return state().source != SOURCE_NONE; }
inline bool present() { return state().present; }
inline bool batteryLow() { return state().batteryLow; }
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
// atual for outro. Precisa ser rechamada depois de qualquer `configTime()`,
// que reescreve TZ para "UTC0DST0" pelas costas.
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
// Hardware: BM8563 via M5Unified 0.2.10 (m5::RTC_Class, driver PCF8563, 0x51)
// --------------------------------------------------------------------------

#if RTCCLOCK_HARDWARE

namespace detail {

inline void rebase(time_t epoch) {
  State &s = state();
  s.baseEpoch = epoch;
  s.baseMs = millis();
}

#if defined(RTCCLOCK_HAS_SNTP)
// Roda na tarefa do SNTP, nao no loop(). Aqui **so** se levanta a bandeira:
// I2C e desenho ficam para o poll(), no loop(), como manda o resto do firmware.
inline volatile bool &ntpFlag() {
  static volatile bool flag = false;
  return flag;
}
inline void onNtpSync(struct timeval *tv) {
  (void)tv;
  ntpFlag() = true;
}
#endif

} // namespace detail

// Grava o relogio do sistema (convertido para UTC) no BM8563. Devolve false se
// o chip nao respondeu ou se a hora do sistema ainda nao presta.
inline bool saveToRtc() {
  State &s = state();
  if (!M5.Rtc.isEnabled())
    return false;
  const time_t now = time(NULL);
  if (!isPlausibleEpoch(now))
    return false;
  struct tm utc;
  memset(&utc, 0, sizeof(utc));
  if (!gmtime_r(&now, &utc))
    return false;
  // rtc_datetime_t(const tm&) preenche data e hora, inclusive tm_wday.
  m5::rtc_datetime_t dt(utc);
  M5.Rtc.setDateTime(&dt);
  s.lastWriteMs = millis();
  return true;
}

// Chamar uma vez no setup(), depois de M5.begin() (que e quem inicializa o
// barramento I2C interno e o driver do BM8563). Nao bloqueia: se o chip nao
// responder, a funcao devolve false e o firmware segue com SOURCE_NONE.
inline bool begin() {
  State &s = state();
  applyTimezone();

#if defined(RTCCLOCK_HAS_SNTP)
  // Registrado antes do configTime: nem sntp_stop() nem sntp_init() limpam o
  // callback, entao a ordem nao importa — mas registrar cedo evita perder o
  // primeiro acerto se a rede subir rapido.
  sntp_set_time_sync_notification_cb(detail::onNtpSync);
#endif

  // M5.begin() ja chamou M5.Rtc.begin() (cfg.internal_rtc e true por padrao).
  // Isto aqui e so a rede de seguranca para quem mudar a config; sao duas
  // transacoes I2C com timeout proprio do driver, nao ha espera indefinida.
  if (!M5.Rtc.isEnabled())
    M5.Rtc.begin(&M5.In_I2C, M5.getBoard());
  if (!M5.Rtc.isEnabled()) {
    s.present = false;
    s.source = SOURCE_NONE;
    return false;
  }
  s.present = true;
  s.batteryLow = M5.Rtc.getVoltLow();

  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) {
    s.source = SOURCE_NONE;
    return false;
  }
  if (!isPlausibleDate(dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes,
                       dt.time.seconds)) {
    // Placa nova, bateria morta ou leitura corrompida: melhor continuar sem
    // hora (a tela ja sabe avisar) que exibir 01/01/2000 com ar de verdade.
    s.source = SOURCE_NONE;
    return false;
  }

  const time_t epoch = epochFromUtc(dt.date.year, dt.date.month, dt.date.date, dt.time.hours,
                                    dt.time.minutes, dt.time.seconds);
  if (!isPlausibleEpoch(epoch)) {
    s.source = SOURCE_NONE;
    return false;
  }
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  detail::rebase(epoch);
  s.source = SOURCE_RTC;
  return true;
}

// Substitui `configTime(0, 0, ...)`: mesmo SNTP, mas o TZ que fica valendo e o
// do Brasil, e nao "UTC0DST0".
inline void configTimeBrazil(const char *server1 = "pool.ntp.org",
                             const char *server2 = "time.cloudflare.com",
                             const char *server3 = NULL) {
  configTzTime(TZ_BRASILIA, server1, server2, server3);
  // configTzTime escreve o TZ cru; a checagem devolve o fallback se preciso.
  applyTimezone();
}

// Chamar a cada volta do loop(). Custa um millis() e um strcmp fora da janela
// de 1 s. Devolve true quando acabou de gravar a hora no BM8563.
inline bool poll() {
  State &s = state();
  const uint32_t nowMs = millis();
  if (uint32_t(nowMs - s.lastPollMs) < 1000u)
    return false;
  s.lastPollMs = nowMs;

  // Se alguem chamou configTime() no meio do caminho, o TZ virou UTC0DST0.
  applyTimezone();

  const time_t now = time(NULL);
  if (!isPlausibleEpoch(now))
    return false;

  bool stepped = false;
#if defined(RTCCLOCK_HAS_SNTP)
  if (detail::ntpFlag()) {
    detail::ntpFlag() = false;
    stepped = true;
  }
#endif
  if (!stepped) {
    if (s.source == SOURCE_NONE) {
      // Nao havia hora nenhuma e agora ha: so pode ter vindo da rede.
      stepped = true;
    } else {
      // O relogio do sistema avanca com o mesmo tick do millis(); divergir
      // acima da tolerancia quer dizer que alguem chamou settimeofday().
      const int64_t elapsed = int64_t(uint32_t(nowMs - s.baseMs)) / 1000;
      const int64_t drift = int64_t(now) - (int64_t(s.baseEpoch) + elapsed);
      if (drift > STEP_TOLERANCE_S || drift < -STEP_TOLERANCE_S)
        stepped = true;
    }
  }

  if (stepped) {
    s.source = SOURCE_NTP;
    detail::rebase(now);
    return saveToRtc();
  }
  // Reescrita periodica: mantem o BM8563 disciplinado numa sessao longa.
  if (s.source == SOURCE_NTP && uint32_t(nowMs - s.lastWriteMs) >= REWRITE_INTERVAL_MS) {
    detail::rebase(now);
    return saveToRtc();
  }
  return false;
}

#else // !RTCCLOCK_HARDWARE — compilacao nativa: so a logica pura

inline bool begin() { return false; }
inline bool saveToRtc() { return false; }
inline bool poll() { return false; }

#endif // RTCCLOCK_HARDWARE

} // namespace rtcclock
