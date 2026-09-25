// ============================================================================
//  The Weather Channel — Local Forecast (clone anos 80)
//  Adafruit Fruit Jam (RP2350B) — DVI 640x480 + áudio pelo TLV320DAC3100
//
//  VÍDEO                      : DVI pelo HSTX (GPIO 12..19), quadro lógico de
//                               320x240 RGB565 dobrado nos dois eixos. O canvas
//                               global `tv` (fj/Display.h) É o framebuffer.
//  ÁUDIO EM LOOP CONTÍNUO     : /M5RETRO/weather_music.wav
//                               (PCM 16-bit estéreo 22050 Hz) -> TLV320DAC3100,
//                               saída P2 (fone), que é a que vai à TV. O DVI
//                               não leva áudio.
//  CARTÃO                     : microSD em SDIO próprio (fj/Storage.h)
//  WI-FI (TODO: preencher)    : WIFI_SSID / WIFI_PASS abaixo; ESP32-C6 com
//                               firmware NINA, TLS feito dentro dele
//  LOCALIDADE                 : Franca - SP (Open-Meteo, API pública sem chave)
//
//  A camada de plataforma (include/fj, src/fj) é a mesma do firmware principal,
//  compilada a partir da raiz — ver platformio.ini. O resto deste arquivo
//  duplica de propósito rotinas do principal (AGENTS.md §1).
// ============================================================================

#include <Arduino.h>
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>
#include <SD.h>

#include "fj/AudioOut.h"
#include "fj/Board.h"
#include "fj/Display.h"
#include "fj/Net.h"
#include "fj/Platform.h"
#include "fj/Storage.h"

// ----------------------------------------------------------------------------
//  Configurações fixas
// ----------------------------------------------------------------------------

// TODO: preencher com as credenciais da rede Wi-Fi local.
#define WIFI_SSID "..."   // TODO: preencher SSID
#define WIFI_PASS "..."   // TODO: preencher senha

// Localidade fixa: Franca - SP. Fonte: Open-Meteo (api.open-meteo.com), API
// pública, sem cadastro nem chave. Substituiu o wttr.in, cujo j1 devolve ~39 KB
// e estourava a RAM/buffer do ESP32; esta resposta tem ~800 bytes.
static const char *WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=-20.5386&longitude=-47.4008"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,"
    "wind_direction_10m&daily=weather_code,temperature_2m_max,temperature_2m_min"
    "&timezone=America%2FSao_Paulo&forecast_days=3";

// ---- Áudio (TLV320DAC3100) ----
static const char *WAV_PATH = "/M5RETRO/weather_music.wav";
// Volume em duas etapas, como no firmware principal: o DAC no máximo e a
// atenuação em software sobre o PCM (scalePcm), que é linear em amplitude —
// o volume do DAC é em dB e não daria os mesmos 80% do original.
static constexpr int AUDIO_VOLUME = 80;   // 0..100
// P2 é a saída que vai à entrada de áudio da TV; o original só tocava pelo RCA.
static constexpr audioout::Route AUDIO_ROUTE = audioout::Route::HEADPHONE;

// ---- Dimensões e cadências ----
static constexpr int CRT_W = display::W, CRT_H = display::H;
// Área segura: uma TV CRT corta cerca de 7% de cada borda (overscan). No DVI um
// monitor mostra o raster inteiro, mas TV ligada por HDMI costuma reaplicar o
// overscan; por isso a caixa segura continua valendo. O fundo continua
// sangrando até a borda; texto, linhas e faixas ficam dentro desta caixa.
static constexpr int SAFE_X = 24, SAFE_Y = 18;
static constexpr int SAFE_L = SAFE_X;          // 24
static constexpr int SAFE_T = SAFE_Y;          // 18
static constexpr int SAFE_R = CRT_W - SAFE_X;  // 296
static constexpr int SAFE_B = CRT_H - SAFE_Y;  // 222
static constexpr int SAFE_W = SAFE_R - SAFE_L; // 272
static constexpr size_t AUDIO_CHUNK = 1024;          // bytes por bloco PCM
// Quanto a tarefa de áudio pode correr à frente do relógio PCM. O ritmo de
// verdade vem do audioout::write(), que bloqueia com o buffer do I2S cheio; este
// teto só pega o caso em que ele não bloqueia (DAC fora, implementação
// provisória) e impede a tarefa de ler o cartão em laço apertado.
static constexpr int64_t AUDIO_LEAD_US = 100000;
static constexpr uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL; // 10 min
static constexpr uint32_t WEATHER_RETRY_MS = 30UL * 1000UL;          // retenta falha
static constexpr uint32_t TICKER_INTERVAL_MS = 33;  // ~30 fps para o ticker
static constexpr int TICKER_STEP = 2;               // desloca 2 px por tick
static constexpr int TICKER_H = 16;                 // altura da fonte Font2
static constexpr int TICKER_Y = SAFE_B - TICKER_H;  // 206

// ----------------------------------------------------------------------------
//  Rotinas de WAV (reaproveitadas do padrão já validado em PlaybackIO.h,
//  copiadas aqui para manter o projeto autocontido).
// ----------------------------------------------------------------------------
namespace playback {

struct WavInfo {
  uint32_t rate = 0, start = 0, end = 0;
  uint16_t channels = 0, align = 0;
};

inline uint32_t le32(const uint8_t *b) {
  return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
         (uint32_t(b[3]) << 24);
}
inline uint16_t le16(const uint8_t *b) {
  return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

// Valida o cabeçalho e localiza o bloco "data" de um WAV PCM 16-bit
// estéreo 22050 Hz (exatamente o formato de weather_music.wav).
template <class Stream> bool readWav(Stream &file, WavInfo &info) {
  uint8_t riff[12];
  if (!file.seek(0) || file.read(riff, 12) != 12 || memcmp(riff, "RIFF", 4) ||
      memcmp(riff + 8, "WAVE", 4))
    return false;
  const uint64_t end = uint64_t(le32(riff + 4)) + 8;
  if (end < 12 || end > file.size())
    return false;
  bool formatted = false;
  while (uint64_t(file.position()) + 8 <= end) {
    uint8_t chunk[8];
    if (file.read(chunk, 8) != 8)
      return false;
    const uint32_t size = le32(chunk + 4), start = file.position();
    const uint64_t next = uint64_t(start) + size + (size & 1U);
    if (next > end)
      return false;
    if (!memcmp(chunk, "fmt ", 4)) {
      uint8_t fmt[16];
      if (size < 16 || file.read(fmt, 16) != 16)
        return false;
      info.channels = le16(fmt + 2);
      info.rate = le32(fmt + 4);
      info.align = le16(fmt + 12);
      if (le16(fmt) != 1 || info.channels != 2 || info.rate != 22050 ||
          info.align != 4 || le16(fmt + 14) != 16 || le32(fmt + 8) != 88200)
        return false;
      formatted = true;
    } else if (!memcmp(chunk, "data", 4)) {
      if (!formatted || !size || size % info.align)
        return false;
      info.start = start;
      info.end = start + size;
      return true;
    }
    if (!file.seek(uint32_t(next)))
      return false;
  }
  return false;
}

inline void scalePcm(int16_t *samples, size_t count, int volume) {
  if (volume < 0)
    volume = 0;
  if (volume > 100)
    volume = 100;
  for (size_t i = 0; i < count; ++i)
    samples[i] = int16_t(int32_t(samples[i]) * volume / 100);
}

} // namespace playback

// ----------------------------------------------------------------------------
//  Estado compartilhado
// ----------------------------------------------------------------------------

// Sprite do ticker, desenhado fora da tela e copiado para o `tv` de uma vez:
// o `tv` é o framebuffer que o DVI está varrendo, e apagar a faixa e escrever
// o texto direto nele faria o ticker piscar. Mesma profundidade do painel
// (RGB565), senão a cor sai errada em silêncio (AGENTS.md §7, armadilha 10).
LGFX_Sprite ticker(&tv);

SemaphoreHandle_t sdMutex = nullptr;
File wavFile;
TaskHandle_t audioTaskHandle = nullptr;

uint32_t sampleRate = 22050, wavDataStart = 0, wavDataEnd = 0;
uint16_t wavBlockAlign = 4;

// Bloco PCM da tarefa de áudio. Global, e não local da tarefa: 1 KB a menos na
// pilha dela. O audioout::write() copia para o buffer do I2S, então um bloco só
// basta (o original girava três porque o i2s_write do ESP-IDF pedia memória DMA).
int16_t audioBlock[AUDIO_CHUNK / sizeof(int16_t)];

std::atomic<uint32_t> audioUnderruns{0};

// Dados meteorológicos em buffer duplo. A tarefa HTTP escreve no buffer
// "não exibido" e só então troca weatherActive (publica) — sem tearing.
struct DayForecast {
  char name[8];   // "DOM" / "SEG" / "TER" ...
  int maxC = 0;
  int minC = 0;
  char cond[24];  // condição ASCII normalizada (ex.: "Sunny")
};

struct WeatherData {
  int tempC = 0;
  int humidity = 0;
  int windKmph = 0;
  char cond[24] = {0};
  char windDir[8] = {0};
  DayForecast days[3];
};

WeatherData weatherShadows[2];
std::atomic<int> weatherActive{0};
std::atomic<uint32_t> weatherVersion{0};
std::atomic<bool> weatherValid{false};
std::atomic<bool> weatherBusy{false};

// Escritos pela tarefa HTTP e lidos pelo loop(), que aqui rodam em núcleos
// diferentes (no Core2 os dois ficavam no core 1). Por isso o status virou um
// ponteiro atômico para literal, em vez de um char[] reescrito por snprintf que
// o loop poderia ler pela metade.
std::atomic<const char *> weatherStatus{"CARREGANDO..."};
std::atomic<uint32_t> lastWeatherGood{0};
uint32_t lastWeatherAttempt = 0;   // só o loop() mexe

uint32_t lastDrawnVersion = UINT32_MAX;
uint32_t lastStatusRedraw = 0;

String tickerPayload;   // texto base da previsão estendida
String tickerFull;      // payload + lacuna + payload (loop contínuo)
int tickerPayloadW = 0;
int tickerGapW = 0;
int tickerWrapAt = 0;
int tickerOffsetPx = 0;
uint32_t lastTickerMs = 0;

static const char TICKER_GAP[] = "      ";

// ----------------------------------------------------------------------------
//  Helpers de texto (as fontes bitmap são ASCII-only)
// ----------------------------------------------------------------------------

static void asciiCopy(char *dst, size_t cap, const char *src) {
  size_t j = 0;
  if (src) {
    for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
      unsigned char c = (unsigned char)src[i];
      if (c >= 0x20 && c < 0x7F)   // mantém somente ASCII imprimível
        dst[j++] = c;
    }
  }
  dst[j] = 0;
}

static void asciiUpper(char *s) {
  for (; *s; ++s)
    if (*s >= 'a' && *s <= 'z')
      *s -= 'a' - 'A';
}

// Dia da semana (0=DOM .. 6=SAB) a partir de "AAAA-MM-DD" (Zeller). -1 se inválido.
static int weekdayFromIso(const char *iso) {
  int y = 0, m = 0, d = 0;
  if (sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3)
    return -1;
  if (m < 3) {
    m += 12;
    --y;
  }
  int K = y % 100, J = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  return (h + 6) % 7;
}

static const char *weekdayPt(int wd) {
  static const char *W[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
  return (wd >= 0 && wd <= 6) ? W[wd] : "???";
}

// ----------------------------------------------------------------------------
//  Consulta HTTP (Open-Meteo) + parse com ArduinoJson 7
// ----------------------------------------------------------------------------

// Código WMO (ww) do Open-Meteo -> condição em português, já em ASCII maiúsculo.
static const char *wmoConditionPt(int code) {
  switch (code) {
  case 0: return "CEU LIMPO";
  case 1: return "POUCAS NUVENS";
  case 2: return "PARCIAL NUBLADO";
  case 3: return "NUBLADO";
  case 45: case 48: return "NEVOEIRO";
  case 51: return "GAROA FRACA";
  case 53: return "GAROA";
  case 55: return "GAROA FORTE";
  case 56: case 57: return "GAROA CONGELANTE";
  case 61: return "CHUVA FRACA";
  case 63: return "CHUVA";
  case 65: return "CHUVA FORTE";
  case 66: case 67: return "CHUVA CONGELANTE";
  case 71: return "NEVE FRACA";
  case 73: return "NEVE";
  case 75: return "NEVE FORTE";
  case 77: return "GRAOS DE NEVE";
  case 80: return "PANCADAS FRACAS";
  case 81: return "PANCADAS DE CHUVA";
  case 82: return "PANCADAS FORTES";
  case 85: case 86: return "PANCADAS DE NEVE";
  case 95: return "TROVOADA";
  case 96: case 99: return "TROVOADA C/ GRANIZO";
  default: return "INDISPONIVEL";
  }
}

// Direção do vento em graus -> rosa de 16 pontos em português (L = leste).
static const char *windDirPt(float deg) {
  static const char *P[16] = {"N",  "NNE", "NE", "ENE", "L",  "ESE", "SE", "SSE",
                              "S",  "SSO", "SO", "OSO", "O",  "ONO", "NO", "NNO"};
  if (!(deg >= 0.0f))
    deg = 0.0f;
  int i = (int)((deg + 11.25f) / 22.5f) % 16;
  return P[i];
}

static bool parseWeather(JsonDocument &doc, WeatherData &out) {
  JsonObject cur = doc["current"];
  if (cur.isNull() || !cur["temperature_2m"].is<float>())
    return false;
  out.tempC = (int)lroundf(cur["temperature_2m"] | 0.0f);
  out.humidity = (int)lroundf(cur["relative_humidity_2m"] | 0.0f);
  out.windKmph = (int)lroundf(cur["wind_speed_10m"] | 0.0f);
  snprintf(out.cond, sizeof(out.cond), "%s", wmoConditionPt(cur["weather_code"] | -1));
  snprintf(out.windDir, sizeof(out.windDir), "%s",
           windDirPt(cur["wind_direction_10m"] | 0.0f));

  JsonObject daily = doc["daily"];
  if (daily.isNull())
    return false;
  JsonArray date = daily["time"], code = daily["weather_code"],
            tmax = daily["temperature_2m_max"], tmin = daily["temperature_2m_min"];
  if (date.isNull() || tmax.isNull() || tmin.isNull() || date.size() < 3 ||
      tmax.size() < 3 || tmin.size() < 3)
    return false;
  for (int i = 0; i < 3; ++i) {
    DayForecast &f = out.days[i];
    snprintf(f.name, sizeof(f.name), "%s", weekdayPt(weekdayFromIso(date[i] | "")));
    f.maxC = (int)lroundf(tmax[i] | 0.0f);
    f.minC = (int)lroundf(tmin[i] | 0.0f);
    snprintf(f.cond, sizeof(f.cond), "%s",
             wmoConditionPt(code.isNull() ? -1 : (code[i] | -1)));
  }
  return true;
}

// Conecta ao Wi-Fi se preciso. Só roda na tarefa HTTP: o WiFi.begin() do
// WiFiNINA bloqueia até ~50 s (10 tentativas de 5 s) esperando o ESP32-C6
// associar, e no loop() isso congelaria o ticker. O net::Lock fica tomado a
// espera inteira — não há outro usuário do SPI1 neste projeto.
static bool ensureWiFi() {
  if (!net::radioReady())
    return false;
  net::Lock lock;
  if (WiFi.status() == WL_CONNECTED)
    return true;
  Serial.println("[WEATHER] conectando Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  return WiFi.status() == WL_CONNECTED;
}

static bool fetchWeather(WeatherData &out) {
  // ~800 bytes: o net::httpGet lê tudo para um buffer contíguo (o leitor Stream
  // do ArduinoJson 7.4.2 descartava escalares no ESP32) e marca `truncated`
  // quando não coube — aí a resposta é rejeitada em vez de publicar dados pela
  // metade. O buffer fica na PSRAM, não na pilha da tarefa (AGENTS.md §2.3).
  constexpr size_t cap = 4096;
  char *buf = (char *)ps_malloc(cap);
  if (!buf) {
    Serial.println("[WEATHER] sem memoria para a resposta");
    return false;
  }
  bool ok = false;
  // TLS dentro do ESP32-C6, validado pelo bundle de raízes do NINA; o RP2350
  // não faz handshake nenhum (fj/Net.h).
  const net::HttpResult r = net::httpGet(WEATHER_URL, buf, cap, 10000);
  if (r.status != 200) {
    Serial.printf("[WEATHER] HTTP %d\n", r.status);
  } else if (r.truncated) {
    Serial.println("[WEATHER] resposta maior que o buffer");
  } else if (r.length) {
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, buf, r.length, DeserializationOption::NestingLimit(8));
    if (!err)
      ok = parseWeather(doc, out);
    else
      Serial.printf("[WEATHER] parse %s (%u bytes)\n", err.c_str(), (unsigned)r.length);
  } else {
    Serial.println("[WEATHER] corpo vazio");
  }
  free(buf);
  return ok;
}

// Tarefa de baixa prioridade no core 1: conecta se preciso, faz o GET e
// publica o resultado.
static void weatherTask(void *arg) {
  int writeIndex = (int)(intptr_t)arg;
  WeatherData local;
  memset(&local, 0, sizeof(local));

  bool ok = false;
  const char *failure = "ERRO NA CONSULTA";
  if (!ensureWiFi())
    failure = net::radioReady() ? "SEM WI-FI" : "SEM RADIO WI-FI";
  else
    ok = fetchWeather(local);

  if (ok) {
    weatherShadows[writeIndex] = local;   // escreve no buffer não exibido
    weatherActive.store(writeIndex);      // publica
    weatherValid.store(true);
    weatherVersion.fetch_add(1);
    lastWeatherGood.store(millis());
    weatherStatus.store("ATUALIZADO");
    Serial.printf("[WEATHER] atualizado: %dC %d%% %s\n", local.tempC,
                  local.humidity, local.cond);
  } else {
    weatherStatus.store(failure);
    Serial.printf("[WEATHER] falha: %s\n", failure);
    if (!weatherValid.load())
      weatherVersion.fetch_add(1);   // força redesenho do estado de erro
  }
  weatherBusy.store(false);
  vTaskDelete(nullptr);
}

static void pollWeather() {
  if (weatherBusy.load())
    return;
  // Sem consulta ao WiFi.status() aqui: toda chamada WiFiNINA precisa do
  // net::Lock, que a tarefa HTTP segura durante a conexão e o GET inteiros, e o
  // loop() ficaria parado atrás dela. A tarefa descobre sozinha que não há rede
  // e publica "SEM WI-FI"; o backoff abaixo evita que isso vire laço.
  const uint32_t now = millis();
  // O backoff vale para toda tentativa, não só antes da primeira que der certo.
  // Antes, com dados já em mãos, o único portão era lastWeatherGood: se a
  // consulta falhasse ele não avançava, a condição seguia falsa e cada loop()
  // criava outra tarefa HTTP de 8 KB — dezenas por segundo com o roteador fora.
  if (lastWeatherAttempt != 0 && now - lastWeatherAttempt < WEATHER_RETRY_MS)
    return;
  if (weatherValid.load() && now - lastWeatherGood.load() < WEATHER_REFRESH_MS)
    return;   // cadência normal de 10 minutos
  lastWeatherAttempt = now;
  weatherBusy.store(true);
  int next = 1 - weatherActive.load();   // buffer oposto ao exibido
  // 8 KB de pilha (em bytes; fj/Platform.h converte para palavras). O TLS saiu
  // do processador, então o aperto do mbedTLS não existe mais, mas o parse do
  // ArduinoJson e os printf ainda empilham quadros, e a SRAM do RP2350 tem folga.
  // Core 1: o 0 fica com o loop() e com a interrupção de linha do DVI.
  if (xTaskCreatePinnedToCore(weatherTask, "WEATHER_HTTP", 8192,
                              (void *)(intptr_t)next, 1, nullptr, 1) != pdPASS) {
    weatherBusy.store(false);
    weatherStatus.store("SEM MEMORIA");
  }
}

// ----------------------------------------------------------------------------
//  Áudio (TLV320DAC3100, core 1) — WAV em loop contínuo
// ----------------------------------------------------------------------------

static bool openWav() {
  wavFile = SD.open(WAV_PATH, FILE_READ);
  if (!wavFile) {
    Serial.printf("[WEATHER] WAV nao abriu: %s\n", WAV_PATH);
    return false;
  }
  playback::WavInfo info;
  if (!playback::readWav(wavFile, info)) {
    wavFile.close();
    Serial.println("[WEATHER] WAV invalido (use PCM 16-bit stereo 22050 Hz)");
    return false;
  }
  sampleRate = info.rate;
  wavBlockAlign = info.align;
  wavDataStart = info.start;
  wavDataEnd = info.end;
  return true;
}

void audioTask(void *) {
  int64_t pcmDueUs = 0;
  for (;;) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      audioUnderruns.fetch_add(1);
      continue;
    }
    uint32_t pos = wavFile.position();
    if (pos >= wavDataEnd) {
      // LOOP CONTÍNUO: volta ao início dos dados em vez de parar.
      if (!wavFile.seek(wavDataStart)) {
        xSemaphoreGive(sdMutex);
        audioUnderruns.fetch_add(1);
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      pos = wavDataStart;
    }
    uint32_t remaining = wavDataEnd - pos;
    size_t bytes = wavFile.read(reinterpret_cast<uint8_t *>(audioBlock),
                                (AUDIO_CHUNK < remaining) ? AUDIO_CHUNK : remaining);
    xSemaphoreGive(sdMutex);

    if (!bytes || (bytes % wavBlockAlign)) {
      audioUnderruns.fetch_add(1);
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    playback::scalePcm(audioBlock, bytes / sizeof(int16_t), AUDIO_VOLUME);

    // O WAV já é estéreo intercalado de 16 bits, exatamente o que o DAC recebe.
    // write() bloqueia até caber: é ele quem dita o ritmo normalmente.
    const size_t frames = bytes / wavBlockAlign;
    if (audioout::write(audioBlock, frames) != frames) {
      audioUnderruns.fetch_add(1);
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Teto pelo relógio de amostras PCM: se o write() não bloqueou, a tarefa
    // não pode passar de AUDIO_LEAD_US à frente do tempo real.
    if (!pcmDueUs)
      pcmDueUs = esp_timer_get_time();
    pcmDueUs += ((int64_t)frames * 1000000LL) / (int64_t)sampleRate;
    const int64_t aheadUs = pcmDueUs - esp_timer_get_time();
    if (aheadUs > AUDIO_LEAD_US)
      vTaskDelay(pdMS_TO_TICKS((aheadUs - AUDIO_LEAD_US + 999) / 1000));
    else if (aheadUs < -100000) {
      pcmDueUs = esp_timer_get_time();
      audioUnderruns.fetch_add(1);
    }
  }
}

// ----------------------------------------------------------------------------
//  Renderização (tela estática + ticker rolante não-bloqueante)
// ----------------------------------------------------------------------------

static void buildTickerPayload() {
  if (!weatherValid.load()) {
    tickerPayload = "AGUARDANDO PREVISAO DE 3 DIAS...   ";
  } else {
    const WeatherData &w = weatherShadows[weatherActive.load()];
    tickerPayload = "";
    for (int i = 0; i < 3; ++i) {
      const DayForecast &f = w.days[i];
      if (i)
        tickerPayload += "    ";
      tickerPayload += f.name;
      tickerPayload += " ";
      tickerPayload += String(f.maxC) + "/" + String(f.minC) + "C ";
      tickerPayload += f.cond;
    }
  }
}

static void maybeRebuildTicker() {
  const String oldPayload = tickerPayload;
  buildTickerPayload();
  if (tickerPayload != oldPayload) {
    // Só reinicia a rolagem quando o conteúdo realmente mudou.
    tickerFull = tickerPayload + TICKER_GAP + tickerPayload;
    tickerPayloadW = ticker.textWidth(tickerPayload.c_str());
    tickerGapW = ticker.textWidth(TICKER_GAP);
    tickerWrapAt = tickerPayloadW + tickerGapW;
    tickerOffsetPx = 0;
  }
}

static void drawScreen() {
  // Sem startWrite/endWrite: o `tv` é memória, não há barramento a reservar.
  tv.fillScreen(TFT_NAVY);

  // Cabeçalho (amarelo, tipo teletexto). Tudo dentro da área segura: antes o
  // título ficava em y=6 e o ticker em y=224, ambos dentro do overscan da TV.
  tv.setFont(&fonts::Font4);
  tv.setTextDatum(top_center);
  tv.setTextColor(TFT_YELLOW, TFT_NAVY);
  tv.setTextSize(1);
  tv.drawString("FRANCA - SP", CRT_W / 2, SAFE_T);
  tv.drawFastHLine(SAFE_L, SAFE_T + 30, SAFE_W, TFT_CYAN);

  if (weatherValid.load()) {
    const WeatherData &w = weatherShadows[weatherActive.load()];
    char buf[64];

    // Condição atual
    tv.setFont(&fonts::Font4);
    tv.setTextSize(1);
    tv.setTextColor(TFT_WHITE, TFT_NAVY);
    tv.drawString(w.cond, CRT_W / 2, SAFE_T + 40);

    // Temperatura grande
    snprintf(buf, sizeof(buf), "%d C", w.tempC);
    tv.setTextSize(2);
    tv.setTextColor(TFT_YELLOW, TFT_NAVY);
    tv.drawString(buf, CRT_W / 2, SAFE_T + 76);

    // Umidade + vento (fonte menor: Font2)
    tv.setFont(&fonts::Font2);
    tv.setTextSize(1);
    tv.setTextColor(TFT_WHITE, TFT_NAVY);
    snprintf(buf, sizeof(buf), "UMIDADE  %d%%", w.humidity);
    tv.drawString(buf, CRT_W / 2, SAFE_T + 138);
    snprintf(buf, sizeof(buf), "VENTO  %d KM/H  %s", w.windKmph, w.windDir);
    tv.drawString(buf, CRT_W / 2, SAFE_T + 158);
  } else {
    tv.setFont(&fonts::Font4);
    tv.setTextSize(1);
    tv.setTextColor(TFT_WHITE, TFT_NAVY);
    tv.drawString(weatherStatus.load(), CRT_W / 2, SAFE_T + 86);
  }

  // Faixa do ticker (rodapé)
  tv.drawFastHLine(SAFE_L, TICKER_Y - 4, SAFE_W, TFT_CYAN);
  tv.fillRect(0, TICKER_Y, CRT_W, TICKER_H, TFT_BLACK);

  maybeRebuildTicker();
}

static void drawTicker() {
  if (tickerWrapAt > 0 && tickerOffsetPx >= tickerWrapAt)
    tickerOffsetPx -= tickerWrapAt;

  // Desenha no sprite (buffer offline), então copia para a tela sem flicker.
  ticker.fillSprite(TFT_BLACK);
  ticker.setTextDatum(top_left);
  ticker.setTextColor(TFT_WHITE, TFT_BLACK);
  ticker.drawString(tickerFull.c_str(), -tickerOffsetPx, 0);
  ticker.pushSprite(0, TICKER_Y);
}

// Sem LCD, a tela de erro vai para a TV; sem display::begin() nem isso — aí só
// resta o serial.
static void halt(const char *why) {
  Serial.printf("[WEATHER] %s\n", why);
  for (;;)
    delay(1000);
}

static void fatal(const char *title, const char *detail) {
  Serial.printf("[WEATHER] %s: %s\n", title, detail);
  tv.fillScreen(TFT_NAVY);
  tv.setFont(&fonts::Font4);
  tv.setTextDatum(middle_center);
  tv.setTextColor(TFT_YELLOW, TFT_NAVY);
  tv.setTextSize(1);
  tv.drawString(title, CRT_W / 2, CRT_H / 2 - 20);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString(detail, CRT_W / 2, CRT_H / 2 + 20);
  for (;;)
    delay(1000);
}

// ----------------------------------------------------------------------------
//  setup / loop
// ----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Ordem de boot FIXA (PORTING.md §2.1). O que importa: net::beginRadio()
  // pulsa o GPIO 22, que zera o ESP32-C6 *e* o DAC; configurar o DAC antes
  // deixaria o áudio mudo sem erro nenhum.
  board::begin();

  // DVI antes de qualquer tarefa: a interrupção de linha fica no núcleo que
  // chamou (o 0, o do loop()).
  if (!display::begin())
    halt("falha ao iniciar o DVI (sem memoria para o framebuffer)");

  // Sprite do ticker na SRAM: 320x16 em RGB565 são 10 KB, e o pushSprite a
  // cada 33 ms não deve passar pelo cache da PSRAM.
  ticker.setPsram(false);
  ticker.setColorDepth(16);
  if (!ticker.createSprite(CRT_W, TICKER_H))
    fatal("SEM MEMORIA", "sprite do ticker");
  ticker.setFont(&fonts::Font2);

  if (!storage::begin())
    fatal("SEM CARTAO SD", WAV_PATH);
  Serial.println("[WEATHER] SD montado (SDIO)");

  sdMutex = xSemaphoreCreateMutex();
  if (!sdMutex)
    fatal("ERRO SD", "sem mutex");

  if (!openWav())
    fatal("WAV INVALIDO", WAV_PATH);

  weatherStatus.store("CARREGANDO...");
  drawScreen();
  lastDrawnVersion = weatherVersion.load();

  // Acorda o ESP32-C6. A conexão ao Wi-Fi fica para a tarefa HTTP (ensureWiFi),
  // porque o WiFi.begin() do NINA bloqueia dezenas de segundos.
  if (!net::beginRadio())
    Serial.println("[WEATHER] ESP32-C6 nao respondeu; sem previsao");

  // Só agora o DAC. Sem ele a previsão segue muda: a música é enfeite, e parar
  // a tela por causa dela seria pior que o original, que só tinha o RCA.
  if (audioout::begin(sampleRate)) {
    audioout::setVolume(100);
    audioout::setRoute(AUDIO_ROUTE);
    // Core 1, prioridade acima da tarefa HTTP: o write() passa quase todo o
    // tempo bloqueado, então sobra CPU para ela no mesmo núcleo.
    if (xTaskCreatePinnedToCore(audioTask, "RCA_MUSIC", 4096, nullptr, 4,
                                &audioTaskHandle, 1) != pdPASS)
      Serial.println("[WEATHER] sem memoria para a tarefa de audio");
  } else {
    Serial.println("[WEATHER] DAC TLV320 nao iniciou; seguindo sem musica");
  }

  Serial.println("[WEATHER] Local Forecast iniciado (DVI + TLV320)");
}

void loop() {
  pollWeather();

  // Redesenha a tela estática quando houver dados novos (ou estado de erro).
  const uint32_t version = weatherVersion.load();
  bool redraw = false;
  if (version != lastDrawnVersion) {
    lastDrawnVersion = version;
    redraw = true;
  } else if (!weatherValid.load() && millis() - lastStatusRedraw >= 1000) {
    lastStatusRedraw = millis();
    redraw = true;
  }
  if (redraw)
    drawScreen();

  // Ticker rolante não-bloqueante (millis, sem delay longo).
  if (millis() - lastTickerMs >= TICKER_INTERVAL_MS) {
    lastTickerMs = millis();
    tickerOffsetPx += TICKER_STEP;
    drawTicker();
  }

  delay(1);
}
