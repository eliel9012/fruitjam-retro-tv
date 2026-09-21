#include <Arduino.h>
#include <atomic>
#include <time.h>
#include "PlaybackIO.h"
#include "UiLogic.h"
#include "Ascii.h"
#include "Id3.h"
#include "libhelix-mp3/mp3dec.h"
#include "SafeStorage.h"
#include "NetworkManager.h"
#include <M5Unified.h>
#include <M5GFX.h>
#include <M5ModuleRCA.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <mbedtls/platform.h>
#include "InputManager.h"
#include "LocalizationPTBR.h"
#include "SecretsManager.h"
#include "ConfigurationPortal.h"

// M5Stack Core2 + physically stacked Module13.2 RCA M125.

enum class AudioOutput : uint8_t { RCA, INTERNAL, MUTED };

struct Settings {
  double lat = 0, lon = 0;
  int rangeKm = 250, refreshSeconds = 10, volume = 75;
  bool vhsOsd = true;
  AudioOutput audioOutput = AudioOutput::RCA;
} settings;

struct Secrets {
  String ssid, password, base, endpoint, authMode, authHeader, authPrefix, token;
  bool insecure = false;
} secrets;

struct Aircraft {
  String icao, callsign, aircraft_type;
  double latitude = 0, longitude = 0;
  float altitude_ft = 0, speed_kt = 0, heading_deg = 0;
  bool valid_position = false;
};

// Forward declarations
void dualText(const String &line1, const String &line2);
void setError(const String &message);
bool makeDirectories();
bool loadConfiguration();
void saveSettings();
void serviceWiFi();
bool initExternalAudio();
bool openWavAndReadHeader();
void audioTask(void *);
int jpegDraw(JPEGDRAW *draw);
bool validateMjpegStream();
bool readAndShowOneFrame(bool render = true);
bool startProgram(const String &dir);
void stopProgram();
void videoTick();
String stringAlias(JsonObject o, const char *a, const char *b, const char *c);
double numberAlias(JsonObject o, const char *a, const char *b, const char *c);
bool parseAircraft(JsonDocument &doc);
void pollAircraft();
void drawHome();
void drawLibrary();
void drawRadar();
void drawSettings();
void drawInfo();
void drawSetupPortal();
void startSetupPortal();
void portalSaved(const SecretsConfig &savedSecrets, const RadarConfig &savedSettings);
void handleTouch();
void drawBackButton();
void handleNavigation(NavAction action);
void drawControllerLabels(const char *left, const char *center, const char *right);
void drawPlaybackOsd();
void drawPlaybackController();
void setBacklight(bool on);
bool drawStaticPoster(const String &dir);
void uiHudInit();
void uiHudDraw();
void uiHudClear();
void uiHudTick(uint32_t now);
void setAudioOutput(AudioOutput output);
void servicePowerButton();
String playbackClock();
String libraryRoot();
String normalizeSdPath(const String &path);
String libraryChildPath(const String &root, const String &entryName);
bool isProgramFolder(const String &path);
int libraryProgramCount();
String libraryProgramAt(int wantedIndex);
void drawWeatherFrame();
void weatherTick();
void pollWeather();
void startWeather();
void stopWeather();
void drawMusicBrowser();
void drawMusicNowPlaying();
bool startMusic(const String &path);
void stopMusic();
void musicTick();
static bool mp3Begin(const String &path);
static void mp3End();
static size_t mp3ReadPcm(int16_t *dst, size_t samples);

static constexpr uint8_t CVBS_PIN = 26;
static constexpr uint8_t RCA_BCK = 19;
static constexpr uint8_t RCA_DATA = 2;
static constexpr uint8_t RCA_LRCK = 0;
// Core2 microSD slot is on the display's VSPI bus. GPIO38 is its MISO;
// GPIO19 is intentionally unavailable because the stacked RCA module uses it for PCM BCK.
static constexpr uint8_t SD_CS = 4;
static constexpr uint8_t SD_SCK = 18;
static constexpr uint8_t SD_MISO = 38;
static constexpr uint8_t SD_MOSI = 23;
static constexpr size_t MAX_JPEG = 128 * 1024;
static constexpr size_t AUDIO_CHUNK = 1024;
static constexpr int CRT_W = 320, CRT_H = 240;
// Faixa reservada no LCD do Core2 para o HUD (tempo + progresso) redesenhado a
// 1 Hz. O vídeo nunca toca o LCD: atrás desta faixa fica apenas o pôster.
static constexpr int HUD_W = 192, HUD_H = 16, HUD_Y = 204;
// Weather Channel: faixa do ticker e cadências de consulta.
static constexpr int TICKER_H = 16, TICKER_Y = 240 - 16;
static constexpr uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t WEATHER_RETRY_MS = 30UL * 1000UL;
static const char *WEATHER_MUSIC = "/M5RETRO/weather_music.wav";

const char *ROOT = "/M5RETRO";
const char *VIDEOS = "/M5RETRO/videos";
const char *CONFIG = "/M5RETRO/config";
const char *SETTINGS_FILE = "/M5RETRO/config/settings.json";
const char *SECRETS_FILE = "/M5RETRO/config/secrets.json";
const char *CA_FILE = "/M5RETRO/config/ca.pem";
const char *CACHE_FILE = "/M5RETRO/cache/aircraft.json";
const char *MUSIC_ROOT = "/M5RETRO/music";

M5ModuleRCA rca(CRT_W, CRT_H, CRT_W, CRT_H, M5ModuleRCA::signal_type_t::PAL_M,
                M5ModuleRCA::use_psram_t::psram_no_use, CVBS_PIN, 200);
JPEGDEC jpeg;
playback::MjpegReader mjpegReader;
bool videoReadError = false;
int videoWidth = 240, videoHeight = 160;
std::atomic<bool> audioIdle{true}, audioReady{false}, audioFailed{false}, audioStreamError{false};
std::atomic<int> playbackVolume{75};
UiState state = BOOT;
File mjpegFile, wavFile;
SemaphoreHandle_t sdMutex;
TaskHandle_t audioTaskHandle = nullptr;
uint8_t *jpegBuffer = nullptr;
Aircraft aircraft[64];
int aircraftCount = 0;
String currentTitle;
float fps = 15.0f;
uint32_t sampleRate = 22050, wavDataStart = 0, wavDataEnd = 0;
uint16_t wavChannels = 2;
uint16_t wavBlockAlign = 4;
std::atomic<uint32_t> samplesPlayed{0};
std::atomic<bool> playing{false}, paused{false}, playbackFinished{false};
std::atomic<AudioOutput> audioOutput{AudioOutput::RCA};
uint32_t videoFrameIndex = 0, decodedFrames = 0, renderedFrames = 0, droppedFrames = 0, jpegErrors = 0;
LGFX_Sprite uiHud(&M5.Display); // sprite minúsculo do HUD (tempo + progresso); pai = LCD
uint32_t lastUiUpdate = 0;      // borda de 1 s que dispara o redesenho do HUD
static bool backlightOn = true; // estado atual do backlight (DCDC3 do AXP192)
std::atomic<bool> weatherAudio{false}; // música do Weather Channel em loop (I2S1), flag de áudio em loop

// --- Weather Channel (Local Forecast) ---
struct WeatherDay {
  char name[8]; // "DOM".."SAB"
  int maxC = 0, minC = 0;
};
struct WeatherData {
  int tempC = -100, humidity = 0, windKmph = 0;
  char cond[24] = {0}, windDir[8] = {0};
  WeatherDay days[3];
};
WeatherData weatherShadows[2];
std::atomic<int> weatherActive{0};
std::atomic<uint32_t> weatherVersion{0};
std::atomic<bool> weatherReady{false};
std::atomic<bool> weatherBusy{false};
char weatherStatus[32] = "CARREGANDO...";
uint32_t lastWeatherAttempt = 0, lastWeatherGood = 0;
uint32_t lastWeatherDraw = 0, lastTickerMs = 0;
uint16_t lastWeatherBg = 0;
LGFX_Sprite weatherTicker(&rca);
String tickerPayload, tickerFull;
int tickerOffset = 0, tickerWrapAt = 0;
std::atomic<uint32_t> audioUnderruns{0};
uint32_t lastTouch = 0, lastRadarDraw = 0, lastApiPoll = 0, lastApiGood = 0, lastStats = 0,
         jpegDecodeTotalMs = 0, jpegDecodeMaxMs = 0;
String lastError, apiStatus = "NAO CONFIGURADA";
struct RadarRequest {
  SecretsConfig config;
  String ca;
};
struct RadarResponse {
  JsonDocument data;
  String status;
  int httpCode = 0;
  bool parsed = false;
  uint32_t elapsedMs = 0;
};
std::atomic<RadarResponse *> radarResponse{nullptr};
bool radarBusy = false;
int lastHttpCode = 0;
uint32_t lastHttpMs = 0;
bool networkConfigPresent = false;

InputManager input;
int homeSelection = 0, librarySelection = 0, radarSelection = -1, settingsSelection = 0, infoPage = 0;
bool settingsEditing = false, radarDetails = false, radarDirty = false;
uint32_t osdUntil = 0;
SecretsManager secretsStore;
NetworkManager network;
ConfigurationPortal portal;
bool portalSavePending = false;
uint32_t portalSaveStarted = 0;
bool bootReady = false;
bool powerOffPending = false;
uint32_t powerOffAt = 0;

// --- Player de música (F1: navegação por pastas + WAV; F3 adiciona MP3) ---
struct MusicEntry {
  String name;    // nome de exibição (normalizado ASCII via ascii::normalize)
  String path;    // caminho SD completo
  bool isFolder;
};
String musicDir = MUSIC_ROOT;
MusicEntry musicEntries[32];
int musicEntryCount = 0;
int musicSelection = 0;
String musicQueue[64]; // faixas (.wav) do diretório corrente, em ordem
int musicQueueCount = 0, musicQueueIndex = -1;
id3::TrackMeta musicMeta; // tags da faixa tocando (F2)
LGFX_Sprite *coverSprite = nullptr; // capa do álbum decodificada (PSRAM RGB565)
static LGFX_Sprite *coverTarget = nullptr; // alvo do callback JPEGDEC durante o decode

// MP3 (libhelix) — F3: decodificação em software no core 0, saída 22050 Hz estéreo.
HMP3Decoder mp3Dec = nullptr;
File mp3File;
bool mp3Mode = false;
static int16_t mp3FramePcm[2304]; // saída de um frame MPEG1 estéreo (1152x2)
static size_t mp3PcmCount = 0;
static int mp3Rate = 44100, mp3Chans = 2;
static double mp3Phase = 0.0;
static uint8_t mp3In[4096];
static size_t mp3InLen = 0;

void drawBackButton() {
  if (state == HOME || state == BOOT)
    return;
  M5.Display.fillRoundRect(280, 4, 36, 30, 4, TFT_BLUE);
  M5.Display.drawRoundRect(280, 4, 36, 30, 4, TFT_CYAN);
  M5.Display.fillTriangle(287, 19, 297, 10, 297, 28, TFT_WHITE);
  M5.Display.fillRect(296, 16, 12, 6, TFT_WHITE);
}
void setBacklight(bool on) {
  // O backlight do Core2 é alimentado pelo DCDC3 do AXP192; 2800 mV é o brilho
  // padrão de fábrica. IMPORTANTE: nunca mexa no LDO2 — ele alimenta LCD e
  // microSD juntos, e desligá-lo derrubaria o cartão.
  if (on == backlightOn)
    return;
  backlightOn = on;
  M5.Power.Axp192.setDCDC3(on ? 2800 : 0);
}
void dualText(const String &line1, const String &line2 = "") {
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(middle_center);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->setTextSize(2);
    d->drawString(line1, 160, 94);
    d->setTextColor(TFT_CYAN, TFT_NAVY);
    d->setTextSize(1);
    d->drawString(line2, 160, 130);
  }
  drawBackButton();
}
void setError(const String &message) {
  if (playing || wavFile)
    stopProgram();
  lastError = message;
  state = ERROR_SCREEN;
  dualText("ERRO DO SISTEMA", message);
  Serial.printf("[M5RETRO] ERRO: %s\n", message.c_str());
}
bool makeDirectories() {
  // FAT directories are created one level at a time. Check each result so a
  // missing config folder never becomes a misleading file-open failure.
  const bool rootReady = SD.exists(ROOT) || SD.mkdir(ROOT);
  const bool videosReady = rootReady && (SD.exists(VIDEOS) || SD.mkdir(VIDEOS));
  const bool configReady = rootReady && (SD.exists(CONFIG) || SD.mkdir(CONFIG));
  const bool cacheReady = rootReady && (SD.exists("/M5RETRO/cache") || SD.mkdir("/M5RETRO/cache"));
  const bool musicReady = rootReady && (SD.exists(MUSIC_ROOT) || SD.mkdir(MUSIC_ROOT));
  if (!rootReady || !videosReady || !configReady || !cacheReady || !musicReady) {
    Serial.printf("[M5RETRO] ERRO: pastas SD indisponiveis root:%d videos:%d config:%d cache:%d music:%d\n",
                  rootReady, videosReady, configReady, cacheReady, musicReady);
    return false;
  }
  return true;
}

RadarConfig currentSettings() {
  RadarConfig r;
  r.latitude = settings.lat;
  r.longitude = settings.lon;
  r.rangeKm = settings.rangeKm;
  r.refreshSeconds = settings.refreshSeconds;
  r.volume = settings.volume;
  r.vhsOsd = settings.vhsOsd;
  r.audioOutput = settings.audioOutput == AudioOutput::INTERNAL ? "interno"
                  : settings.audioOutput == AudioOutput::MUTED  ? "mudo"
                                                                : "rca";
  return r;
}
void applySettings(const RadarConfig &r) {
  settings.lat = r.latitude;
  settings.lon = r.longitude;
  settings.rangeKm = r.rangeKm;
  settings.refreshSeconds = r.refreshSeconds;
  settings.volume = r.volume;
  settings.vhsOsd = r.vhsOsd;
  settings.audioOutput = r.audioOutput == "interno" ? AudioOutput::INTERNAL
                         : r.audioOutput == "mudo"  ? AudioOutput::MUTED
                                                    : AudioOutput::RCA;
  playbackVolume = r.volume;
}
bool loadConfiguration() {
  SecretsConfig s;
  RadarConfig r;
  const bool valid = secretsStore.load(s, r);
  applySettings(r);
  secrets.ssid = s.ssid;
  secrets.password = s.password;
  secrets.base = s.baseUrl;
  secrets.endpoint = s.endpoint;
  secrets.authMode = s.authMode;
  secrets.authHeader = s.authHeader;
  secrets.authPrefix = s.authPrefix;
  secrets.token = s.token;
  secrets.insecure = s.allowInsecureTls;
  networkConfigPresent = valid;
  network.begin(s);
  return valid;
}

SecretsConfig currentSecrets() {
  SecretsConfig s;
  s.ssid = secrets.ssid;
  s.password = secrets.password;
  s.baseUrl = secrets.base;
  s.endpoint = secrets.endpoint;
  s.authMode = secrets.authMode;
  s.authHeader = secrets.authHeader;
  s.authPrefix = secrets.authPrefix;
  s.token = secrets.token;
  s.allowInsecureTls = secrets.insecure;
  return s;
}
void serviceWiFi() {
  if (!networkConfigPresent || playing)
    return;
  const NetworkState before = network.state();
  network.update();
  if (network.connected()) {
    static bool clockRequested = false;
    if (!clockRequested) {
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      clockRequested = true;
    }
    if (before != NetworkState::CONNECTED)
      apiStatus = "CONECTADA";
  } else if (network.state() == NetworkState::CONNECTING)
    apiStatus = "CONECTANDO WI-FI";
  else
    apiStatus = "SEM WI-FI";
}

void saveSettings() {
  playbackVolume = settings.volume;
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  const bool saved = secretsStore.saveSettings(currentSettings());
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  if (!saved)
    setError(PTBR::ERRO_SD);
}

void drawSetupPortal() {
  String line1 = "CONFIGURACAO";
  String line2 = portal.active() ? "WI-FI: " + portal.apSsid() : "INICIANDO...";
  dualText(line1, line2);
  if (portal.active()) {
    rca.setTextDatum(top_left);
    rca.setTextColor(TFT_WHITE, TFT_NAVY);
    rca.setTextSize(1);
    rca.drawString("SENHA: " + portal.apPassword(), 26, 144);
    rca.drawString("ABRA: 192.168.4.1", 26, 162);
    rca.drawString(portal.status() == PortalStatus::CONFIGURANDO ? "CONFIGURACAO ATIVA"
                                                                 : "AGUARDANDO CELULAR...",
                   26, 180);
    M5.Display.fillScreen(TFT_NAVY);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextSize(2);
    M5.Display.drawString("M5 RETRO TV", 12, 8);
    M5.Display.drawFastHLine(8, 36, 304, TFT_CYAN);
    M5.Display.drawString("CONFIGURACAO", 12, 48);
    M5.Display.setTextSize(1);
    M5.Display.drawString("WI-FI: " + portal.apSsid(), 20, 88);
    M5.Display.drawString("SENHA: " + portal.apPassword(), 20, 112);
    M5.Display.drawString("ABRA: 192.168.4.1", 20, 136);
    drawControllerLabels("VOLTAR", "STATUS", "SAIR");
  }
}

void startSetupPortal() {
  if (playing)
    stopProgram();
  SecretsConfig saved;
  saved.ssid = secrets.ssid;
  saved.password = secrets.password;
  saved.baseUrl = secrets.base;
  saved.endpoint = secrets.endpoint;
  saved.authMode = secrets.authMode;
  saved.authHeader = secrets.authHeader;
  saved.authPrefix = secrets.authPrefix;
  saved.token = secrets.token;
  saved.allowInsecureTls = secrets.insecure;
  RadarConfig radar = currentSettings();
  network.disconnect();
  portal.begin(secretsStore, saved, radar, portalSaved);
  if (!portal.active()) {
    network.begin(currentSecrets());
    setError("FALHA AO ABRIR PORTAL");
    return;
  }
  state = SETUP_PORTAL;
  drawSetupPortal();
}

void portalSaved(const SecretsConfig &savedSecrets, const RadarConfig &savedSettings) {
  secrets.ssid = savedSecrets.ssid;
  secrets.password = savedSecrets.password;
  secrets.base = savedSecrets.baseUrl;
  secrets.endpoint = savedSecrets.endpoint;
  secrets.authMode = savedSecrets.authMode;
  secrets.authHeader = savedSecrets.authHeader;
  secrets.authPrefix = savedSecrets.authPrefix;
  secrets.token = savedSecrets.token;
  secrets.insecure = savedSecrets.allowInsecureTls;
  applySettings(savedSettings);
  network.begin(savedSecrets);
  WiFi.disconnect(false, false);
  WiFi.begin(secrets.ssid.c_str(), secrets.password.c_str());
  networkConfigPresent = true;
  portalSavePending = true;
  portalSaveStarted = millis();
  apiStatus = "CONECTANDO WI-FI";
  dualText("CONFIGURACAO SALVA", "CONECTANDO AO WI-FI...");
}

bool initExternalAudio() {
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
  p.mck_io_num = I2S_PIN_NO_CHANGE;
  p.bck_io_num = RCA_BCK;
  p.ws_io_num = RCA_LRCK;
  p.data_out_num = RCA_DATA;
  p.data_in_num = I2S_PIN_NO_CHANGE;
  return i2s_set_pin(I2S_NUM_1, &p) == ESP_OK;
}

bool openWavAndReadHeader() {
  playback::WavInfo info;
  if (!playback::readWav(wavFile, info)) {
    Serial.println("[M5RETRO] WAV invalido: use PCM 16-bit stereo 22050 Hz");
    return false;
  }
  sampleRate = info.rate;
  wavChannels = info.channels;
  wavBlockAlign = info.align;
  wavDataStart = info.start;
  wavDataEnd = info.end;
  return true;
}

void audioTask(void *) {
  // These buffers remain in internal RAM. The speaker queue also needs the source
  // blocks to remain valid after playRaw returns, so three blocks are rotated.
  uint8_t *buffers[3] = {};
  for (auto &buffer : buffers)
    buffer = (uint8_t *)heap_caps_malloc(AUDIO_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buffers[0] || !buffers[1] || !buffers[2]) {
    for (auto buffer : buffers)
      free(buffer);
    audioFailed = true;
    audioIdle = true;
    vTaskDelete(nullptr);
    return;
  }
  audioReady = true;
  uint8_t bufferIndex = 0;
  AudioOutput active = AudioOutput::RCA;
  bool outputPaused = false;
  int64_t pcmDueUs = 0;
  for (;;) {
    audioIdle = false;
    AudioOutput wanted = audioOutput.load();
    if (wanted != active) {
      // Core2 internal audio and the RCA module share DATA and LRCK. Stop I2S1,
      // rather than merely filling it with zeroes, so it no longer drives those lines.
      if (active == AudioOutput::RCA) {
        i2s_zero_dma_buffer(I2S_NUM_1);
        i2s_stop(I2S_NUM_1);
        i2s_driver_uninstall(I2S_NUM_1);
        pinMode(RCA_BCK, INPUT);
      }
      if (active == AudioOutput::INTERNAL)
        if (M5.Speaker.isRunning())
          M5.Speaker.end();
      if (wanted == AudioOutput::INTERNAL) {
        if (!M5.Speaker.begin()) {
          audioFailed = true;
          playing = false;
          audioIdle = true;
          vTaskDelete(nullptr);
          return;
        }
        M5.Speaker.setVolume(255);
        Serial.printf("[M5RETRO] Audio: alto-falante interno, volume:%d\n", playbackVolume.load());
      } else if (wanted == AudioOutput::RCA) {
        if (!initExternalAudio()) {
          audioFailed = true;
          playing = false;
          audioIdle = true;
          vTaskDelete(nullptr);
          return;
        }
        Serial.println("[M5RETRO] Audio: RCA selecionado");
      } else {
        Serial.println("[M5RETRO] Audio: mudo selecionado");
      }
      active = wanted;
      outputPaused = false;
      pcmDueUs = 0;
    }
    // Pause must silence the physical output as well as stop file reads. Clear
    // the RCA DMA queue so a tap does not leave already-buffered PCM playing.
    if ((!playing && !weatherAudio.load()) || paused || !wavFile) {
      if (!outputPaused && active == AudioOutput::RCA) {
        i2s_zero_dma_buffer(I2S_NUM_1);
        i2s_stop(I2S_NUM_1);
      }
      if (!outputPaused && active == AudioOutput::INTERNAL)
        if (M5.Speaker.isRunning())
          M5.Speaker.end();
      outputPaused = true;
      pcmDueUs = 0;
      audioIdle = true;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (outputPaused) {
      if (active == AudioOutput::RCA)
        i2s_start(I2S_NUM_1);
      else if (active == AudioOutput::INTERNAL) {
        if (!M5.Speaker.begin()) {
          audioFailed = true;
          playing = false;
          audioIdle = true;
          vTaskDelete(nullptr);
          return;
        }
        M5.Speaker.setVolume(255);
      }
      outputPaused = false;
      pcmDueUs = 0;
    }
    uint8_t *buf = buffers[bufferIndex];
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      audioUnderruns++;
      continue;
    }
    size_t bytes;
    if (mp3Mode) {
      // MP3: decodifica para PCM 22050 Hz estéreo (resampler linear).
      const size_t samps = mp3ReadPcm(reinterpret_cast<int16_t *>(buf), AUDIO_CHUNK / 2);
      bytes = samps * sizeof(int16_t);
      xSemaphoreGive(sdMutex);
      if (!bytes) {
        playbackFinished = true;
        playing = false;
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
    } else {
      const uint32_t position = wavFile.position();
      if (position >= wavDataEnd) {
        if (weatherAudio.load()) {
          // Música de fundo do Weather Channel: rebobina e continua em loop.
          if (!wavFile.seek(wavDataStart))
            weatherAudio = false;
          xSemaphoreGive(sdMutex);
          vTaskDelay(pdMS_TO_TICKS(2));
          continue;
        }
        xSemaphoreGive(sdMutex);
        playbackFinished = true;
        playing = false;
        Serial.println("[M5RETRO] Fim do audio: programa concluido");
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      const size_t remaining = wavDataEnd - position;
      bytes = wavFile.read(buf, min(AUDIO_CHUNK, remaining));
      xSemaphoreGive(sdMutex);
      if (!bytes || bytes % wavBlockAlign) {
        if (weatherAudio.load()) {
          // Música do clima: falha transitória de leitura do SD. Rebobina e tenta de novo.
          audioUnderruns++;
          if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!wavFile.seek(wavDataStart))
              weatherAudio = false;
            xSemaphoreGive(sdMutex);
          }
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        audioStreamError = true;
        audioUnderruns++;
        playbackFinished = true;
        playing = false;
        Serial.println("[M5RETRO] ERRO: leitura WAV terminou antes do esperado");
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
    }
    playback::scalePcm(reinterpret_cast<int16_t *>(buf), bytes / sizeof(int16_t), playbackVolume.load());
    size_t delivered = bytes;
    if (active == AudioOutput::RCA) {
      size_t written = 0;
      esp_err_t ok = i2s_write(I2S_NUM_1, buf, bytes, &written, pdMS_TO_TICKS(100));
      if (ok != ESP_OK || written != bytes) {
        audioUnderruns++;
        audioStreamError = true;
        playbackFinished = true;
        playing = false;
      }
      delivered = written;
    } else if (active == AudioOutput::INTERNAL) {
      if (!M5.Speaker.playRaw((const int16_t *)buf, bytes / sizeof(int16_t), sampleRate, true, 1, 0, false)) {
        audioUnderruns++;
        audioStreamError = true;
        playbackFinished = true;
        playing = false;
      }
    }
    // i2s_write and playRaw may accept a block before it has physically played.
    // Pace every destination from the PCM sample count, so the video clock is
    // 15 FPS for this file instead of running at the SD/queue feed rate.
    const uint32_t frames = delivered / wavBlockAlign;
    if (!pcmDueUs)
      pcmDueUs = esp_timer_get_time();
    pcmDueUs += ((int64_t)frames * 1000000LL) / sampleRate;
    const int64_t waitUs = pcmDueUs - esp_timer_get_time();
    if (waitUs > 0)
      vTaskDelay(pdMS_TO_TICKS((waitUs + 999) / 1000));
    else if (waitUs < -50000) {
      pcmDueUs = esp_timer_get_time();
      audioUnderruns++;
    }
    // Advance the player clock only after this PCM block has had time to leave
    // the selected output. A queued block must not make the timer/video jump.
    if (playing)
      samplesPlayed.fetch_add(frames);
    bufferIndex = (bufferIndex + 1) % 3;
  }
}

int jpegDraw(JPEGDRAW *draw) {
  // Saída de vídeo SOMENTE na RCA (composta). Nenhum pixel de frame é espelhado
  // no LCD: isso libera a banda do barramento SPI compartilhado com o microSD.
  rca.pushImage(draw->x + (CRT_W - videoWidth) / 2, draw->y + (CRT_H - videoHeight) / 2, draw->iWidth,
                draw->iHeight, reinterpret_cast<const lgfx::rgb565_t *>(draw->pPixels));
  return 1;
}

bool validateMjpegStream() {
  mjpegReader.reset();
  return mjpegFile.seek(0);
}

bool readAndShowOneFrame(bool render) {
  if (!mjpegFile || !jpegBuffer)
    return false;
  size_t used = 0;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(150)) != pdTRUE) {
    videoReadError = true;
    return false;
  }
  const auto result = mjpegReader.next(mjpegFile, render ? jpegBuffer : nullptr, MAX_JPEG, used);
  xSemaphoreGive(sdMutex);
  if (result != playback::FrameResult::Ready) {
    videoReadError = result != playback::FrameResult::End;
    if (videoReadError) {
      jpegErrors++;
      Serial.printf("[M5RETRO] MJPEG invalido: %d\n", int(result));
    }
    return false;
  }
  if (!render)
    return true;
  const uint32_t started = millis();
  if (!jpeg.openRAM(jpegBuffer, used, jpegDraw)) {
    jpegErrors++;
    videoReadError = true;
    return false;
  }
  const int width = jpeg.getWidth(), height = jpeg.getHeight();
  if (width < 1 || height < 1 || width > 320 || height > 240) {
    jpeg.close();
    videoReadError = true;
    jpegErrors++;
    return false;
  }
  // A prévia no LCD foi removida. Mantém-se apenas a limpeza da RCA no primeiro
  // frame (ou quando a resolução muda), para o CRT iniciar sem lixo.
  const bool firstFrame = (videoFrameIndex == 0);
  if (width != videoWidth || height != videoHeight || firstFrame) {
    videoWidth = width;
    videoHeight = height;
    rca.fillScreen(TFT_BLACK);
  }
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  const bool decoded = jpeg.decode(0, 0, 0);
  jpeg.close();
  if (!decoded) {
    jpegErrors++;
    videoReadError = true;
    return false;
  }
  decodedFrames++;
  renderedFrames++;
  const uint32_t elapsed = millis() - started;
  jpegDecodeTotalMs += elapsed;
  jpegDecodeMaxMs = max(jpegDecodeMaxMs, elapsed);
  return true;
}

// Alvo temporário do callback de decodificação do pôster. JPEGDEC exige um
// ponteiro de função (não aceita lambda com captura), então o sprite-alvo é
// guardado aqui somente durante o jpeg.decode() do pôster.
static LGFX_Sprite *posterSprite = nullptr;

int posterDraw(JPEGDRAW *draw) {
  if (posterSprite)
    posterSprite->pushImage(draw->x, draw->y, draw->iWidth, draw->iHeight,
                            reinterpret_cast<const lgfx::rgb565_t *>(draw->pPixels));
  return 1;
}

bool drawStaticPoster(const String &dir) {
  // Pôster do programa (opcional): tenta "poster.jpg" na pasta do vídeo e, na
  // ausência, um pôster global em /M5RETRO/config. O decodificador desenha em
  // um sprite do tamanho nativo e depois escala/centraliza para 320x240.
  String posterPath = dir + "/poster.jpg";
  if (!SD.exists(posterPath))
    posterPath = String(CONFIG) + "/poster.jpg";

  bool drew = false;
  File poster = SD.open(posterPath, FILE_READ);
  if (poster) {
    const size_t size = poster.size();
    // Limita a leitura para não atrasar o PLAY: pôsteres ~320x240 cabem com
    // folga em 128 KB. Arquivos maiores ou inválidos caem no fallback sólido.
    if (size && size <= MAX_JPEG) {
      uint8_t *buf = (uint8_t *)ps_malloc(size);
      if (buf) {
        if (poster.read(buf, size) == size && jpeg.openRAM(buf, size, posterDraw)) {
          const int w = jpeg.getWidth(), h = jpeg.getHeight();
          if (w >= 1 && h >= 1 && w <= 320 && h <= 240) {
            LGFX_Sprite sprite;
            sprite.setPsram(true);
            sprite.setColorDepth(16);
            if (sprite.createSprite(w, h)) {
              posterSprite = &sprite;
              jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
              const bool decoded = jpeg.decode(0, 0, 0);
              posterSprite = nullptr;
              if (decoded) {
                // Mantém a proporção (letterbox) e centraliza em 320x240.
                M5.Display.fillScreen(TFT_BLACK);
                const float scale = min(320.0f / w, 240.0f / h);
                sprite.setPivot(w / 2.0f, h / 2.0f);
                sprite.pushRotateZoom(&M5.Display, 160, 120, 0, scale, scale);
                drew = true;
              }
              sprite.deleteSprite();
            }
          }
          jpeg.close();
        }
        free(buf);
      }
    }
    poster.close();
  }

  if (!drew) {
    // Fallback sem arquivo: fundo sólido + título, usando só primitivas M5GFX.
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.drawString(currentTitle, 160, 100);
  }
  // O pôster nunca bloqueia o PLAY: retorna true até no fallback. (false só
  // caberia se o LCD estivesse inacessível, o que não ocorre após M5.begin.)
  return true;
}

bool startProgram(const String &dir) {
  stopProgram();
  if (!audioReady || audioFailed || !dir.length())
    return false;
  // meta.json is optional for simple card folders. Without it, the standard
  // file names and 15 FPS are used so a valid MJPEG/WAV pair is still playable.
  String video = "video.mjpeg", audio = "audio.wav";
  currentTitle = dir.substring(dir.lastIndexOf('/') + 1);
  fps = 15.0f;
  File meta = SD.open(dir + "/meta.json", FILE_READ);
  if (meta) {
    JsonDocument d;
    DeserializationError error = deserializeJson(d, meta);
    meta.close();
    if (error) {
      Serial.println("[M5RETRO] meta.json invalido");
      return false;
    }
    currentTitle = (const char *)(d["title"] | currentTitle.c_str());
    fps = d["fps"] | 15.0f;
    video = (const char *)(d["video"] | video.c_str());
    audio = (const char *)(d["audio"] | audio.c_str());
  }
  if (!isfinite(fps) || fps < 1.0f || fps > 30.0f) {
    Serial.printf("[M5RETRO] FPS invalido no meta.json: %.2f\n", fps);
    return false;
  }
  if (video.indexOf("..") >= 0 || audio.indexOf("..") >= 0 || video.startsWith("/") || audio.startsWith("/"))
    return false;
  mjpegFile = SD.open(dir + "/" + video, FILE_READ);
  wavFile = SD.open(dir + "/" + audio, FILE_READ);
  if (!mjpegFile)
    Serial.printf("[M5RETRO] MJPEG nao abriu: %s/%s\n", dir.c_str(), video.c_str());
  if (!wavFile)
    Serial.printf("[M5RETRO] WAV nao abriu: %s/%s\n", dir.c_str(), audio.c_str());
  if (!mjpegFile || !wavFile || !openWavAndReadHeader() || !validateMjpegStream()) {
    if (mjpegFile)
      mjpegFile.close();
    if (wavFile)
      wavFile.close();
    return false;
  }
  samplesPlayed = 0;
  videoFrameIndex = 0;
  playbackFinished = false;
  audioStreamError = false;
  videoReadError = false;
  paused = false;
  state = VIDEO_PLAYBACK;
  setBacklight(true); // LCD visível por padrão, mostrando o pôster + HUD
  rca.fillScreen(TFT_BLACK);
  osdUntil = millis() + 3000;
  Serial.printf("[M5RETRO] Reproduzindo: %s video:%lu bytes wav:%lu bytes\n", dir.c_str(), mjpegFile.size(),
                wavDataEnd - wavDataStart);
  drawStaticPoster(dir);
  drawBackButton(); // botão voltar (canto sup. direito) sobre o pôster no LCD
  if (!readAndShowOneFrame()) {
    stopProgram();
    return false;
  }
  videoFrameIndex = 1;
  drawPlaybackController();
  playing = true;
  return true;
}
void stopProgram() {
  playing = false;
  // Wait for the consumer to acknowledge idle before closing or replacing files.
  while (audioReady && !audioIdle)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (mjpegFile)
    mjpegFile.close();
  if (wavFile)
    wavFile.close();
  mjpegReader.reset();
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  uiHudClear(); // apaga só o HUD; o pôster permanece intacto no LCD
  playbackFinished = false;
  state = VIDEO_LIBRARY;
}
void videoTick() {
  if (!playing || paused)
    return;
  const uint32_t target = uint32_t((double(samplesPlayed.load()) * fps) / sampleRate);
  // Catch up without JPEG decoding. Bound the work so navigation stays responsive.
  int skipped = 0;
  while (videoFrameIndex < target && skipped < 4) {
    if (!readAndShowOneFrame(false)) {
      playbackFinished = true;
      playing = false;
      return;
    }
    videoFrameIndex++;
    droppedFrames++;
    skipped++;
  }
  if (videoFrameIndex == target) {
    if (readAndShowOneFrame())
      videoFrameIndex++;
    else {
      playbackFinished = true;
      playing = false;
    }
  }
}

String stringAlias(JsonObject o, const char *a, const char *b = nullptr, const char *c = nullptr) {
  if (o[a].is<const char *>())
    return o[a].as<const char *>();
  if (b && o[b].is<const char *>())
    return o[b].as<const char *>();
  if (c && o[c].is<const char *>())
    return o[c].as<const char *>();
  return "";
}
double numberAlias(JsonObject o, const char *a, const char *b = nullptr, const char *c = nullptr) {
  if (!o[a].isNull())
    return o[a].as<double>();
  if (b && !o[b].isNull())
    return o[b].as<double>();
  if (c && !o[c].isNull())
    return o[c].as<double>();
  return 0;
}
bool parseAircraft(JsonDocument &doc) {
  JsonArray list;
  if (doc.is<JsonArray>())
    list = doc.as<JsonArray>();
  else if (doc["aircraft"].is<JsonArray>())
    list = doc["aircraft"].as<JsonArray>();
  else if (doc["data"].is<JsonArray>())
    list = doc["data"].as<JsonArray>();
  else if (doc["items"].is<JsonArray>())
    list = doc["items"].as<JsonArray>();
  else
    return false;
  aircraftCount = 0;
  for (JsonObject object : list) {
    if (aircraftCount >= 64)
      break;
    JsonVariant lat = object["lat"].isNull() ? object["latitude"] : object["lat"];
    JsonVariant lon = !object["lon"].isNull()   ? object["lon"]
                      : !object["lng"].isNull() ? object["lng"]
                                                : object["longitude"];
    if (!lat.is<double>() || !lon.is<double>())
      continue;
    const double latitude = lat.as<double>(), longitude = lon.as<double>();
    if (!isfinite(latitude) || !isfinite(longitude) || latitude < -90 || latitude > 90 || longitude < -180 ||
        longitude > 180)
      continue;
    Aircraft &p = aircraft[aircraftCount++];
    p.icao = stringAlias(object, "icao", "icao24", "hex");
    p.callsign = stringAlias(object, "callsign", "flight");
    p.latitude = latitude;
    p.longitude = longitude;
    p.valid_position = true;
    p.altitude_ft = numberAlias(object, "altitude_ft", "altitude", "baro_altitude");
    p.speed_kt = numberAlias(object, "speed_kt", "ground_speed_kt", "speed");
    p.heading_deg = numberAlias(object, "heading", "track");
    p.aircraft_type = stringAlias(object, "aircraft_type", "type", "model");
  }
  if (aircraftCount && settings.lat == 0 && settings.lon == 0) {
    double latSum = 0, lonSum = 0;
    for (int i = 0; i < aircraftCount; ++i) {
      latSum += aircraft[i].latitude;
      lonSum += aircraft[i].longitude;
    }
    settings.lat = latSum / aircraftCount;
    settings.lon = lonSum / aircraftCount;
  }
  if (radarSelection >= aircraftCount)
    radarSelection = aircraftCount - 1;
  return true;
}

void radarNetworkTask(void *argument) {
  auto *request = static_cast<RadarRequest *>(argument);
  auto *result = new RadarResponse;
  const uint32_t started = millis();
  {
    WiFiClientSecure client;
    client.setHandshakeTimeout(12);
    if (request->config.allowInsecureTls)
      client.setInsecure();
    else
      client.setCACert(request->ca.c_str());
    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin(client, request->config.baseUrl + request->config.endpoint))
      result->status = "SEM SERVIDOR";
    else {
      const SecretsConfig &c = request->config;
      if (c.authMode == "bearer")
        http.addHeader("Authorization", "Bearer " + c.token);
      else if (c.authMode == "x-api-key")
        http.addHeader("X-API-Key", c.token);
      else if (c.authMode == "custom-header")
        http.addHeader(c.authHeader, c.authPrefix + c.token);
      result->httpCode = http.GET();
      if (result->httpCode == HTTP_CODE_OK) {
        if (http.getSize() > 65536)
          result->status = "RESPOSTA MUITO GRANDE";
        else {
          auto error =
              deserializeJson(result->data, http.getStream(), DeserializationOption::NestingLimit(12));
          result->parsed = !error;
          result->status = error ? "RESPOSTA INVALIDA" : "CONECTADA";
        }
      } else if (result->httpCode == 401 || result->httpCode == 403)
        result->status = "ERRO AUTENTICACAO";
      else
        result->status = "SEM SERVIDOR";
    }
    http.end();
  }
  result->elapsedMs = millis() - started;
  delete request;
  radarResponse.store(result);
  vTaskDelete(nullptr);
}
void pollAircraft() {
  if (auto *result = radarResponse.exchange(nullptr)) {
    radarBusy = false;
    lastApiPoll = millis();
    lastHttpCode = result->httpCode;
    lastHttpMs = result->elapsedMs;
    if (state == AIRCRAFT_RADAR) {
      apiStatus = result->status;
      radarDirty = true; // redesenha apenas quando há resposta (fim da consulta)
      if (result->parsed) {
        if (parseAircraft(result->data))
          lastApiGood = millis();
        else
          apiStatus = "RESPOSTA INVALIDA";
      }
      Serial.printf("[M5RETRO] API HTTP:%d tempo:%lu ms aeronaves:%d status:%s\n", lastHttpCode, lastHttpMs,
                    aircraftCount, apiStatus.c_str());
    }
    delete result;
  }
  if (state != AIRCRAFT_RADAR || radarBusy || playing ||
      millis() - lastApiPoll < uint32_t(settings.refreshSeconds) * 1000)
    return;
  lastApiPoll = millis();
  if (!networkConfigPresent || !secrets.base.startsWith("https://") || !secrets.endpoint.startsWith("/")) {
    apiStatus = "NAO CONFIGURADA";
    return;
  }
  if (!network.connected()) {
    apiStatus = "SEM WI-FI";
    return;
  }
  if (!secrets.insecure && time(nullptr) < 1704067200) {
    apiStatus = "SINCRONIZANDO HORA";
    return;
  }
  auto *request = new RadarRequest;
  request->config = currentSecrets();
  if (!secrets.insecure) {
    if (sdMutex)
      xSemaphoreTake(sdMutex, portMAX_DELAY);
    File ca = SD.open(CA_FILE, FILE_READ);
    if (ca) {
      request->ca = ca.readString();
      ca.close();
    }
    if (sdMutex)
      xSemaphoreGive(sdMutex);
    if (!request->ca.length()) {
      delete request;
      apiStatus = "CERTIFICADO AUSENTE";
      return;
    }
  }
  radarBusy = true;
  apiStatus = "CONSULTANDO";
  // Keep TLS off core 0, where Wi-Fi and audio run. Idle priority lets the scheduler service the watchdog
  // during expensive certificate verification; loop() applies the response.
  if (xTaskCreatePinnedToCore(radarNetworkTask, "RADAR_HTTPS", 8192, request, 0, nullptr, 1) != pdPASS) {
    radarBusy = false;
    delete request;
    apiStatus = "SEM MEMORIA PARA API";
  }
}

String normalizeSdPath(const String &path) {
  // ESP32 FS directory entries can be reported with the internal VFS mount
  // prefix (/sd). SD.open and SD.exists require the public card path instead.
  if (path.startsWith("/sd/"))
    return path.substring(3);
  if (path == "/sd")
    return "/";
  return path;
}

String libraryChildPath(const String &root, const String &entryName) {
  // FAT directory iteration may return only a child name (for example
  // "primeiro-teste") instead of an absolute SD path. Always rebuild the
  // physical card path from the root currently being scanned.
  const String child = normalizeSdPath(entryName);
  if (child.startsWith("/"))
    return child;
  return root + "/" + child;
}

bool isProgramFolder(const String &path) {
  // A complete package can provide metadata, or use the documented default
  // stream names when it was prepared by an older exporter.
  const String cardPath = normalizeSdPath(path);
  const bool valid = SD.exists(cardPath + "/meta.json") ||
                     (SD.exists(cardPath + "/video.mjpeg") && SD.exists(cardPath + "/audio.wav"));
  Serial.printf("[M5RETRO] Pasta SD: %s %s\n", cardPath.c_str(), valid ? "PROGRAMA" : "IGNORADA");
  return valid;
}

String libraryRoot() {
  // New cards use /M5RETRO/videos. Older prepared cards often use /videos.
  // Select the first location that actually contains a playable program folder.
  const char *roots[] = {VIDEOS, "/videos"};
  for (const char *candidate : roots) {
    File root = SD.open(candidate);
    if (!root || !root.isDirectory()) {
      if (root)
        root.close();
      continue;
    }
    bool found = false;
    for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (entry.isDirectory() && isProgramFolder(libraryChildPath(candidate, String(entry.name()))))
        found = true;
      entry.close();
      if (found)
        break;
    }
    root.close();
    if (found)
      return String(candidate);
  }
  return String(VIDEOS);
}

int libraryProgramCount() {
  int count = 0;
  const String rootPath = libraryRoot();
  File root = SD.open(rootPath);
  if (!root)
    return 0;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory() && isProgramFolder(libraryChildPath(rootPath, String(entry.name()))))
      ++count;
    entry.close();
  }
  root.close();
  Serial.printf("[M5RETRO] Biblioteca: %d programa(s) em %s\n", count, rootPath.c_str());
  return count;
}

String libraryProgramAt(int wantedIndex) {
  int index = 0;
  const String rootPath = libraryRoot();
  File root = SD.open(rootPath);
  if (!root)
    return "";
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory() && isProgramFolder(libraryChildPath(rootPath, String(entry.name())))) {
      if (index++ == wantedIndex) {
        String path = libraryChildPath(rootPath, String(entry.name()));
        entry.close();
        root.close();
        return path;
      }
    }
    entry.close();
  }
  root.close();
  return "";
}

String playbackClock() {
  uint32_t seconds = sampleRate ? samplesPlayed / sampleRate : 0;
  char text[12];
  snprintf(text, sizeof(text), "%02lu:%02lu:%02lu", seconds / 3600UL, (seconds / 60UL) % 60UL,
           seconds % 60UL);
  return String(text);
}

void uiHudInit() {
  // Sprite do HUD criado UMA vez (fora do hot path). Cabe folgadamente no PSRAM
  // e é reaproveitado por toda a vida útil do programa.
  uiHud.setPsram(true);
  uiHud.setColorDepth(16);
  uiHud.createSprite(HUD_W, HUD_H);
}

void uiHudDraw() {
  if (!uiHud.getBuffer())
    return; // sprite indisponível (PSRAM esgotada): não há HUD a desenhar
  const uint32_t bytesPerSecond = sampleRate * 4UL;
  const uint32_t total = bytesPerSecond ? (wavDataEnd - wavDataStart) / bytesPerSecond : 0;
  char totalText[12];
  snprintf(totalText, sizeof(totalText), "%02lu:%02lu:%02lu", total / 3600UL, (total / 60UL) % 60UL,
           total % 60UL);
  const bool isPaused = paused.load();

  uiHud.fillRect(0, 0, HUD_W, HUD_H, TFT_BLACK);
  uiHud.setTextDatum(top_left);
  uiHud.setTextSize(1);
  uiHud.setTextColor(TFT_WHITE, TFT_BLACK);
  uiHud.drawString(String(isPaused ? "PAUSA " : "PLAY  ") + playbackClock() + " / " + totalText, 4, 1);

  // Barra de progresso proporcional a samplesPlayed (1 px de preenchimento).
  const int barX = 4, barY = 12, barW = HUD_W - 8;
  uiHud.drawRect(barX, barY, barW, 3, TFT_CYAN);
  const int progress =
      total ? constrain(int((samplesPlayed.load() / float(sampleRate)) * barW / total), 0, barW) : 0;
  if (progress)
    uiHud.fillRect(barX + 1, barY + 1, progress, 1, TFT_YELLOW);

  uiHud.pushSprite(0, HUD_Y); // uma única transferência SPI por redesenho
}

void uiHudClear() {
  // Apaga apenas a faixa do HUD, sem tocar no pôster que ocupa o restante.
  M5.Display.fillRect(0, HUD_Y, HUD_W, HUD_H, TFT_BLACK);
}

void uiHudTick(uint32_t now) {
  // Borda de 1 s baseada em millis() (sem FreeRTOS timer). Redesenha quando a
  // borda vence ou a pausa muda, para exibir "PAUSA" imediatamente.
  static bool lastPausedUi = false;
  const bool isPaused = paused.load();
  if (now - lastUiUpdate >= 1000 || isPaused != lastPausedUi) {
    lastPausedUi = isPaused;
    lastUiUpdate = now;
    uiHudDraw();
  }
}

void drawPlaybackController() {
  // A prévia no LCD foi removida para liberar o barramento SPI compartilhado.
  // O Core2 mostra apenas pôster + HUD mínimo, desenhado pelo sprite de 1 Hz
  // (uiHudDraw). Redesenhos por aqui são pontuais (início/toggle), nunca a cada
  // 250 ms; o OSD completo continua saindo exclusivamente pela RCA.
  uiHudDraw();
}

void drawPlaybackOsd() {
  static bool visible = false, lastPaused = false;
  static uint32_t lastPaint = 0, lastFrame = 0, lastDeadline = 0;
  const uint32_t now = millis();
  const bool isPaused = paused.load();
  const bool show = isPaused || !timeReached(now, osdUntil);
  if (visible && !show) {
    const int belowPicture = max(202, (CRT_H + videoHeight) / 2);
    if (belowPicture < CRT_H)
      rca.fillRect(0, belowPicture, CRT_W, CRT_H - belowPicture, TFT_BLACK);
    visible = false;
  }
  // Repaint over new video frames, or at 4 Hz for the clock/controls, not on every loop.
  if (show && (!visible || lastFrame != videoFrameIndex || lastPaused != isPaused ||
               lastDeadline != osdUntil || now - lastPaint >= 250)) {
    visible = true;
    lastPaint = now;
    lastFrame = videoFrameIndex;
    lastPaused = isPaused;
    lastDeadline = osdUntil;
    rca.fillRect(0, 202, 320, 38, TFT_NAVY);
    rca.setTextDatum(top_left);
    rca.setTextSize(1);
    rca.setTextColor(TFT_WHITE, TFT_NAVY);
    if (settings.vhsOsd)
      rca.drawString(String(paused ? "PAUSE" : "PLAY  SP") + "   " + playbackClock(), 6, 205);
    else
      rca.drawString(String(PTBR::APP) + "  " + currentTitle, 6, 205);
    rca.setTextColor(TFT_CYAN, TFT_NAVY);
    rca.drawString(String("[ ") + PTBR::ANTERIOR + " ]  [ " + (paused ? PTBR::REPRODUZIR : PTBR::PAUSAR) +
                       " ]  [ " + PTBR::PROXIMO + " ]",
                   6, 222);
  }
  // O redesenho do LCD (pôster + HUD) não acontece mais aqui a cada 250 ms: a
  // borda de 1 s é feita por uiHudTick() chamado em loop(), logo após este OSD.
}

void setAudioOutput(AudioOutput output) {
  // Let the current PCM block finish before persisting to the shared SD card.
  // The audio task changes I2S ownership while paused, keeping file position and
  // the video clock stable instead of starving a live SD read during the save.
  const bool wasPaused = paused.load();
  paused = true;
  while (audioReady && !audioIdle && !audioFailed)
    vTaskDelay(pdMS_TO_TICKS(1));
  settings.audioOutput = output;
  audioOutput = output;
  saveSettings();
  paused = wasPaused;
  osdUntil = millis() + 3000;
}

void servicePowerButton() {
  // M5.BtnPWR is M5Unified's debounced event for the Core2 side Power key.
  // M5.update() runs before this function, so one physical press becomes one
  // shutdown request. The AXP192 then restores power on the next Power press.
  if (!powerOffPending && M5.BtnPWR.wasClicked()) {
    powerOffPending = true;
    powerOffAt = millis() + 120;
    playing = false;
    paused = true;
    if (portal.active())
      portal.stop();
    WiFi.disconnect(false, false);
    Serial.println("[M5RETRO] Botao Power: desligamento solicitado");
    dualText("DESLIGANDO...", "APERTE POWER PARA LIGAR");
    return;
  }
  if (powerOffPending && (int32_t)(millis() - powerOffAt) >= 0) {
    stopProgram();
    Serial.println("[M5RETRO] Alimentacao do Core2 desligada pelo AXP192");
    Serial.flush();
    // audioTask acknowledged idle and stopped its current output.
    M5.Power.powerOff();
  }
}

void drawControllerLabels(const char *left, const char *center, const char *right) {
  const char *labels[3] = {left, center, right};
  M5.Display.fillRect(0, 184, 320, 56, TFT_NAVY);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  for (int i = 0; i < 3; ++i) {
    const int x = i == 0 ? 6 : i == 1 ? 110 : 214;
    const int w = i == 1 ? 100 : 100;
    const bool hot = input.highlightActive() && input.highlightedButton() == i;
    uint32_t fill = hot ? TFT_CYAN : TFT_BLUE;
    uint32_t ink = hot ? TFT_NAVY : TFT_WHITE;
    M5.Display.fillRoundRect(x, 190, w, 43, 4, fill);
    M5.Display.drawRoundRect(x, 190, w, 43, 4, TFT_WHITE);
    M5.Display.setTextColor(ink, fill);
    M5.Display.drawString(labels[i], x + w / 2, 211);
  }
  if (!timeReached(millis(), osdUntil)) {
    rca.fillRect(0, 220, 320, 20, TFT_NAVY);
    rca.setTextDatum(middle_center);
    rca.setTextSize(1);
    rca.setTextColor(TFT_CYAN, TFT_NAVY);
    rca.drawString(String("[ ") + left + " ]  [ " + center + " ]  [ " + right + " ]", 160, 230);
  }
  drawBackButton();
}
// ============================================================================
// Player de música (F1): navegação por pastas estilo iPod + reprodução de WAV.
// F3 adiciona MP3 via libhelix; F2/F4 adicionam tags ID3 e capa do álbum.
// ============================================================================

static bool isMusicTrack(const String &name) {
  String n = name;
  n.toLowerCase();
  return n.endsWith(".wav") || n.endsWith(".mp3");
}

static String musicParentDir(const String &path) {
  const int s = path.lastIndexOf('/');
  if (s <= 0)
    return String("/");
  const String p = path.substring(0, s);
  return p.length() ? p : String("/");
}

static void musicScanDir() {
  musicEntryCount = 0;
  musicSelection = 0;
  musicQueueCount = 0;
  // Entrada ".." para subir, quando não estamos na raiz de música.
  if (musicDir != String(MUSIC_ROOT)) {
    musicEntries[musicEntryCount].name = "..";
    musicEntries[musicEntryCount].path = musicParentDir(musicDir);
    musicEntries[musicEntryCount].isFolder = true;
    ++musicEntryCount;
  }
  File dir = SD.open(musicDir);
  if (!dir || !dir.isDirectory()) {
    if (dir)
      dir.close();
    return;
  }
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const String rawName = String(entry.name());
    if (rawName == "." || rawName == "..")
      continue;
    const bool folder = entry.isDirectory();
    if (!folder && !isMusicTrack(rawName))
      continue;
    if (musicEntryCount < 32) {
      MusicEntry &e = musicEntries[musicEntryCount];
      e.isFolder = folder;
      e.path = musicDir + "/" + rawName;
      char buf[48];
      ascii::normalize(buf, sizeof(buf), rawName.c_str()); // acentos -> ASCII
      e.name = buf;
      ++musicEntryCount;
    }
    if (!folder && musicQueueCount < 64)
      musicQueue[musicQueueCount++] = musicDir + "/" + rawName;
    entry.close();
  }
  dir.close();
}

void drawMusicBrowser() {
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(top_left);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->setTextSize(2);
    d->drawString(PTBR::MUSICA, 12, 8);
    d->drawFastHLine(8, 36, 304, TFT_CYAN);
    // breadcrumb do diretório corrente.
    String crumb = musicDir;
    crumb.replace(String(MUSIC_ROOT), "/");
    d->setTextSize(1);
    d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
    d->drawString(crumb, 12, 42);
    if (!musicEntryCount)
      d->drawString(PTBR::SEM_MUSICAS, 30, 92);
    const int first = (musicSelection / 4) * 4;
    for (int row = 0; row < 4 && first + row < musicEntryCount; ++row) {
      const int index = first + row, y = 56 + row * 32;
      const bool selected = index == musicSelection;
      d->fillRoundRect(12, y - 2, 296, 28, 4, selected ? TFT_CYAN : TFT_NAVY);
      d->setTextColor(selected ? TFT_NAVY : (musicEntries[index].isFolder ? TFT_CYAN : TFT_WHITE),
                      selected ? TFT_CYAN : TFT_NAVY);
      String label = String(selected ? "> " : "  ") + musicEntries[index].name;
      if (musicEntries[index].isFolder)
        label += "/";
      d->drawString(label.substring(0, 40), 20, y + 6);
    }
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(String(musicEntryCount ? musicSelection + 1 : 0) + " / " + musicEntryCount, 216, 16);
  }
  drawControllerLabels("ACIMA", "OK", "ABAIXO");
}

static int coverDraw(JPEGDRAW *draw) {
  if (coverTarget)
    coverTarget->pushImage(draw->x, draw->y, draw->iWidth, draw->iHeight,
                           reinterpret_cast<const lgfx::rgb565_t *>(draw->pPixels));
  return 1;
}

static void freeCover() {
  if (coverSprite) {
    coverSprite->deleteSprite();
    delete coverSprite;
    coverSprite = nullptr;
  }
}

// Carrega e decodifica a capa do álbum para `coverSprite` (PSRAM RGB565).
// Precedência: cover.jpg/folder.jpg/front.jpg na pasta do álbum -> APIC embutido.
static bool loadAlbumCover(const String &trackPath) {
  freeCover();
  uint8_t *buf = nullptr;
  size_t size = 0;

  const int slash = trackPath.lastIndexOf('/');
  const String dir = slash > 0 ? trackPath.substring(0, slash) : String("/");
  const char *names[] = {"cover.jpg", "folder.jpg", "front.jpg"};
  for (const char *n : names) {
    const String p = dir + "/" + n;
    if (SD.exists(p)) {
      File f = SD.open(p, FILE_READ);
      if (f && f.size() && f.size() <= MAX_JPEG) {
        size = f.size();
        buf = (uint8_t *)ps_malloc(size);
        if (buf && f.read(buf, size) != size) {
          free(buf);
          buf = nullptr;
        }
      }
      if (f)
        f.close();
      if (buf)
        break;
    }
  }

  // APIC embutido (offset/tamanho já lidos pelo Id3.h).
  if (!buf && musicMeta.hasCover && musicMeta.coverSize && musicMeta.coverSize <= MAX_JPEG) {
    File f = SD.open(trackPath, FILE_READ);
    if (f && f.seek(musicMeta.coverOffset)) {
      size = musicMeta.coverSize;
      buf = (uint8_t *)ps_malloc(size);
      if (buf && f.read(buf, size) != size) {
        free(buf);
        buf = nullptr;
      }
    }
    if (f)
      f.close();
  }

  if (!buf)
    return false;

  if (!jpeg.openRAM(buf, size, coverDraw)) {
    free(buf);
    return false;
  }
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  const int fullW = jpeg.getWidth(), fullH = jpeg.getHeight();
  if (fullW < 1 || fullH < 1) {
    jpeg.close();
    free(buf);
    return false;
  }
  int options = 0, scale = 1;
  if (fullW > 220) {
    options = JPEG_SCALE_QUARTER;
    scale = 4;
  } else if (fullW > 130) {
    options = JPEG_SCALE_HALF;
    scale = 2;
  }
  const int w = (fullW + scale - 1) / scale;
  const int h = (fullH + scale - 1) / scale;

  coverSprite = new LGFX_Sprite();
  coverSprite->setPsram(true);
  coverSprite->setColorDepth(16);
  if (!coverSprite->createSprite(w, h)) {
    delete coverSprite;
    coverSprite = nullptr;
    jpeg.close();
    free(buf);
    return false;
  }
  coverTarget = coverSprite;
  const bool ok = jpeg.decode(0, 0, options);
  coverTarget = nullptr;
  jpeg.close();
  free(buf);
  if (!ok)
    freeCover();
  return ok;
}

void drawMusicNowPlaying() {
  char title[64] = "MUSICA";
  if (musicMeta.title[0])
    strcpy(title, musicMeta.title);
  else if (musicQueueIndex >= 0 && musicQueueIndex < musicQueueCount) {
    const String p = musicQueue[musicQueueIndex];
    const int s = p.lastIndexOf('/');
    ascii::normalize(title, sizeof(title), (s >= 0 ? p.substring(s + 1) : p).c_str());
  }
  const bool isPaused = paused.load();
  const uint32_t totalSec = (sampleRate && wavDataEnd >= wavDataStart) ? (wavDataEnd - wavDataStart) / (sampleRate * 4UL) : 0;
  const uint32_t curSec = sampleRate ? samplesPlayed.load() / sampleRate : 0;
  const int pct = totalSec ? (int)((uint64_t)curSec * 286 / totalSec) : 0;
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(top_left);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->setTextSize(2);
    d->drawString(PTBR::MUSICA, 12, 8);
    d->drawFastHLine(8, 36, 304, TFT_CYAN);
    d->setTextSize(1);
    // Capa do álbum (esquerda) com borda estilo VHS.
    if (coverSprite) {
      const int cw = coverSprite->width(), ch = coverSprite->height();
      const float sc = min(min(88.0f / cw, 88.0f / ch), 1.6f);
      coverSprite->setPivot(cw / 2.0f, ch / 2.0f);
      coverSprite->pushRotateZoom(d, 16 + 44, 52 + 44, 0, sc, sc);
    }
    d->drawRect(16, 52, 88, 88, coverSprite ? TFT_CYAN : TFT_DARKCYAN);
    // Título + tags.
    d->setTextColor(TFT_YELLOW, TFT_NAVY);
    d->drawString(title, 120, 52);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(musicMeta.artist[0] ? musicMeta.artist : "---", 120, 82);
    d->drawString(musicMeta.album[0] ? musicMeta.album : "---", 120, 102);
    if (musicMeta.year[0])
      d->drawString(musicMeta.year, 120, 122);
    // Progresso.
    d->drawRect(16, 170, 288, 6, TFT_CYAN);
    if (pct > 0)
      d->fillRect(17, 171, pct, 4, TFT_YELLOW);
    d->setTextColor(TFT_CYAN, TFT_NAVY);
    d->drawString(String(isPaused ? "PAUSA " : "PLAY  ") + playbackClock(), 16, 184);
  }
  drawBackButton();
}

static bool mp3Begin(const String &path) {
  mp3End();
  mp3File = SD.open(path, FILE_READ);
  if (!mp3File)
    return false;
  mp3Dec = MP3InitDecoder();
  if (!mp3Dec) {
    mp3File.close();
    return false;
  }
  mp3Mode = true;
  mp3PcmCount = 0;
  mp3InLen = 0;
  mp3Phase = 0.0;
  mp3Rate = 44100;
  mp3Chans = 2;
  return true;
}

static void mp3End() {
  if (mp3Dec) {
    MP3FreeDecoder(mp3Dec);
    mp3Dec = nullptr;
  }
  if (mp3File)
    mp3File.close();
  mp3Mode = false;
}

// Decodifica MP3 -> PCM 22050 Hz estéreo (resampler linear + mono->estéreo).
// Retorna o número de shorts estéreo produzidos em dst (<= samples).
static size_t mp3ReadPcm(int16_t *dst, size_t samples) {
  size_t produced = 0;
  while (produced + 1 < samples) {
    // Garante um frame decodificado no buffer de saída.
    if (mp3PcmCount == 0) {
      if (mp3InLen < 2048) {
        const size_t got = mp3File.read(mp3In + mp3InLen, sizeof(mp3In) - mp3InLen);
        if (!got && mp3InLen == 0)
          break; // EOF
        mp3InLen += got;
      }
      unsigned char *in = mp3In;
      int bytesLeft = (int)mp3InLen;
      const int samps = MP3Decode(mp3Dec, &in, &bytesLeft, mp3FramePcm, 0);
      const size_t consumed = (size_t)(in - mp3In);
      if (consumed > 0) {
        memmove(mp3In, in, mp3InLen - consumed);
        mp3InLen -= consumed;
      }
      if (samps > 0) {
        mp3PcmCount = (size_t)samps;
        mp3Phase = 0.0;
        MP3FrameInfo info;
        MP3GetLastFrameInfo(mp3Dec, &info);
        mp3Rate = info.samprate;
        mp3Chans = info.nChans;
      } else if (consumed == 0 && !mp3File.available()) {
        break; // EOF real
      } else {
        continue; // erro/necessita de mais dados
      }
    }
    const int ch = mp3Chans;
    const double ratio = (double)mp3Rate / 22050.0;
    const int framesIn = (int)(mp3PcmCount / ch);
    const int idx = (int)mp3Phase;
    if (idx >= framesIn - 1) {
      mp3PcmCount = 0; // frame consumido
      continue;
    }
    const float frac = (float)(mp3Phase - idx);
    for (int c = 0; c < 2; ++c) {
      const int sc = (ch == 1) ? 0 : c;
      const int16_t a = mp3FramePcm[idx * ch + sc];
      const int16_t b = mp3FramePcm[(idx + 1) * ch + sc];
      dst[produced++] = (int16_t)(a + (b - a) * frac);
    }
    mp3Phase += ratio;
  }
  return produced;
}

bool startMusic(const String &path) {
  // Garante I2S1 livre: encerra qualquer vídeo/weather em andamento.
  playing = false;
  while (audioReady && !audioIdle && !audioFailed)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (mjpegFile)
    mjpegFile.close();
  if (wavFile)
    wavFile.close();
  mp3End();
  mjpegReader.reset();
  String ext = path;
  ext.toLowerCase();
  const bool isMp3 = ext.endsWith(".mp3");
  bool ok = false;
  musicMeta.reset();
  if (isMp3) {
    ok = mp3Begin(path);
    if (ok) {
      sampleRate = 22050;
      wavChannels = 2;
      wavBlockAlign = 4;
      wavDataStart = 0;
      // Duração total estimada (assume ~128 kbps) para a barra de progresso.
      const uint32_t estSec = (uint32_t)(mp3File.size() * 8ULL / 128000ULL);
      wavDataEnd = estSec * 22050UL * 4UL;
      id3::readTags(mp3File, musicMeta);
    }
  } else {
    wavFile = SD.open(path, FILE_READ);
    ok = wavFile && openWavAndReadHeader();
    if (ok)
      id3::readTags(wavFile, musicMeta); // melhor esforço (WAV raramente tem ID3)
    else if (wavFile) {
      wavFile.close();
      Serial.printf("[M5RETRO] Musica: WAV invalido %s\n", path.c_str());
    }
  }
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  if (!ok)
    return false;
  samplesPlayed = 0;
  playbackFinished = false;
  audioStreamError = false;
  paused = false;
  playing = true;
  // Reconstrói a fila com as faixas do diretório da faixa tocando.
  musicQueueCount = 0;
  musicQueueIndex = -1;
  const int slash = path.lastIndexOf('/');
  const String dir = slash > 0 ? path.substring(0, slash) : String("/");
  File d = SD.open(dir);
  if (d && d.isDirectory()) {
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      const String n = String(e.name());
      if (!e.isDirectory() && isMusicTrack(n)) {
        const String p = dir + "/" + n;
        if (musicQueueCount < 64) {
          if (p == path)
            musicQueueIndex = musicQueueCount;
          musicQueue[musicQueueCount++] = p;
        }
      }
      e.close();
    }
    d.close();
  }
  if (musicQueueIndex < 0 && musicQueueCount < 64) {
    musicQueueIndex = musicQueueCount;
    musicQueue[musicQueueCount++] = path;
  }
  loadAlbumCover(path); // capa do álbum (folder.jpg/cover.jpg ou APIC)
  drawMusicNowPlaying();
  return true;
}

void stopMusic() {
  playing = false;
  while (audioReady && !audioIdle && !audioFailed)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (wavFile)
    wavFile.close();
  mp3End(); // fecha mp3File + libera o decodificador libhelix
  mjpegReader.reset();
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  freeCover();
}

void musicTick() {
  if (!playbackFinished)
    return;
  playbackFinished = false;
  if (audioStreamError) {
    audioStreamError = false;
    stopMusic();
    state = MUSIC_BROWSER;
    drawMusicBrowser();
    return;
  }
  // Fim natural da faixa: avança para a próxima, ou volta ao navegador.
  if (musicQueueCount && musicQueueIndex + 1 < musicQueueCount) {
    ++musicQueueIndex;
    if (!startMusic(musicQueue[musicQueueIndex])) {
      stopMusic();
      state = MUSIC_BROWSER;
      drawMusicBrowser();
    }
  } else {
    stopMusic();
    state = MUSIC_BROWSER;
    drawMusicBrowser();
  }
}

void drawHome() {
  const char *items[] = {PTBR::VIDEOS,    PTBR::MUSICA,   PTBR::TRAFEGO,
                         PTBR::CONFIGURACOES, PTBR::INFO_SISTEMA, PTBR::WEATHER};
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.drawString(PTBR::APP, 12, 8);
  M5.Display.drawFastHLine(8, 36, 304, TFT_CYAN);
  M5.Display.setTextSize(2);
  for (int i = 0; i < 6; i++) {
    int y = 40 + i * 22;
    bool selected = i == homeSelection;
    if (selected)
      M5.Display.fillRoundRect(12, y - 2, 296, 20, 4, TFT_CYAN);
    M5.Display.setTextColor(selected ? TFT_NAVY : TFT_WHITE, selected ? TFT_CYAN : TFT_NAVY);
    M5.Display.drawString(String(selected ? "> " : "  ") + items[i], 24, y);
  }
  rca.fillScreen(TFT_NAVY);
  rca.setTextDatum(top_left);
  rca.setTextSize(2);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(PTBR::APP, 12, 8);
  rca.drawFastHLine(8, 36, 304, TFT_CYAN);
  rca.setTextSize(1);
  for (int i = 0; i < 6; i++) {
    int y = 46 + i * 22;
    rca.setTextColor(i == homeSelection ? TFT_CYAN : TFT_WHITE, TFT_NAVY);
    rca.drawString(String(i == homeSelection ? "> " : "  ") + items[i], 28, y);
  }
  drawControllerLabels("ACIMA", "OK", "ABAIXO");
}
void drawLibrary() {
  const int count = libraryProgramCount();
  librarySelection = count ? constrain(librarySelection, 0, count - 1) : 0;
  const int first = (librarySelection / 4) * 4;
  for (auto *display : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    display->fillScreen(TFT_NAVY);
    display->setTextDatum(top_left);
    display->setTextSize(2);
    display->setTextColor(TFT_WHITE, TFT_NAVY);
    display->drawString(PTBR::VIDEOS, 12, 8);
    display->drawFastHLine(8, 36, 304, TFT_CYAN);
    display->setTextSize(1);
    if (!count)
      display->drawString("SEM VIDEOS NO CARTAO", 30, 92);
    for (int row = 0; row < 4 && first + row < count; ++row) {
      const int index = first + row, y = 50 + row * 32;
      String path = libraryProgramAt(index);
      String title = path.substring(path.lastIndexOf('/') + 1);
      const bool selected = index == librarySelection;
      display->fillRoundRect(12, y - 2, 296, 28, 4, selected ? TFT_CYAN : TFT_NAVY);
      display->setTextColor(selected ? TFT_NAVY : TFT_WHITE, selected ? TFT_CYAN : TFT_NAVY);
      display->drawString(String(selected ? "> " : "  ") + title.substring(0, 40), 20, y + 6);
    }
    display->setTextColor(TFT_WHITE, TFT_NAVY);
    display->drawString(String(count ? librarySelection + 1 : 0) + " / " + count, 216, 16);
  }
  drawControllerLabels("ANTERIOR", "PLAY", "PROXIMO");
}

// Desenha um "aviaozinho" top-down orientado pela proa (0 = norte, horario).
// Coordenadas locais: lx = direita, ly = frente (nariz). Rotaciona por heading.
static void drawAirplane(M5GFX *d, int px, int py, double headingDeg, uint16_t color) {
  const double a = headingDeg * DEG_TO_RAD;
  const double c = cos(a), s = sin(a);
  auto rot = [&](int lx, int ly, int &sx, int &sy) {
    sx = px + (int)lround(lx * c + ly * s);
    sy = py + (int)lround(lx * s - ly * c);
  };
  int x0, y0, x1, y1;
  // Fuselagem (nariz -> cauda).
  rot(0, 3, x0, y0);
  rot(0, -3, x1, y1);
  d->drawLine(x0, y0, x1, y1, color);
  // Nariz (triângulo apontando para a frente).
  int nx, ny;
  rot(0, 3, nx, ny);
  rot(-2, 1, x0, y0);
  rot(2, 1, x1, y1);
  d->fillTriangle(nx, ny, x0, y0, x1, y1, color);
  // Asas (barra transversal em ly = 0).
  rot(-5, 0, x0, y0);
  rot(5, 0, x1, y1);
  d->drawLine(x0, y0, x1, y1, color);
  // Cauda (barra menor em ly = -2).
  rot(-2, -2, x0, y0);
  rot(2, -2, x1, y1);
  d->drawLine(x0, y0, x1, y1, color);
}

void drawRadar() {
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);

    // Título (canto superior esquerdo).
    d->setTextDatum(top_left);
    d->setTextSize(1);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(PTBR::APP, 10, 7);
    d->drawString(PTBR::TRAFEGO, 10, 22);

    // Scope: anel externo (alcance total), anel interno (metade) e mira.
    const int cx = 105, cy = 105, R = 62;
    d->drawCircle(cx, cy, R, TFT_CYAN);
    d->drawCircle(cx, cy, R / 2, TFT_DARKCYAN);
    d->drawFastHLine(cx - R, cy, 2 * R, TFT_DARKCYAN);
    d->drawFastVLine(cx, cy - R, 2 * R, TFT_DARKCYAN);

    // Pontos cardeais (fora do anel) + alcance na base do scope.
    d->setTextDatum(middle_center);
    d->setTextColor(TFT_CYAN, TFT_NAVY);
    d->drawString("N", cx, cy - R - 8);
    d->drawString("S", cx, cy + R + 8);
    d->drawString("W", cx - R - 8, cy);
    d->drawString("E", cx + R + 8, cy);
    d->setTextDatum(top_left);
    d->drawString(String(settings.rangeKm) + " km", cx - R, cy + R + 4);

    // Aeronaves (norte = cima, leste = direita). A selecionada ganha uma
    // linha-guia a partir do centro, além do triângulo destacado.
    d->setTextDatum(top_left);
    for (int i = 0; i < aircraftCount; i++) {
      double y = (aircraft[i].latitude - settings.lat) * 111.0;
      double x = (aircraft[i].longitude - settings.lon) * 111.0 * cos(settings.lat * DEG_TO_RAD);
      if (x * x + y * y > double(settings.rangeKm) * settings.rangeKm)
        continue;
      int px = cx + (int)(x / settings.rangeKm * R), py = cy - (int)(y / settings.rangeKm * R);
      if (sq(px - cx) + sq(py - cy) >= sq(R))
        continue;
      const bool sel = (i == radarSelection);
      if (sel)
        d->drawLine(cx, cy, px, py, TFT_CYAN);
      drawAirplane(d, px, py, aircraft[i].heading_deg, sel ? TFT_CYAN : TFT_YELLOW);
    }

    // Painel lateral de dados (à direita do scope), sempre acima da barra de
    // controle (y < 184).
    const int PX = 178;
    d->setTextDatum(top_left);
    if (radarSelection >= 0 && radarSelection < aircraftCount) {
      const Aircraft &p = aircraft[radarSelection];
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->drawString(p.callsign.length() ? p.callsign : p.icao, PX, 28);
      d->setTextColor(TFT_CYAN, TFT_NAVY);
      d->drawString("ALT FL" + String((int)(p.altitude_ft / 100)), PX, 48);
      d->drawString("VEL " + String((int)p.speed_kt) + " KT", PX, 66);
      double y = (p.latitude - settings.lat) * 111.0;
      double x = (p.longitude - settings.lon) * 111.0 * cos(settings.lat * DEG_TO_RAD);
      int distKm = (int)sqrt(x * x + y * y);
      int proa = (int)(atan2(x, y) * RAD_TO_DEG);
      if (proa < 0)
        proa += 360;
      d->drawString("DIST " + String(distKm) + " km  PROA " + String(proa), PX, 84);
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      if (radarDetails) {
        d->drawString(p.icao, PX, 112);
        d->drawString(p.aircraft_type, PX, 128);
      }
    } else {
      d->setTextColor(TFT_CYAN, TFT_NAVY);
      d->drawString(PTBR::AERONAVES_RASTREADAS, PX, 40);
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->drawString(String(aircraftCount), PX, 62);
    }
    d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
    d->drawString(String(settings.rangeKm) + " km de alcance", PX, 150);
    d->setTextColor(TFT_CYAN, TFT_NAVY);
    d->drawString(apiStatus.substring(0, 22), PX, 166);
    if (!aircraftCount) {
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->setTextDatum(middle_center);
      d->drawString(PTBR::NENHUMA_AERONAVE, cx, cy);
      d->setTextDatum(top_left);
    }
  }
  drawControllerLabels("ANTERIOR", "DETALHES", "PROXIMO");
}
// ============================================================================
// Weather Channel — "Local Forecast" dos anos 80 (item do menu principal)
// ============================================================================

static void asciiCopy(char *dst, size_t cap, const char *src) {
  size_t j = 0;
  if (src)
    for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
      unsigned char c = (unsigned char)src[i];
      if (c >= 0x20 && c < 0x7F)
        dst[j++] = c;
    }
  dst[j] = 0;
}
static void asciiUpper(char *s) {
  for (; *s; ++s)
    if (*s >= 'a' && *s <= 'z')
      *s -= 'a' - 'A';
}
// Dia da semana (0=DOM..6=SAB) a partir de "AAAA-MM-DD" (congruência de Zeller).
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

static bool weatherParse(JsonDocument &doc, WeatherData &out) {
  JsonArray cc = doc["current_condition"];
  if (cc.isNull() || cc.size() == 0)
    return false;
  JsonObject c = cc[0];
  if (c.isNull())
    return false;
  out.tempC = c["temp_C"] | -100;
  out.humidity = c["humidity"] | 0;
  out.windKmph = c["windspeedKmph"] | 0;
  asciiCopy(out.cond, sizeof(out.cond), c["weatherDesc"][0]["value"] | "");
  asciiCopy(out.windDir, sizeof(out.windDir), c["winddir16Point"] | "");
  asciiUpper(out.cond);
  asciiUpper(out.windDir);
  JsonArray days = doc["weather"];
  if (days.isNull() || days.size() < 3)
    return false;
  for (int i = 0; i < 3; ++i) {
    JsonObject d = days[i];
    if (d.isNull())
      return false;
    const char *iso = d["date"] | "";
    snprintf(out.days[i].name, sizeof(out.days[i].name), "%s", weekdayPt(weekdayFromIso(iso)));
    out.days[i].maxC = d["maxtempC"] | 0;
    out.days[i].minC = d["mintempC"] | 0;
  }
  return true;
}

static void weatherFilterBuild(JsonDocument &filter) {
  filter["current_condition"][0]["temp_C"] = true;
  filter["current_condition"][0]["humidity"] = true;
  filter["current_condition"][0]["weatherDesc"][0]["value"] = true;
  filter["current_condition"][0]["windspeedKmph"] = true;
  filter["current_condition"][0]["winddir16Point"] = true;
  for (int i = 0; i < 3; ++i) {
    filter["weather"][i]["date"] = true;
    filter["weather"][i]["maxtempC"] = true;
    filter["weather"][i]["mintempC"] = true;
  }
}

static bool weatherFetch(WeatherData &out) {
  WiFiClientSecure client;
  client.setInsecure(); // sem CA (economiza RAM)
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  bool ok = false;
  if (http.begin(client, "https://wttr.in/Franca,Brazil?format=j1")) {
    const int code = http.GET();
    if (code == HTTP_CODE_OK) {
      // Lê o corpo para um buffer contíguo em PSRAM e parseia a partir dele:
      // o leitor Stream+Filter do ArduinoJson 7.4.2 descarta campos escalares no
      // ESP32, e getString() retorna vazio com HTTP/1.0. Aqui unimos getStream()
      // (comprovado no radar) + filtro sobre buffer (comprovado em teste nativo).
      const int cap = 24000;
      char *buf = (char *)ps_malloc(cap);
      if (buf) {
        Stream &s = http.getStream();
        size_t len = 0;
        uint32_t quiet = 0;
        while (len < cap - 1 && quiet < 1500) {
          int avail = s.available();
          if (avail > 0) {
            int n = s.readBytes(buf + len, min(avail, cap - 1 - (int)len));
            if (n <= 0)
              break;
            len += n;
            quiet = 0;
          } else {
            delay(5);
            quiet += 5;
          }
        }
        buf[len] = 0;
        if (len) {
          JsonDocument filter;
          weatherFilterBuild(filter);
          JsonDocument doc;
          auto err = deserializeJson(doc, buf, DeserializationOption::Filter(filter),
                                     DeserializationOption::NestingLimit(8));
          if (!err)
            ok = weatherParse(doc, out);
          else
            Serial.printf("[M5RETRO] Tempo: parse %s (%u bytes)\n", err.c_str(), (unsigned)len);
        } else {
          Serial.println("[M5RETRO] Tempo: corpo vazio");
        }
        free(buf);
      }
    } else {
      Serial.printf("[M5RETRO] Tempo: HTTP %d\n", code);
    }
  } else {
    Serial.println("[M5RETRO] Tempo: begin falhou");
  }
  http.end();
  return ok;
}

void weatherNetworkTask(void *arg) {
  int writeIndex = (int)(intptr_t)arg;
  WeatherData local{};
  local.tempC = -100;
  const bool ok = weatherFetch(local);
  if (ok) {
    weatherShadows[writeIndex] = local; // buffer duplo: escreve no não exibido
    weatherActive.store(writeIndex);    // publica
    weatherReady.store(true);
    weatherVersion.fetch_add(1);
    lastWeatherGood = millis();
    snprintf(weatherStatus, sizeof(weatherStatus), "ATUALIZADO");
    Serial.printf("[M5RETRO] Tempo: %dC umidade:%d%% %s\n", local.tempC, local.humidity, local.cond);
  } else {
    snprintf(weatherStatus, sizeof(weatherStatus), "ERRO NA CONSULTA");
    Serial.println("[M5RETRO] Tempo: falha na consulta");
  }
  weatherBusy.store(false);
  vTaskDelete(nullptr);
}

void pollWeather() {
  if (state != WEATHER || weatherBusy.load())
    return;
  if (!network.connected()) {
    if (!weatherReady.load())
      snprintf(weatherStatus, sizeof(weatherStatus), "SEM WI-FI");
    return;
  }
  const uint32_t now = millis();
  if (weatherReady.load()) {
    if (now - lastWeatherGood < WEATHER_REFRESH_MS)
      return;
  } else if (lastWeatherAttempt && now - lastWeatherAttempt < WEATHER_RETRY_MS)
    return;
  lastWeatherAttempt = now;
  weatherBusy.store(true);
  int next = 1 - weatherActive.load();
  if (xTaskCreatePinnedToCore(weatherNetworkTask, "WEATHER_HTTP", 8192, (void *)(intptr_t)next, 0,
                              nullptr, 1) != pdPASS) {
    weatherBusy.store(false);
    snprintf(weatherStatus, sizeof(weatherStatus), "SEM MEMORIA");
  }
}

// Fundo oscilante estilo "Local Forecast": tons escuros variando suavemente entre
// azul, petróleo e roxo ao longo de um ciclo de ~12 s (quantizado em RGB565).
static uint16_t weatherBackground() {
  const float t = (millis() % 12000) / 12000.0f * 2.0f * PI;
  int r = (int)(2.0f + 2.0f * sin(t));
  int g = (int)(1.5f + 1.2f * sin(t + 2.09f));
  int b = (int)(3.0f + 1.0f * sin(t + 4.19f));
  uint16_t r5 = r * 31 / 4, g6 = g * 63 / 3, b5 = b * 31 / 4;
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void weatherTickerBuild() {
  if (!weatherReady.load()) {
    tickerPayload = "AGUARDANDO PREVISAO DE 3 DIAS...";
  } else {
    const WeatherData &w = weatherShadows[weatherActive.load()];
    tickerPayload = "";
    for (int i = 0; i < 3; ++i) {
      if (i)
        tickerPayload += "    ";
      tickerPayload += w.days[i].name;
      tickerPayload += " " + String(w.days[i].maxC) + "/" + String(w.days[i].minC) + "C";
    }
  }
  tickerFull = tickerPayload + "      " + tickerPayload;
  tickerWrapAt = weatherTicker.textWidth(tickerPayload.c_str()) + weatherTicker.textWidth("      ");
  tickerOffset = 0;
}

static void weatherTickerTick() {
  if (!weatherTicker.getBuffer())
    return;
  if (tickerWrapAt > 0 && tickerOffset >= tickerWrapAt)
    tickerOffset -= tickerWrapAt;
  weatherTicker.fillSprite(TFT_BLACK);
  weatherTicker.setTextDatum(top_left);
  weatherTicker.setTextColor(TFT_CYAN, TFT_BLACK);
  weatherTicker.drawString(tickerFull.c_str(), -tickerOffset, 0);
  weatherTicker.pushSprite(0, TICKER_Y);
}

void drawWeatherFrame() {
  const uint16_t bg = weatherBackground();
  lastWeatherBg = bg;
  rca.fillScreen(bg);
  rca.setFont(&fonts::Font4);
  rca.setTextDatum(top_center);
  rca.setTextSize(1);
  rca.setTextColor(TFT_YELLOW, bg);
  rca.drawString("FRANCA - SP", CRT_W / 2, 8);
  rca.drawFastHLine(8, 44, CRT_W - 16, TFT_CYAN);

  if (weatherReady.load()) {
    const WeatherData &w = weatherShadows[weatherActive.load()];
    rca.setTextColor(TFT_WHITE, bg);
    rca.drawString(w.cond, CRT_W / 2, 56);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d C", w.tempC);
    rca.setTextSize(2);
    rca.setTextColor(TFT_YELLOW, bg);
    rca.drawString(buf, CRT_W / 2, 108);
    rca.setFont(&fonts::Font2);
    rca.setTextSize(1);
    rca.setTextColor(TFT_WHITE, bg);
    snprintf(buf, sizeof(buf), "UMIDADE  %d%%", w.humidity);
    rca.drawString(buf, CRT_W / 2, 178);
    snprintf(buf, sizeof(buf), "VENTO  %d KM/H  %s", w.windKmph, w.windDir);
    rca.drawString(buf, CRT_W / 2, 198);
  } else {
    rca.setFont(&fonts::Font2);
    rca.setTextSize(1);
    rca.setTextColor(TFT_WHITE, bg);
    rca.drawString(weatherStatus, CRT_W / 2, 120);
  }
  rca.drawFastHLine(8, TICKER_Y - 2, CRT_W - 16, TFT_CYAN);
}

void weatherTick() {
  const uint32_t now = millis();
  static uint32_t lastVersion = UINT32_MAX;
  const uint32_t version = weatherVersion.load();
  if (version != lastVersion) {
    lastVersion = version;
    weatherTickerBuild();
    drawWeatherFrame();
  } else if (now - lastWeatherDraw >= 300) {
    lastWeatherDraw = now;
    const uint16_t bg = weatherBackground();
    if (bg != lastWeatherBg)
      drawWeatherFrame();
  }
  if (now - lastTickerMs >= 33) {
    lastTickerMs = now;
    tickerOffset += 2;
    weatherTickerTick();
  }
}

void startWeather() {
  playing = false;
  while (audioReady && !audioIdle && !audioFailed)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (mjpegFile)
    mjpegFile.close();
  if (wavFile)
    wavFile.close();
  mjpegReader.reset();
  wavFile = SD.open(WEATHER_MUSIC, FILE_READ);
  const bool ok = wavFile && openWavAndReadHeader();
  if (!ok && wavFile) {
    wavFile.close();
    snprintf(weatherStatus, sizeof(weatherStatus), "SEM MUSICA (TELA ATIVA)");
  }
  if (sdMutex)
    xSemaphoreGive(sdMutex);

  weatherAudio = ok; // música em loop apenas se o WAV abriu
  weatherReady = false;
  lastWeatherAttempt = 0; // força a primeira consulta imediata
  state = WEATHER;

  // Sprite do ticker (uma única vez).
  if (!weatherTicker.getBuffer()) {
    weatherTicker.setPsram(false);
    weatherTicker.setColorDepth(8);
    weatherTicker.createSprite(CRT_W, TICKER_H);
    weatherTicker.setFont(&fonts::Font2);
  }
  weatherTickerBuild();
  drawWeatherFrame();

  // No LCD do Core2, apenas uma placa simples (a experiência é na TV).
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.drawString(PTBR::WEATHER, 12, 8);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_NAVY);
  M5.Display.drawString("EXIBINDO NA TV", 12, 64);
  M5.Display.drawString("VOLTAR: BOTAO CENTRAL", 12, 88);
  drawBackButton();
}

void stopWeather() {
  weatherAudio = false;
  playing = false;
  while (audioReady && !audioIdle && !audioFailed)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (wavFile)
    wavFile.close();
  mjpegReader.reset();
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  state = HOME;
}

void drawSettings() {
  const char *names[] = {"VIDEO", "VOLUME", "ALCANCE RADAR", "ATUALIZACAO", PTBR::SAIDA_AUDIO,
                         PTBR::OSD_ESTILO_VHS, "WI-FI", "API", "CARTAO SD", "IDIOMA", "CONFIGURAR REDE"};
  String audio = settings.audioOutput == AudioOutput::RCA        ? PTBR::RCA
                 : settings.audioOutput == AudioOutput::INTERNAL ? PTBR::ALTO_FALANTE_INTERNO
                                                                 : PTBR::MUDO;
  String value = settingsSelection == 0   ? "PAL-M"
                 : settingsSelection == 1 ? String(settings.volume) + "%"
                 : settingsSelection == 2 ? String(settings.rangeKm) + " km"
                 : settingsSelection == 3 ? String(settings.refreshSeconds) + " s"
                 : settingsSelection == 4 ? audio
                 : settingsSelection == 5 ? (settings.vhsOsd ? PTBR::ATIVADO : PTBR::DESATIVADO)
                 : settingsSelection == 6
                     ? (WiFi.status() == WL_CONNECTED ? PTBR::CONECTADO : PTBR::DESCONECTADO)
                 : settingsSelection == 7  ? apiStatus
                 : settingsSelection == 8  ? PTBR::DISPONIVEL
                 : settingsSelection == 9  ? "PORTUGUES BR"
                                           : "ABRIR";
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.drawString(PTBR::CONFIGURACOES, 12, 8);
  M5.Display.drawFastHLine(8, 36, 304, TFT_CYAN);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_CYAN, TFT_NAVY);
  M5.Display.drawString(String(settingsEditing ? "> " : "  ") + names[settingsSelection], 20, 68);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.drawString(value, 34, 110);
  rca.fillScreen(TFT_NAVY);
  rca.setTextDatum(top_left);
  rca.setTextSize(2);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(PTBR::CONFIGURACOES, 12, 8);
  rca.drawFastHLine(8, 36, 304, TFT_CYAN);
  rca.setTextSize(1);
  rca.setTextColor(TFT_CYAN, TFT_NAVY);
  rca.drawString(String(settingsEditing ? "> " : "  ") + names[settingsSelection], 20, 74);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(value, 34, 105);
  drawControllerLabels(settingsEditing ? "-" : "ACIMA", settingsEditing ? "SALVAR" : "OK",
                       settingsEditing ? "+" : "ABAIXO");
}
void drawInfo() {
  String status = WiFi.status() == WL_CONNECTED ? PTBR::CONECTADO : PTBR::DESCONECTADO;
  String detalhe;
  if (infoPage) {
    detalhe = String("WI-FI: ") + status + "  " + PTBR::SINAL + ": " + WiFi.RSSI() + " dBm";
    if (WiFi.status() == WL_CONNECTED)
      detalhe += "  " + String(PTBR::ENDERECO_IP) + ": " + WiFi.localIP().toString();
  } else
    detalhe = String("MEMORIA: ") + ESP.getFreeHeap() + "  PSRAM: " + ESP.getFreePsram();
  dualText(PTBR::INFO_SISTEMA, detalhe);
  drawControllerLabels(PTBR::ANTERIOR, PTBR::DETALHES, PTBR::PROXIMO);
}

void handleNavigation(NavAction a) {
  if (a == NavAction::NONE)
    return;
  osdUntil = millis() + 3000;
  // Qualquer botão físico durante o playback religa o backlight, caso o comando
  // "diag backlight" o tenha desligado para inspeção.
  if (state == VIDEO_PLAYBACK)
    setBacklight(true);
  if (a == NavAction::PREVIOUS)
    a = NavAction::LEFT;
  if (a == NavAction::NEXT)
    a = NavAction::RIGHT;
  if (state == ERROR_SCREEN) {
    if (!bootReady) {
      if (a == NavAction::BACK || a == NavAction::HOME)
        ESP.restart(); // Retry initialization after a boot-time hardware/card error.
      return;
    }
    if (a == NavAction::BACK || a == NavAction::HOME) {
      state = HOME;
      drawHome();
    }
    return;
  }
  if (a == NavAction::HOME) {
    weatherAudio = false; // HOME também encerra a música do Weather Channel
    if (playing || wavFile)
      stopProgram();
    state = HOME;
    settingsEditing = false;
    drawHome();
    return;
  }
  if (state == HOME) {
    if (a == NavAction::LEFT)
      homeSelection = (homeSelection + 5) % 6;
    else if (a == NavAction::RIGHT)
      homeSelection = (homeSelection + 1) % 6;
    else if (a == NavAction::SELECT) {
      state = homeTarget(homeSelection);
      if (state == VIDEO_LIBRARY)
        drawLibrary();
      else if (state == MUSIC_BROWSER)
        drawMusicBrowser();
      else if (state == AIRCRAFT_RADAR) {
        lastApiPoll = 0; // consulta imediata ao entrar no radar
        drawRadar();
      } else if (state == SETTINGS)
        drawSettings();
      else if (state == WEATHER)
        startWeather();
      else
        drawInfo();
      return;
    }
    drawHome();
    return;
  }
  if (state == VIDEO_LIBRARY) {
    int count = libraryProgramCount();
    if (a == NavAction::BACK) {
      state = HOME;
      drawHome();
      return;
    }
    if (count && a == NavAction::LEFT)
      librarySelection = (librarySelection + count - 1) % count;
    else if (count && a == NavAction::RIGHT)
      librarySelection = (librarySelection + 1) % count;
    else if (a == NavAction::SELECT && count) {
      if (!startProgram(libraryProgramAt(librarySelection))) {
        setError(PTBR::VIDEO_CORROMPIDO);
        return;
      }
      // startProgram already changed to VIDEO_PLAYBACK and drew its controller.
      // Do not redraw the library over the player after a successful PLAY.
      return;
    }
    drawLibrary();
    return;
  }
  if (state == VIDEO_PLAYBACK) {
    if (a == NavAction::SELECT || a == NavAction::PLAY_PAUSE) {
      paused = !paused;
      osdUntil = millis() + 3000;
      drawPlaybackOsd();
    } else if (a == NavAction::LEFT || a == NavAction::PREVIOUS || a == NavAction::RIGHT ||
               a == NavAction::NEXT) {
      stopProgram();
      int count = libraryProgramCount();
      if (count) {
        int delta = (a == NavAction::LEFT || a == NavAction::PREVIOUS) ? count - 1 : 1;
        int wanted = (librarySelection + delta) % count;
        librarySelection = wanted;
        if (!startProgram(libraryProgramAt(librarySelection)))
          setError(PTBR::VIDEO_CORROMPIDO);
        else
          drawPlaybackOsd();
      } else
        drawLibrary();
    } else if (a == NavAction::BACK) {
      stopProgram();
      drawLibrary();
    }
    return;
  }
  if (state == AIRCRAFT_RADAR) {
    if (a == NavAction::BACK) {
      state = HOME;
      drawHome();
      return;
    }
    if ((a == NavAction::LEFT || a == NavAction::RIGHT) && aircraftCount)
      radarSelection =
          radarSelection < 0
              ? 0
              : (radarSelection + (a == NavAction::LEFT ? aircraftCount - 1 : 1)) % aircraftCount;
    else if (a == NavAction::SELECT)
      radarDetails = !radarDetails;
    drawRadar();
    drawControllerLabels(PTBR::AERONAVE, PTBR::DETALHES, PTBR::AERONAVE);
    return;
  }
  if (state == SETTINGS) {
    constexpr int SETTINGS_COUNT = 11;
    if (a == NavAction::BACK) {
      if (settingsEditing)
        settingsEditing = false;
      else
        state = HOME;
      if (state == HOME)
        drawHome();
      else
        drawSettings();
      return;
    }
    if (a == NavAction::SELECT) {
      if (!settingsEditing && settingsSelection == 10) {
        startSetupPortal();
        return;
      }
      if (settingsSelection == 0 || (settingsSelection >= 6 && settingsSelection <= 9))
        return;
      settingsEditing = !settingsEditing;
      if (!settingsEditing) {
        saveSettings();
        if (state == ERROR_SCREEN)
          return;
      }
    } else if (!settingsEditing && (a == NavAction::LEFT || a == NavAction::RIGHT))
      settingsSelection =
          (settingsSelection + (a == NavAction::LEFT ? SETTINGS_COUNT - 1 : 1)) % SETTINGS_COUNT;
    else if (settingsEditing && (a == NavAction::LEFT || a == NavAction::RIGHT)) {
      int d = a == NavAction::LEFT ? -1 : 1;
      if (settingsSelection == 1)
        settings.volume = constrain(settings.volume + d * 5, 0, 100);
      else if (settingsSelection == 2) {
        int v[] = {50, 100, 250, 500}, i = 0;
        while (i < 3 && v[i] != settings.rangeKm)
          i++;
        settings.rangeKm = v[(i + d + 4) % 4];
      } else if (settingsSelection == 3) {
        int v[] = {5, 10, 30}, i = 0;
        while (i < 2 && v[i] != settings.refreshSeconds)
          i++;
        settings.refreshSeconds = v[(i + d + 3) % 3];
      } else if (settingsSelection == 4) {
        AudioOutput next = settings.audioOutput == AudioOutput::RCA        ? AudioOutput::INTERNAL
                           : settings.audioOutput == AudioOutput::INTERNAL ? AudioOutput::MUTED
                                                                           : AudioOutput::RCA;
        setAudioOutput(next);
        if (state == ERROR_SCREEN)
          return;
      } else if (settingsSelection == 5)
        settings.vhsOsd = !settings.vhsOsd;
    }
    drawSettings();
    return;
  }
  if (state == WEATHER) {
    if (a == NavAction::BACK || a == NavAction::HOME || a == NavAction::SELECT)
      stopWeather(), drawHome();
    return;
  }
  if (state == MUSIC_BROWSER) {
    if (a == NavAction::BACK) {
      if (musicDir != String(MUSIC_ROOT)) {
        musicDir = musicParentDir(musicDir);
        musicScanDir();
        drawMusicBrowser();
      } else {
        state = HOME;
        drawHome();
      }
      return;
    }
    if ((a == NavAction::LEFT || a == NavAction::RIGHT) && musicEntryCount) {
      musicSelection =
          (musicSelection + (a == NavAction::LEFT ? musicEntryCount - 1 : 1)) % musicEntryCount;
      drawMusicBrowser();
      return;
    }
    if (a == NavAction::SELECT && musicEntryCount) {
      MusicEntry &e = musicEntries[musicSelection];
      if (e.isFolder) {
        musicDir = e.path;
        musicScanDir();
        drawMusicBrowser();
      } else if (startMusic(e.path)) {
        state = MUSIC_NOW_PLAYING; // startMusic já desenhou o now playing
      }
      return;
    }
    return;
  }
  if (state == MUSIC_NOW_PLAYING) {
    if (a == NavAction::BACK) {
      stopMusic();
      state = MUSIC_BROWSER;
      drawMusicBrowser();
      return;
    }
    if (a == NavAction::SELECT || a == NavAction::PLAY_PAUSE) {
      paused = !paused;
      drawMusicNowPlaying();
      return;
    }
    if (a == NavAction::LEFT || a == NavAction::PREVIOUS || a == NavAction::RIGHT || a == NavAction::NEXT) {
      if (musicQueueCount) {
        const int delta = (a == NavAction::LEFT || a == NavAction::PREVIOUS) ? -1 : 1;
        musicQueueIndex = (musicQueueIndex + delta + musicQueueCount) % musicQueueCount;
        if (!startMusic(musicQueue[musicQueueIndex])) {
          stopMusic();
          state = MUSIC_BROWSER;
          drawMusicBrowser();
        }
      }
      return;
    }
    return;
  }
  if (state == SYSTEM_INFO) {
    if (a == NavAction::BACK) {
      state = HOME;
      drawHome();
    } else if (a == NavAction::LEFT || a == NavAction::RIGHT || a == NavAction::SELECT) {
      infoPage = 1 - infoPage;
      drawInfo();
    }
    return;
  }
}
void togglePlayerAudio() {
  if (state != VIDEO_PLAYBACK)
    return;
  setAudioOutput(audioOutput == AudioOutput::INTERNAL ? AudioOutput::RCA : AudioOutput::INTERNAL);
  if (state == VIDEO_PLAYBACK)
    drawPlaybackController();
}
void handleTouch() {
  static bool down = false;
  static int8_t button = -1;
  static uint32_t downAt = 0;
  const uint32_t now = millis();
  if (!M5.Touch.getCount()) {
    if (down && button >= 0) {
      uint32_t held = now - downAt;
      NavAction action = (button == 0               ? NavAction::LEFT
                          : button == 2             ? NavAction::RIGHT
                          : held >= 1500            ? NavAction::HOME
                          : held >= 700             ? NavAction::BACK
                          : state == VIDEO_PLAYBACK ? NavAction::PLAY_PAUSE
                                                    : NavAction::SELECT);
      input.inject(action, InputSource::LCD_BUTTON);
    }
    down = false;
    button = -1;
    return;
  }
  const auto &p = M5.Touch.getDetail();
  if (!p.isPressed())
    return;
  // Um toque físico durante o playback também religa o backlight (idem à
  // navegação por botões), caso "diag backlight" o tenha desligado.
  if (state == VIDEO_PLAYBACK)
    setBacklight(true);
  if (!down) {
    down = true;
    downAt = now;
    if (p.y >= 240) {
      down = false;
      return;
    }
    if (state != HOME && isBackButton(p.x, p.y)) {
      button = -1;
      input.inject(NavAction::BACK, InputSource::LCD_BUTTON);
      return;
    }
    if (state == VIDEO_PLAYBACK && isPlayerAudioButton(p.x, p.y)) {
      button = -1;
      togglePlayerAudio();
      return;
    }
    if (p.y >= 184)
      button = touchButton(p.x, p.y);
    else {
      button = -1;
      if (state == HOME && p.y >= 40 && p.y < 172) {
        homeSelection = (p.y - 40) / 22;
        input.inject(NavAction::SELECT, InputSource::LCD_BUTTON);
      } else if (state == VIDEO_PLAYBACK)
        input.inject(NavAction::PLAY_PAUSE, InputSource::LCD_BUTTON);
    }
  }
}

// Local USB diagnostics. Commands never print credentials or persist test settings.
void diagnosticStatus() {
  JsonDocument d;
  d["build"] = "core2-2026-09-20";
  d["uptime_ms"] = millis();
  d["state"] = int(state);
  d["ready"] = bootReady;
  d["playing"] = playing.load();
  d["paused"] = paused.load();
  d["audio_ready"] = audioReady.load();
  d["audio_failed"] = audioFailed.load();
  d["audio_stream_error"] = audioStreamError.load();
  d["video_error"] = videoReadError;
  d["samples"] = samplesPlayed.load();
  d["sample_rate"] = sampleRate;
  d["duration_ms"] = sampleRate ? uint64_t(wavDataEnd - wavDataStart) * 1000 / (sampleRate * 4) : 0;
  d["frame"] = videoFrameIndex;
  d["fps"] = fps;
  d["width"] = videoWidth;
  d["height"] = videoHeight;
  d["jpeg_errors"] = jpegErrors;
  d["dropped"] = droppedFrames;
  d["underruns"] = audioUnderruns.load();
  d["heap"] = ESP.getFreeHeap();
  d["min_heap"] = ESP.getMinFreeHeap();
  d["psram"] = ESP.getFreePsram();
  d["audio_output"] = int(audioOutput.load());
  d["volume"] = playbackVolume.load();
  d["title"] = currentTitle;
  d["error"] = lastError;
  d["wifi_connected"] = network.connected();
  d["ip"] = network.connected() ? WiFi.localIP().toString() : "";
  d["api_status"] = apiStatus;
  d["aircraft_count"] = aircraftCount;
  d["clock_ready"] = time(nullptr) >= 1704067200;
  d["api_http_code"] = lastHttpCode;
  d["api_ms"] = lastHttpMs;
  d["api_busy"] = radarBusy;
  String payload;
  serializeJson(d, payload);
  String message = "[DIAG] " + payload;
  message += '\n';
  Serial.write(reinterpret_cast<const uint8_t *>(message.c_str()), message.length());
}

void provisionDevice(const String &payload) {
  if (playing || portal.active()) {
    Serial.println("[DIAG] ERROR stop playback before provisioning");
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, payload)) {
    Serial.println("[DIAG] ERROR invalid provisioning JSON");
    return;
  }
  SecretsConfig cfg = currentSecrets();
  cfg.ssid = d["wifi_ssid"] | cfg.ssid;
  cfg.password = d["wifi_password"] | cfg.password;
  cfg.baseUrl = d["api_base_url"] | cfg.baseUrl;
  cfg.endpoint = d["aircraft_endpoint"] | cfg.endpoint;
  cfg.token = d["api_token"] | cfg.token;
  cfg.authMode = "bearer";
  cfg.authHeader = "Authorization";
  cfg.authPrefix = "Bearer ";
  cfg.allowInsecureTls = false;
  const String ca = d["ca_pem"] | "";
  if (!cfg.valid() || cfg.ssid.length() > 32 || !cfg.baseUrl.startsWith("https://") ||
      !cfg.endpoint.startsWith("/") || (ca.length() && !ca.startsWith("-----BEGIN CERTIFICATE-----"))) {
    Serial.println("[DIAG] ERROR invalid network/API configuration");
    return;
  }
  bool saved = ca.length() ? storage::replace(SD, String(CA_FILE), ca) : true;
  saved = saved && secretsStore.save(cfg, currentSettings());
  if (!saved) {
    Serial.println("[DIAG] ERROR configuration write failed");
    return;
  }
  network.disconnect();
  loadConfiguration();
  state = HOME;
  drawHome();
  Serial.println("[DIAG] Provisioning saved; secrets redacted");
}
void testPixelColors() {
  M5Canvas sample;
  sample.setColorDepth(16);
  sample.createSprite(3, 1);
  uint16_t values[] = {0xf800, 0x07e0, 0x001f};
  sample.pushImage(0, 0, 3, 1, reinterpret_cast<const lgfx::rgb565_t *>(values));
  bool correct = true;
  for (int i = 0; i < 3; ++i)
    correct = correct && sample.readPixel(i, 0) == values[i];
  sample.deleteSprite();
  Serial.println(correct ? "[DIAG] RGB565 colors PASS" : "[DIAG] RGB565 colors FAIL");
}
void serviceDiagnostics() {
  static char line[4096];
  static size_t used = 0;
  static bool overflow = false;
  // Limit parsing work per loop so a noisy serial sender cannot monopolize playback.
  for (int read = 0; read < 80 && Serial.available(); ++read) {
    const char c = Serial.read();
    if (c == '\r')
      continue;
    if (c != '\n') {
      if (used < sizeof(line) - 1)
        line[used++] = c;
      else
        overflow = true;
      continue;
    }
    line[used] = 0;
    String command = line;
    used = 0;
    if (overflow) {
      overflow = false;
      Serial.println("[DIAG] ERROR command too long");
      continue;
    }
    command.trim();
    if (command == "diag status") {
      diagnosticStatus();
      continue;
    }
    if (!bootReady || audioFailed || portal.active()) {
      Serial.println("[DIAG] ERROR device not ready");
      continue;
    }
    if (command.startsWith("provision ")) {
      provisionDevice(command.substring(10));
      memset(line, 0, sizeof(line));
      continue;
    }
    if (command == "diag colors") {
      testPixelColors();
      continue;
    }
    if (command == "diag backlight") {
      // Alterna o backlight (ligado <-> desligado) para inspeção. O próximo
      // botão/toque físico durante o playback religa o backlight sozinho.
      setBacklight(!backlightOn);
    } else if (command == "diag audio toggle") {
      togglePlayerAudio();
    } else if (command == "diag radar") {
      stopProgram();
      state = AIRCRAFT_RADAR;
      lastApiPoll = 0;
      pollAircraft();
      drawRadar();
    } else if (command == "diag weather") {
      stopProgram();
      startWeather();
    } else if (command == "diag music") {
      weatherAudio = false; // para a música do weather (se ativa) antes de stopProgram
      stopProgram();
      musicDir = MUSIC_ROOT;
      musicScanDir();
      state = MUSIC_BROWSER;
      drawMusicBrowser();
    } else if (command == "diag play") {
      stopProgram();
      librarySelection = 0;
      String path = libraryProgramAt(0);
      if (!startProgram(path))
        setError(PTBR::VIDEO_CORROMPIDO);
    } else if (command == "diag pause" && state == VIDEO_PLAYBACK)
      paused = true;
    else if (command == "diag resume" && state == VIDEO_PLAYBACK)
      paused = false;
    else if (command == "diag stop") {
      stopProgram();
      drawLibrary();
    } else if (command == "diag home")
      handleNavigation(NavAction::HOME);
    else if (command == "diag next")
      handleNavigation(NavAction::NEXT);
    else if (command == "diag previous")
      handleNavigation(NavAction::PREVIOUS);
    else if (command == "diag select")
      handleNavigation(NavAction::SELECT);
    else if (command == "diag back")
      handleNavigation(NavAction::BACK);
    else if (command == "diag left")
      handleNavigation(NavAction::LEFT);
    else if (command == "diag right")
      handleNavigation(NavAction::RIGHT);
    else if (command == "diag audio rca")
      audioOutput = AudioOutput::RCA;
    else if (command == "diag audio internal")
      audioOutput = AudioOutput::INTERNAL;
    else if (command == "diag audio mute")
      audioOutput = AudioOutput::MUTED;
    else {
      Serial.println("[DIAG] ERROR unknown command");
      continue;
    }
    osdUntil = millis() + 3000;
    diagnosticStatus();
  }
}

void *tlsCalloc(size_t count, size_t size) {
  // Keep certificate and TLS record buffers out of the DMA-capable SRAM used
  // by continuous CVBS. Core2 PSRAM is suitable for these CPU-only buffers.
  void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return memory ? memory : heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  auto cfg = M5.config();
  // Let M5Unified configure Core2 pins and its amplifier callback. End the
  // speaker before installing RCA; the audio task owns I2S1 from then on.
  cfg.internal_spk = true; // Configure Core2 pins and amplifier callback; playback starts later.
  cfg.external_spk = false;
  M5.begin(cfg);
  if (psramFound())
    mbedtls_platform_set_calloc_free(tlsCalloc, heap_caps_free);
  if (M5.Speaker.isRunning())
    M5.Speaker.end();
  auto speakerConfig = M5.Speaker.config();
  speakerConfig.i2s_port = I2S_NUM_1; // I2S0 belongs to CVBS.
  speakerConfig.sample_rate = 22050;
  M5.Speaker.config(speakerConfig);
  M5.Display.setRotation(1);
  input.begin();
  input.setAutoRepeat(true);
  uiHudInit(); // sprite do HUD criado uma única vez, fora do hot path
  jpegBuffer = (uint8_t *)ps_malloc(MAX_JPEG);
  if (!rca.init()) {
    M5.Display.println("FALHA PAL-M");
    state = ERROR_SCREEN;
    drawBackButton();
    return;
  }
  rca.setOutputBoost(true);
  dualText(PTBR::APP, PTBR::INICIANDO);
  if (!jpegBuffer) {
    setError(PTBR::MEMORIA_INSUFICIENTE);
    return;
  }
  dualText(PTBR::APP, PTBR::VERIFICANDO_SD);

  // Core2 TF card wiring: SCK=18, MISO=38, MOSI=23, CS=4. GPIO19 is
  // deliberately not used by SPI: the stacked M125 RCA module owns it for PCM BCK.
  // Explicit setup avoids GPIO19, which is reserved for RCA audio.
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  // Try the normal 25 MHz rate first, then the conservative 4 MHz rate used by
  // marginal/older FAT32 cards before reporting a real card error.
  bool sdMounted = SD.begin(SD_CS, SPI, 25000000);
  if (!sdMounted) {
    SD.end();
    delay(30);
    sdMounted = SD.begin(SD_CS, SPI, 4000000);
  }
  if (!sdMounted) {
    setError(PTBR::CARTAO_SD_NAO_ENCONTRADO);
    return;
  }
  Serial.println("[M5RETRO] SD mounted on Core2 TF bus (CS4 SCK18 MISO38 MOSI23)");
  sdMutex = xSemaphoreCreateMutex();
  if (!sdMutex) {
    setError(PTBR::ERRO_SD);
    return;
  }
  if (!makeDirectories()) {
    setError(PTBR::ERRO_SD);
    return;
  }
  // Always inspect the card at boot. This keeps the video library usable when
  // Wi-Fi has not been configured and leaves serial evidence of the detected path.
  libraryProgramCount();
  dualText(PTBR::APP, PTBR::CARREGANDO_CONFIG);
  if (!loadConfiguration()) {
    apiStatus = PTBR::CONFIG_REDE_AUSENTE;
    dualText(PTBR::CONFIG_REDE_AUSENTE, PTBR::EDITE_SECRETS);
  } else {
    dualText(PTBR::APP, PTBR::CONECTANDO_WIFI);
    // NetworkManager retries without blocking the UI.
  }
  dualText(PTBR::APP, PTBR::INICIANDO_PALM);
  if (!initExternalAudio()) {
    setError(PTBR::FALHA_AUDIO);
    return;
  }
  audioOutput = settings.audioOutput;
  if (xTaskCreatePinnedToCore(audioTask, "RCA_PCM", 4096, nullptr, 4, &audioTaskHandle, 0) != pdPASS) {
    setError(PTBR::FALHA_AUDIO);
    return;
  }
  while (!audioReady && !audioFailed)
    delay(1);
  if (audioFailed) {
    setError(PTBR::MEMORIA_INSUFICIENTE);
    return;
  }
  dualText(PTBR::APP, PTBR::SISTEMA_PRONTO);
  // Offline playback must not be hidden behind network setup. The portal stays
  // available from CONFIGURACOES when the owner wants to add Wi-Fi later.
  bootReady = true;
  state = HOME;
  drawHome();
}
void loop() {
  M5.update();
  servicePowerButton();
  if (powerOffPending)
    return;
  serviceDiagnostics();
  input.setAutoRepeat(state != VIDEO_PLAYBACK);
  input.update();
  if (audioFailed && state != ERROR_SCREEN) {
    setError(PTBR::FALHA_AUDIO);
    return;
  }
  if (state == SETUP_PORTAL) {
    portal.update();
    if (!portal.active()) {
      network.begin(currentSecrets());
      portalSavePending = false;
      state = HOME;
      drawHome();
      return;
    }
    handleTouch();
    NavAction portalAction = input.getAction();
    if (portalAction == NavAction::BACK || portalAction == NavAction::LEFT ||
        portalAction == NavAction::RIGHT || portalAction == NavAction::HOME) {
      portalSavePending = false;
      portal.stop();
      network.begin(currentSecrets());
      state = HOME;
      drawHome();
    } else if (portalAction != NavAction::NONE) {
      drawSetupPortal();
    }
    if (portalSavePending) {
      if (WiFi.status() == WL_CONNECTED) {
        portalSavePending = false;
        portal.stop();
        network.begin(currentSecrets());
        apiStatus = "CONECTADA";
        dualText(PTBR::WIFI_CONECTADO, WiFi.localIP().toString());
        state = HOME;
        drawHome();
      } else if (millis() - portalSaveStarted >= 10000) {
        portalSavePending = false;
        apiStatus = "SEM WI-FI";
        drawSetupPortal();
      }
    }
  } else {
    serviceWiFi();
    handleTouch();
    handleNavigation(input.getAction());
  }
  if (state == VIDEO_PLAYBACK) {
    videoTick();
    drawPlaybackOsd();
    uiHudTick(millis()); // HUD a 1 Hz; a leitura do SD nunca espera este desenho
    if (playbackFinished) {
      playbackFinished = false;
      stopProgram();
      if (audioStreamError)
        setError(PTBR::AUDIO_INVALIDO);
      else if (videoReadError)
        setError(PTBR::VIDEO_CORROMPIDO);
      else
        drawLibrary();
      Serial.println("[M5RETRO] PLAY encerrado no fim do programa");
    }
  }
  if (state == WEATHER) {
    pollWeather();
    weatherTick();
  }
  if (state == MUSIC_NOW_PLAYING) {
    musicTick();
  }
  pollAircraft();
  if (state == AIRCRAFT_RADAR && radarDirty) {
    radarDirty = false;
    drawRadar();
  }
  if (millis() - lastStats >= 1000) {
    lastStats = millis();
    uint32_t avg = decodedFrames ? jpegDecodeTotalMs / decodedFrames : 0;
    Serial.printf(
        "[M5RETRO] FPS RCA:%lu FPS JPEG:%lu frames descartados:%lu JPEG medio:%lu JPEG "
        "maximo:%lu underruns audio:%lu heap livre:%u heap minimo:%u PSRAM livre:%u RSSI:%d API:%s\n",
        renderedFrames, decodedFrames, droppedFrames, avg, jpegDecodeMaxMs,
        audioUnderruns.load(), ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getFreePsram(), WiFi.RSSI(),
        apiStatus.c_str());
    renderedFrames = decodedFrames = 0;
    jpegDecodeTotalMs = jpegDecodeMaxMs = 0;
  }
  delay(4);
}