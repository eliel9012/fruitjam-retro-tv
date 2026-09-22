// ============================================================================
//  The Weather Channel — Local Forecast (clone anos 80)
//  M5Stack Core2 + módulo RCA (M125) — vídeo composto NTSC + áudio I2S
//
//  PINOS DO I2S (áudio do RCA) :
//      BCK  = GPIO 19
//      DATA = GPIO  2
//      LRCK = GPIO  0
//      MCK  = NÃO USADO (NONE)
//
//  VÍDEO COMPOSTO (CVBS)      : GPIO 26 (periférico I2S0 reservado ao vídeo)
//  ÁUDIO EM LOOP CONTÍNUO     : /M5RETRO/weather_music.wav
//                               (PCM 16-bit estéreo 22050 Hz — já confirmado)
//
//  WI-FI (TODO: preencher)    : WIFI_SSID / WIFI_PASS abaixo
//  LOCALIDADE                 : Franca - SP (Open-Meteo, API pública sem chave)
//
//  OBSERVAÇÃO DE HARDWARE: o cartão TF do Core2 usa o barramento VSPI
//  (SCK=18, MISO=38, MOSI=23, CS=4). O GPIO 19 NÃO pode ser usado no SPI
//  porque pertence ao BCK do módulo RCA empilhado.
// ============================================================================

#include <Arduino.h>
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <M5Unified.h>
#include <M5GFX.h>
#include <M5ModuleRCA.h>

#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

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

// ---- Áudio (I2S1) ----
static constexpr uint8_t RCA_BCK  = 19;   // BCK do RCA (I2S1)
static constexpr uint8_t RCA_DATA = 2;    // DATA do RCA (I2S1)
static constexpr uint8_t RCA_LRCK = 0;    // LRCK/WS do RCA (I2S1)
static const char *WAV_PATH = "/M5RETRO/weather_music.wav";
static constexpr int AUDIO_VOLUME = 80;   // 0..100

// ---- Vídeo (CVBS) ----
static constexpr uint8_t CVBS_PIN = 26;

// ---- SD (VSPI — GPIO19 NÃO pode ser usado) ----
static constexpr uint8_t SD_CS   = 4;
static constexpr uint8_t SD_SCK  = 18;
static constexpr uint8_t SD_MISO = 38;
static constexpr uint8_t SD_MOSI = 23;

// ---- Dimensões e cadências ----
static constexpr int CRT_W = 320, CRT_H = 240;
// Área segura do tubo: uma TV CRT corta cerca de 7% de cada borda (overscan),
// então o raster inteiro nunca aparece. Era isso que cortava o cabeçalho e o
// ticker no aparelho. O fundo continua sangrando até a borda; texto, linhas e
// faixas ficam dentro desta caixa.
static constexpr int SAFE_X = 24, SAFE_Y = 18;
static constexpr int SAFE_L = SAFE_X;          // 24
static constexpr int SAFE_T = SAFE_Y;          // 18
static constexpr int SAFE_R = CRT_W - SAFE_X;  // 296
static constexpr int SAFE_B = CRT_H - SAFE_Y;  // 222
static constexpr int SAFE_W = SAFE_R - SAFE_L; // 272
static constexpr size_t AUDIO_CHUNK = 1024;          // bytes por bloco PCM
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

// NTSC (525/59,94 Hz, preto em 7,5 IRE). O modo PAL_M do M5GFX monta a linha
// com 908 amostras, mas 4x3,57561149 MHz x 63,5556 us dá 909,02 — a linha sai
// ~0,11% curta e a fase da burst anda a cada linha, produzindo a faixa de cor
// diagonal que caminha pela tela. A tabela NTSC usa 910 amostras, valor exato
// para 4x3,579545 MHz, então a burst fica estável.
M5ModuleRCA rca(CRT_W, CRT_H, CRT_W, CRT_H, M5ModuleRCA::signal_type_t::NTSC,
                M5ModuleRCA::use_psram_t::psram_no_use, CVBS_PIN, 200);
LGFX_Sprite ticker(&rca);

SemaphoreHandle_t sdMutex = nullptr;
File wavFile;
TaskHandle_t audioTaskHandle = nullptr;

uint32_t sampleRate = 22050, wavDataStart = 0, wavDataEnd = 0;
uint16_t wavBlockAlign = 4;

std::atomic<bool> audioReady{false};
std::atomic<bool> audioFailed{false};
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

// Escrita apenas pela tarefa HTTP (core 1), lida pelo loop (core 1).
char weatherStatus[32] = "CARREGANDO...";
uint32_t lastWeatherAttempt = 0;
uint32_t lastWeatherGood = 0;

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

static bool fetchWeather(WeatherData &out) {
  WiFiClientSecure client;
  client.setInsecure();   // sem CA (economiza RAM)

  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  bool ok = false;
  if (http.begin(client, WEATHER_URL)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      // ~800 bytes: lê tudo para um buffer contíguo (o leitor Stream do
      // ArduinoJson 7.4.2 descarta escalares no ESP32) e rejeita corpo
      // truncado em vez de publicar dados pela metade.
      constexpr int cap = 4096;
      char buf[cap];
      size_t len = 0;
      uint32_t quiet = 0;
      Stream &st = http.getStream();
      while (len < cap - 1 && quiet < 3000) {
        int avail = st.available();
        if (avail > 0) {
          int n = st.readBytes(buf + len, min(avail, cap - 1 - (int)len));
          if (n <= 0)
            break;
          len += n;
          quiet = 0;
        } else if (!http.connected()) {
          break;
        } else {
          delay(5);
          quiet += 5;
        }
      }
      buf[len] = 0;
      if (len >= cap - 1) {
        Serial.println("[WEATHER] resposta maior que o buffer");
      } else if (len) {
        JsonDocument doc;
        DeserializationError err =
            deserializeJson(doc, buf, DeserializationOption::NestingLimit(8));
        if (!err)
          ok = parseWeather(doc, out);
        else
          Serial.printf("[WEATHER] parse %s (%u bytes)\n", err.c_str(), (unsigned)len);
      } else {
        Serial.println("[WEATHER] corpo vazio");
      }
    } else {
      Serial.printf("[WEATHER] HTTP %d\n", code);
    }
  } else {
    Serial.println("[WEATHER] falha em http.begin");
  }
  http.end();
  return ok;
}

// Tarefa de baixa prioridade no core 1: faz o GET e publica o resultado.
static void weatherTask(void *arg) {
  int writeIndex = (int)(intptr_t)arg;
  WeatherData local;
  memset(&local, 0, sizeof(local));

  bool ok = fetchWeather(local);
  if (ok) {
    weatherShadows[writeIndex] = local;   // escreve no buffer não exibido
    weatherActive.store(writeIndex);      // publica
    weatherValid.store(true);
    weatherVersion.fetch_add(1);
    lastWeatherGood = millis();
    snprintf(weatherStatus, sizeof(weatherStatus), "ATUALIZADO");
    Serial.printf("[WEATHER] atualizado: %dC %d%% %s\n", local.tempC,
                  local.humidity, local.cond);
  } else {
    snprintf(weatherStatus, sizeof(weatherStatus), "ERRO NA CONSULTA");
    Serial.println("[WEATHER] falha na consulta");
    if (!weatherValid.load())
      weatherVersion.fetch_add(1);   // força redesenho do estado de erro
  }
  weatherBusy.store(false);
  vTaskDelete(nullptr);
}

static void pollWeather() {
  if (weatherBusy.load())
    return;
  if (WiFi.status() != WL_CONNECTED) {
    if (!weatherValid.load())
      snprintf(weatherStatus, sizeof(weatherStatus), "SEM WI-FI");
    return;
  }
  const uint32_t now = millis();
  if (weatherValid.load()) {
    if (now - lastWeatherGood < WEATHER_REFRESH_MS)
      return;   // cadência normal de 10 minutos
  } else {
    // Primeira consulta (ou falhas consecutivas): evita retry em loop apertado.
    if (lastWeatherAttempt != 0 && now - lastWeatherAttempt < WEATHER_RETRY_MS)
      return;
  }
  lastWeatherAttempt = now;
  weatherBusy.store(true);
  int next = 1 - weatherActive.load();   // buffer oposto ao exibido
  if (xTaskCreatePinnedToCore(weatherTask, "WEATHER_HTTP", 8192,
                              (void *)(intptr_t)next, 0, nullptr, 1) != pdPASS) {
    weatherBusy.store(false);
    snprintf(weatherStatus, sizeof(weatherStatus), "SEM MEMORIA");
  }
}

static void serviceWiFi() {
  static uint32_t lastTry = 0;
  if (WiFi.status() == WL_CONNECTED)
    return;
  if (millis() - lastTry < 10000)
    return;
  lastTry = millis();
  Serial.println("[WEATHER] reconectando Wi-Fi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ----------------------------------------------------------------------------
//  Áudio (I2S1, core 0) — WAV em loop contínuo, ritmado pelo relógio PCM
// ----------------------------------------------------------------------------

static bool initAudio() {
  i2s_config_t c = {};
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  c.sample_rate = 22050;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count = 8;
  c.dma_buf_len = 256;
  c.use_apll = false;
  c.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_1, &c, 0, nullptr) != ESP_OK)
    return false;
  i2s_pin_config_t p = {};
  p.mck_io_num = I2S_PIN_NO_CHANGE;   // MCK não é usado
  p.bck_io_num = RCA_BCK;
  p.ws_io_num = RCA_LRCK;
  p.data_out_num = RCA_DATA;
  p.data_in_num = I2S_PIN_NO_CHANGE;
  return i2s_set_pin(I2S_NUM_1, &p) == ESP_OK;
}

static bool openWav() {
  wavFile = SD.open(WAV_PATH, FILE_READ);
  if (!wavFile) {
    Serial.printf("[WEATHER] WAV não abriu: %s\n", WAV_PATH);
    return false;
  }
  playback::WavInfo info;
  if (!playback::readWav(wavFile, info)) {
    wavFile.close();
    Serial.println("[WEATHER] WAV inválido (use PCM 16-bit stereo 22050 Hz)");
    return false;
  }
  sampleRate = info.rate;
  wavBlockAlign = info.align;
  wavDataStart = info.start;
  wavDataEnd = info.end;
  return true;
}

void audioTask(void *) {
  // Três blocos rotativos em SRAM interna (DMA). O i2s_write copia o bloco
  // para a fila DMA, mas o padrão validado mantém 3 buffers.
  uint8_t *buffers[3];
  for (auto &b : buffers)
    b = (uint8_t *)heap_caps_malloc(AUDIO_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buffers[0] || !buffers[1] || !buffers[2]) {
    for (auto b : buffers)
      free(b);
    audioFailed = true;
    audioReady = true;
    vTaskDelete(nullptr);
    return;
  }
  audioReady = true;

  int idx = 0;
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
    size_t bytes = wavFile.read(buffers[idx],
                                (AUDIO_CHUNK < remaining) ? AUDIO_CHUNK : remaining);
    xSemaphoreGive(sdMutex);

    if (!bytes || (bytes % wavBlockAlign)) {
      audioUnderruns.fetch_add(1);
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    playback::scalePcm(reinterpret_cast<int16_t *>(buffers[idx]),
                       bytes / sizeof(int16_t), AUDIO_VOLUME);

    size_t written = 0;
    esp_err_t r = i2s_write(I2S_NUM_1, buffers[idx], bytes, &written, pdMS_TO_TICKS(100));
    if (r != ESP_OK || written != bytes) {
      audioUnderruns.fetch_add(1);
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Ritmo pelo relógio de amostras PCM, para não encher a fila do DMA.
    const uint32_t frames = written / wavBlockAlign;
    if (!pcmDueUs)
      pcmDueUs = esp_timer_get_time();
    pcmDueUs += ((int64_t)frames * 1000000LL) / (int64_t)sampleRate;
    const int64_t waitUs = pcmDueUs - esp_timer_get_time();
    if (waitUs > 0)
      vTaskDelay(pdMS_TO_TICKS((waitUs + 999) / 1000));
    else if (waitUs < -100000) {
      pcmDueUs = esp_timer_get_time();
      audioUnderruns.fetch_add(1);
    }
    idx = (idx + 1) % 3;
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
  rca.startWrite();
  rca.fillScreen(TFT_NAVY);

  // Cabeçalho (amarelo, tipo teletexto). Tudo dentro da área segura: antes o
  // título ficava em y=6 e o ticker em y=224, ambos dentro do overscan da TV.
  rca.setFont(&fonts::Font4);
  rca.setTextDatum(top_center);
  rca.setTextColor(TFT_YELLOW, TFT_NAVY);
  rca.setTextSize(1);
  rca.drawString("FRANCA - SP", CRT_W / 2, SAFE_T);
  rca.drawFastHLine(SAFE_L, SAFE_T + 30, SAFE_W, TFT_CYAN);

  if (weatherValid.load()) {
    const WeatherData &w = weatherShadows[weatherActive.load()];
    char buf[64];

    // Condição atual
    rca.setFont(&fonts::Font4);
    rca.setTextSize(1);
    rca.setTextColor(TFT_WHITE, TFT_NAVY);
    rca.drawString(w.cond, CRT_W / 2, SAFE_T + 40);

    // Temperatura grande
    snprintf(buf, sizeof(buf), "%d C", w.tempC);
    rca.setTextSize(2);
    rca.setTextColor(TFT_YELLOW, TFT_NAVY);
    rca.drawString(buf, CRT_W / 2, SAFE_T + 76);

    // Umidade + vento (fonte menor: Font2)
    rca.setFont(&fonts::Font2);
    rca.setTextSize(1);
    rca.setTextColor(TFT_WHITE, TFT_NAVY);
    snprintf(buf, sizeof(buf), "UMIDADE  %d%%", w.humidity);
    rca.drawString(buf, CRT_W / 2, SAFE_T + 138);
    snprintf(buf, sizeof(buf), "VENTO  %d KM/H  %s", w.windKmph, w.windDir);
    rca.drawString(buf, CRT_W / 2, SAFE_T + 158);
  } else {
    rca.setFont(&fonts::Font4);
    rca.setTextSize(1);
    rca.setTextColor(TFT_WHITE, TFT_NAVY);
    rca.drawString(weatherStatus, CRT_W / 2, SAFE_T + 86);
  }

  // Faixa do ticker (rodapé)
  rca.drawFastHLine(SAFE_L, TICKER_Y - 4, SAFE_W, TFT_CYAN);
  rca.fillRect(0, TICKER_Y, CRT_W, TICKER_H, TFT_BLACK);
  rca.endWrite();

  maybeRebuildTicker();
}

static void drawTicker() {
  if (tickerOffsetPx >= tickerWrapAt)
    tickerOffsetPx -= tickerWrapAt;

  // Desenha no sprite (buffer offline), então copia para a tela sem flicker.
  ticker.fillSprite(TFT_BLACK);
  ticker.setTextDatum(top_left);
  ticker.setTextColor(TFT_WHITE, TFT_BLACK);
  ticker.drawString(tickerFull.c_str(), -tickerOffsetPx, 0);

  rca.startWrite();
  ticker.pushSprite(0, TICKER_Y);
  rca.endWrite();
}

static void fatal(const char *title, const char *detail) {
  rca.startWrite();
  rca.fillScreen(TFT_NAVY);
  rca.setFont(&fonts::Font4);
  rca.setTextDatum(middle_center);
  rca.setTextColor(TFT_YELLOW, TFT_NAVY);
  rca.setTextSize(1);
  rca.drawString(title, CRT_W / 2, CRT_H / 2 - 20);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(detail, CRT_W / 2, CRT_H / 2 + 20);
  rca.endWrite();
  for (;;)
    delay(1000);
}

// ----------------------------------------------------------------------------
//  setup / loop
// ----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Core2: AXP192 + I2C (usado na detecção do módulo RCA). Sem alto-falante
  // interno — o áudio sai apenas pelo RCA (I2S1).
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.external_spk = false;
  M5.begin(cfg);

  // Vídeo composto (CVBS, I2S0 reservado ao DAC do painel).
  if (!rca.init()) {
    Serial.println("[WEATHER] falha ao iniciar NTSC (CVBS)");
    for (;;)
      delay(1000);
  }
  rca.setColorDepth(8);   // RGB332: 1 byte/pixel (320x240 = 76.800 bytes em SRAM)
  rca.setOutputBoost(true);

  // Sprite do ticker (SRAM, RGB332) — usa a rca como destino do pushSprite.
  ticker.setPsram(false);
  ticker.setColorDepth(8);
  if (!ticker.createSprite(CRT_W, TICKER_H)) {
    Serial.println("[WEATHER] falha ao criar sprite do ticker");
    for (;;)
      delay(1000);
  }
  ticker.setFont(&fonts::Font2);

  // Cartão TF no VSPI. GPIO19 está reservado ao BCK do RCA (não entra no SPI).
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  bool sdMounted = SD.begin(SD_CS, SPI, 25000000);
  if (!sdMounted) {
    SD.end();
    delay(30);
    sdMounted = SD.begin(SD_CS, SPI, 4000000);
  }
  if (!sdMounted)
    fatal("SEM CARTAO SD", WAV_PATH);
  Serial.println("[WEATHER] SD montado (CS4 SCK18 MISO38 MOSI23)");

  sdMutex = xSemaphoreCreateMutex();
  if (!sdMutex)
    fatal("ERRO SD", "sem mutex");

  if (!openWav())
    fatal("WAV INVALIDO", WAV_PATH);

  snprintf(weatherStatus, sizeof(weatherStatus), "CARREGANDO...");
  drawScreen();
  lastDrawnVersion = weatherVersion.load();

  // Wi-Fi (conexão assíncrona; serviceWiFi() cuida da reconexão).
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Áudio RCA (I2S1) no core 0.
  if (!initAudio())
    fatal("FALHA AUDIO", "I2S1");
  if (xTaskCreatePinnedToCore(audioTask, "RCA_MUSIC", 4096, nullptr, 4,
                              &audioTaskHandle, 0) != pdPASS)
    fatal("FALHA AUDIO", "sem tarefa");
  while (!audioReady && !audioFailed)
    delay(1);
  if (audioFailed)
    fatal("FALHA AUDIO", "sem memoria DMA");

  Serial.println("[WEATHER] Local Forecast iniciado (NTSC + RCA audio)");
}

void loop() {
  serviceWiFi();
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