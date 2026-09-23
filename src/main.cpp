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

#include "SafeArea.h"
#include "VcrOsd.h"
#include "WeatherIcons.h"
#include "ScreenFx.h"
#include "FileTransfer.h"
#include "TransferScreen.h"
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
void requestPowerOff();
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
void startTransfer();
void stopTransfer();
void transferTick();
void drawTransferFrame();
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
// Geometria da saída composta: quadro, área segura do tubo e faixas padrão.
// Definições em include/SafeArea.h, compartilhadas com o simulador de telas.
static constexpr int CRT_W = crt::W, CRT_H = crt::H;
static constexpr int SAFE_L = crt::SAFE_L, SAFE_T = crt::SAFE_T;
static constexpr int SAFE_R = crt::SAFE_R, SAFE_B = crt::SAFE_B;
static constexpr int SAFE_W = crt::SAFE_W;
static constexpr int HEAD_Y = crt::HEAD_Y, HEAD_RULE_Y = crt::HEAD_RULE_Y;
static constexpr int BODY_Y = crt::BODY_Y;
static constexpr int BAR_H = crt::BAR_H, BAR_Y = crt::BAR_Y;
// Acento seguro para NTSC.
//
// O ciano puro (0x07FF) tem croma máximo e, numa saída composta, produz dot
// crawl: o padrão que rasteja devagar pela tela. Foi medido no aparelho e o
// sintoma era seletivo de um jeito que só isso explica — glitchavam a régua, o
// rodapé, o letreiro e, no menu inicial, EXATAMENTE o item selecionado, que é o
// único em ciano. O texto branco, de croma zero, nunca glitchou.
//
// Levantar o vermelho aproxima a cor do branco e derruba a amplitude de croma,
// preservando a luminância e a leitura de "azul claro". Regra de ouro de
// gráficos para tubo: contraste por luminância, não por saturação.
static constexpr uint16_t RCA_ACCENT = 0x96BC;
// Itens do menu inicial. Uma constante só: já houve divergência entre o
// desenho, a navegação por botão e o mapeamento de toque, e o resultado foi o
// último item (DESLIGAR) ficar inalcançável.
static constexpr int HOME_COUNT = 8;
static constexpr int HOME_POWER_OFF = HOME_COUNT - 1;
// Passo dos itens no LCD. Com 8 itens, 20 px faria o último cair sobre a faixa
// de botões que começa em 184 e o toque em DESLIGAR teria 4 px úteis.
static constexpr int HOME_LCD_Y0 = 40, HOME_LCD_STEP = 18;

// Transferência de arquivos por Wi-Fi (include/FileTransfer.h + TransferScreen.h).
xfer::FileTransfer fileTransfer;
static uint32_t lastTransferDraw = 0;
// Relógio do menu inicial. Redesenhado a cada segundo, só na sua faixa — a tela
// inteira não é repintada, senão a TV piscaria uma vez por segundo.
static uint32_t lastClockDraw = 0;
static constexpr int CLOCK_Y = crt::HEAD_RULE_Y + 4; // 50..66, acima dos itens
static xfer::Stage lastTransferStage = xfer::Stage::Error; // força o 1º desenho
// Faixa reservada no LCD do Core2 para o HUD (tempo + progresso) redesenhado a
// 1 Hz. O vídeo nunca toca o LCD: atrás desta faixa fica apenas o pôster.
static constexpr int HUD_W = 192, HUD_H = 16, HUD_Y = 204;
// Weather Channel: faixa do ticker e cadências de consulta.
static constexpr int TICKER_H = crt::TICKER_H, TICKER_Y = crt::TICKER_Y;
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

// NTSC (525/59,94 Hz, preto em 7,5 IRE). O modo PAL_M do M5GFX monta a linha
// com 908 amostras, mas 4x3,57561149 MHz x 63,5556 us dá 909,02 — a linha sai
// ~0,11% curta e a fase da burst anda a cada linha, o que produz a faixa de cor
// diagonal que caminha pela tela. A tabela NTSC usa 910 amostras, que é o valor
// exato para 4x3,579545 MHz, então a burst fica estável.
M5ModuleRCA rca(CRT_W, CRT_H, CRT_W, CRT_H, M5ModuleRCA::signal_type_t::NTSC,
                M5ModuleRCA::use_psram_t::psram_half_use, CVBS_PIN, 200);
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
// Instrumentação do "diag bench". Fora do benchmark benchActive é falso e o
// jpegDraw não paga nada além de um teste de bool por bloco de MCU.
bool benchActive = false;
// Tamanho do último JPEG decodificado, que continua inteiro no jpegBuffer. Com o
// vídeo pausado é daí que o OSD restaura a imagem que fica atrás dele.
size_t lastJpegUsed = 0;
// Sequência monotônica de quadros efetivamente desenhados. O videoFrameIndex não
// serve para o OSD: ele avança também nos quadros descartados sem render, então
// o OSD acreditava ter imagem nova embaixo e pintava contador sobre contador.
uint32_t renderedSeq = 0;
// true enquanto o vídeo está atrasado em relação ao relógio de PCM. Só nesse
// caso o loop encurta a soneca: dormir menos sempre quadruplicaria o polling
// I2C do touch feito por M5.update() sem ganho nenhum.
bool videoBehind = false;
uint64_t benchBlitUs = 0;
uint32_t benchBlitCalls = 0;
LGFX_Sprite uiHud(&M5.Display); // sprite minúsculo do HUD (tempo + progresso); pai = LCD
uint32_t lastUiUpdate = 0;      // borda de 1 s que dispara o redesenho do HUD
static bool backlightOn = true; // estado atual do backlight (DCDC3 do AXP192)
std::atomic<bool> weatherAudio{false}; // música do Weather Channel em loop (I2S1), flag de áudio em loop

// --- Weather Channel (Local Forecast) ---
struct WeatherDay {
  char name[8]; // "DOM".."SAB"
  int maxC = 0, minC = 0;
  // Código WMO do dia, guardado além do texto: o ícone é escolhido pelo código
  // (ver include/WeatherIcons.h), não por comparação de string.
  int wmoCode = -1;
};
struct WeatherData {
  int tempC = -100, humidity = 0, windKmph = 0;
  int wmoCode = -1;
  char cond[24] = {0}, windDir[8] = {0};
  WeatherDay days[3];
};
WeatherData weatherShadows[2];
std::atomic<int> weatherActive{0};
std::atomic<uint32_t> weatherVersion{0};
std::atomic<bool> weatherReady{false};
std::atomic<bool> weatherBusy{false};
std::atomic<const char *> weatherStatus{"CARREGANDO..."};
uint32_t lastWeatherAttempt = 0, lastWeatherGood = 0;
uint32_t lastTickerMs = 0;
uint16_t lastWeatherBg = 0;
// (o letreiro não usa mais sprite: ver weatherDrawTicker)
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
int musicBitrateKbps = 0;       // bitrate real do 1º frame (MP3); 0 = WAV/desconhecido
bool musicShuffle = false, musicRepeat = false; // modos de reprodução

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
  M5.Display.drawRoundRect(280, 4, 36, 30, 4, RCA_ACCENT);
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
  // Trunca as duas linhas para caber nas fontes bitmap (size 2 ~15 chars, size 1
  // ~40 chars na fonte Courier, 320px), evitando texto vazando do LCD/RCA.
  const String l1 = line1.length() > 17 ? line1.substring(0, 17) : line1;
  const String l2 = line2.length() > 44 ? line2.substring(0, 41) + "..." : line2;
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(middle_center);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->setTextSize(2);
    d->drawString(l1, 160, 94);
    d->setTextColor(RCA_ACCENT, TFT_NAVY);
    d->setTextSize(1);
    d->drawString(l2, 160, 130);
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
  if (!networkConfigPresent)
    return;
  // NTP é único e barato: solicita mesmo durante o playback. Senão, se o usuário
  // nunca parar de tocar, o relógio não sincroniza e o radar fica preso em
  // "SINCRONIZANDO HORA".
  if (network.connected()) {
    static bool clockRequested = false;
    if (!clockRequested) {
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      clockRequested = true;
    }
  }
  // Retry/roaming e texto de status ficam suspensos durante o playback, para não
  // disputar a banda com o streaming de vídeo.
  if (playing)
    return;
  const NetworkState before = network.state();
  network.update();
  if (network.connected()) {
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
    rca.drawString("SENHA: " + portal.apPassword(), SAFE_L, 144);
    rca.drawString("ABRA: 192.168.4.1", SAFE_L, 162);
    rca.drawString(portal.status() == PortalStatus::CONFIGURANDO ? "CONFIGURACAO ATIVA"
                                                                 : "AGUARDANDO CELULAR...",
                   SAFE_L, 180);
    M5.Display.fillScreen(TFT_NAVY);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextSize(2);
    M5.Display.drawString("M5 RETRO TV", 12, 8);
    M5.Display.drawFastHLine(8, 36, 304, RCA_ACCENT);
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
    // `!wavFile && !mp3Mode` (e não apenas `!wavFile`) porque, no MP3, o arquivo
    // aberto é `mp3File` (wavFile fica nulo) — senão o MP3 nunca tocaria.
    if ((!playing && !weatherAudio.load()) || paused || (!wavFile && !mp3Mode)) {
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
  const int64_t t0 = benchActive ? esp_timer_get_time() : 0;
  rca.pushImage(draw->x + (CRT_W - videoWidth) / 2, draw->y + (CRT_H - videoHeight) / 2, draw->iWidth,
                draw->iHeight, reinterpret_cast<const lgfx::rgb565_t *>(draw->pPixels));
  if (benchActive) {
    benchBlitUs += uint64_t(esp_timer_get_time() - t0);
    ++benchBlitCalls;
  }
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
  // Só agora o par (jpegBuffer, tamanho) está comprovadamente coerente, que é a
  // pré-condição do redrawCurrentFrame().
  lastJpegUsed = used;
  decodedFrames++;
  renderedFrames++;
  ++renderedSeq;
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

// ============================================================================
// Benchmark do caminho de vídeo da RCA ("diag bench")
//
// Responde na bancada a pergunta que nenhum simulador de PC responde: até que
// resolução e taxa ESTE aparelho sustenta na saída composta. Mede as três
// etapas separadamente, em microssegundos (millis() tem resolução grossa demais
// para um quadro de ~20 ms), rodando o mais rápido possível, sem cadência de
// áudio e sem descarte de quadros:
//
//   leitura   -> tirar o JPEG do cartão (mjpegReader.next)
//   decode    -> JPEGDEC, já descontado o blit
//   blit      -> pushImage no framebuffer CVBS, cronometrado dentro do jpegDraw
//
// O teto real não é do aparelho e sim do sinal: o NTSC entrega 59,94 campos por
// segundo e a luminância tem ~4,2 MHz de banda, o que equivale a ~330 pontos por
// linha. Acima de 320x240 a 30 quadros/s não há qualidade a ganhar no tubo, só
// trabalho a mais. Este benchmark serve para saber se o Core2 alcança esse teto
// com a mídia que você preparou.
// ============================================================================
static void runVideoBenchmark(const String &dir, uint32_t maxFrames) {
  stopProgram(); // nada de áudio ou reprodução competindo pelo cartão

  String video = "video.mjpeg";
  float metaFps = 15.0f;
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  File meta = SD.open(dir + "/meta.json", FILE_READ);
  if (meta) {
    JsonDocument d;
    if (!deserializeJson(d, meta)) {
      video = (const char *)(d["video"] | "video.mjpeg");
      metaFps = d["fps"] | 15.0f;
    }
    meta.close();
  }

  File f = SD.open(dir + "/" + video, FILE_READ);
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  if (!f) {
    Serial.printf("[BENCH] nao abriu %s/%s\n", dir.c_str(), video.c_str());
    return;
  }
  if (!jpegBuffer) {
    Serial.println("[BENCH] sem buffer JPEG");
    if (sdMutex)
      xSemaphoreTake(sdMutex, portMAX_DELAY);
    f.close();
    if (sdMutex)
      xSemaphoreGive(sdMutex);
    return;
  }

  // Reaproveita o leitor global: um MjpegReader local são 4104 bytes, e esta
  // função roda na pilha do loopTask (8 KB), que ainda precisa de FATFS,
  // deserializeJson e dos printf com float do relatório. O stopProgram() acima
  // já resetou o leitor.
  playback::MjpegReader &reader = mjpegReader;
  reader.reset();
  rca.fillScreen(TFT_BLACK);
  videoFrameIndex = 0;

  uint64_t readUs = 0, decodeUs = 0, totalBytes = 0;
  uint32_t maxReadUs = 0, maxDecodeUs = 0, maxTotalUs = 0;
  uint32_t frames = 0, errors = 0;
  int w = 0, h = 0;

  benchActive = true;
  benchBlitUs = 0;
  benchBlitCalls = 0;
  const int64_t wallStart = esp_timer_get_time();

  uint32_t iterations = 0;
  while (frames < maxFrames) {
    // O cão de guarda precisa respirar mesmo quando todo quadro dá erro: contar
    // sucessos deixava os caminhos de falha girando sem yield e sem avançar o
    // limite pedido pelo usuário.
    if ((++iterations & 0x0F) == 0)
      vTaskDelay(1);
    if (errors > maxFrames) {
      Serial.println("[BENCH] erros demais; abortando");
      break;
    }
    const int64_t frameStart = esp_timer_get_time();

    size_t used = 0;
    const int64_t r0 = esp_timer_get_time();
    // Mesma disciplina do caminho normal: o cartão é compartilhado com a tarefa
    // de áudio, então nunca se lê fora do mutex. Toma e devolve por quadro, em
    // vez de segurar durante todo o benchmark, para não matar o áudio de fome.
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(150)) != pdTRUE) {
      ++errors;
      continue;
    }
    const auto result = reader.next(f, jpegBuffer, MAX_JPEG, used);
    if (sdMutex)
      xSemaphoreGive(sdMutex);
    const uint32_t rUs = uint32_t(esp_timer_get_time() - r0);
    if (result != playback::FrameResult::Ready)
      break; // fim do arquivo ou fluxo invalido
    readUs += rUs;
    maxReadUs = max(maxReadUs, rUs);
    totalBytes += used;

    const int64_t d0 = esp_timer_get_time();
    if (!jpeg.openRAM(jpegBuffer, used, jpegDraw)) {
      ++errors;
      continue;
    }
    w = jpeg.getWidth();
    h = jpeg.getHeight();
    if (w < 1 || h < 1 || w > CRT_W || h > CRT_H) {
      jpeg.close();
      ++errors;
      continue;
    }
    videoWidth = w; // o jpegDraw centraliza a partir destes
    videoHeight = h;
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    const bool ok = jpeg.decode(0, 0, 0);
    jpeg.close();
    const uint32_t dUs = uint32_t(esp_timer_get_time() - d0);
    if (!ok) {
      ++errors;
      continue;
    }
    decodeUs += dUs;
    maxDecodeUs = max(maxDecodeUs, dUs);
    maxTotalUs = max(maxTotalUs, uint32_t(esp_timer_get_time() - frameStart));
    ++frames;
  }

  const uint64_t wallUs = uint64_t(esp_timer_get_time() - wallStart);
  benchActive = false;
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  f.close();
  if (sdMutex)
    xSemaphoreGive(sdMutex);

  if (!frames) {
    Serial.printf("[BENCH] nenhum quadro decodificado (erros:%lu)\n", errors);
    return;
  }

  // O blit é cronometrado dentro do jpegDraw, que roda durante o decode, então
  // o tempo de JPEGDEC puro é a diferença entre os dois.
  const uint64_t blitUs = benchBlitUs;
  const uint64_t pureDecodeUs = decodeUs > blitUs ? decodeUs - blitUs : 0;
  const uint32_t avgRead = uint32_t(readUs / frames);
  const uint32_t avgDecode = uint32_t(pureDecodeUs / frames);
  const uint32_t avgBlit = uint32_t(blitUs / frames);
  const uint32_t avgTotal = uint32_t(wallUs / frames);
  const float sustainedFps = avgTotal ? 1000000.0f / avgTotal : 0.0f;
  const float peakFps = maxTotalUs ? 1000000.0f / maxTotalUs : 0.0f;
  const float mibPerS = wallUs ? (float)totalBytes / (float)wallUs : 0.0f; // bytes/us == MB/s

  Serial.println("[BENCH] ----------------------------------------");
  Serial.printf("[BENCH] pasta          : %s\n", dir.c_str());
  Serial.printf("[BENCH] resolucao      : %dx%d   fps do meta.json: %.2f\n", w, h, metaFps);
  Serial.printf("[BENCH] quadros        : %lu   erros: %lu   bytes: %llu\n", frames, errors,
                (unsigned long long)totalBytes);
  Serial.printf("[BENCH] leitura SD     : medio %lu us   maximo %lu us   %.2f MB/s\n", avgRead,
                maxReadUs, mibPerS);
  Serial.printf("[BENCH] decode JPEG    : medio %lu us   maximo %lu us\n", avgDecode, maxDecodeUs);
  Serial.printf("[BENCH] blit CVBS      : medio %lu us   (%lu blocos)\n", avgBlit, benchBlitCalls);
  Serial.printf("[BENCH] quadro inteiro : medio %lu us   pior caso %lu us\n", avgTotal, maxTotalUs);
  Serial.printf("[BENCH] FPS sustentado : %.1f   pior caso %.1f\n", sustainedFps, peakFps);
  // O NTSC entrega 59,94 campos/s; acima disso a TV nao tem como mostrar.
  const float ceiling = min(sustainedFps, 59.94f);
  Serial.printf("[BENCH] teto util      : %.1f fps (limitado por %s)\n", ceiling,
                sustainedFps < 59.94f ? "decodificacao/cartao" : "taxa de campo do NTSC");
  if (sustainedFps < metaFps)
    Serial.printf("[BENCH] ATENCAO: abaixo dos %.2f fps do meta.json; havera descarte de quadros\n",
                  metaFps);
  Serial.println("[BENCH] ----------------------------------------");

  // stopProgram() já levou o estado para VIDEO_LIBRARY; sem redesenhar, a TV
  // ficava preta enquanto o LCD mostrava a tela anterior e os botões passavam a
  // agir como se estivessem na biblioteca.
  videoFrameIndex = 0;
  lastJpegUsed = 0;
  state = VIDEO_LIBRARY;
  drawLibrary();
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
    const String defaultTitle = currentTitle, defaultVideo = video, defaultAudio = audio;
    currentTitle = (const char *)(d["title"] | defaultTitle.c_str());
    fps = d["fps"] | 15.0f;
    video = (const char *)(d["video"] | defaultVideo.c_str());
    audio = (const char *)(d["audio"] | defaultAudio.c_str());
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
  // A música em loop do Weather Channel também segura o áudio: se ela continuar
  // ligada, a condição de ocioso do audioTask nunca vale e a espera abaixo não
  // termina nunca. Tem que cair ANTES do while, não depois.
  weatherAudio = false;
  // Wait for the consumer to acknowledge idle before closing or replacing files.
  while (audioReady && !audioIdle)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (mjpegFile)
    mjpegFile.close();
  if (wavFile)
    wavFile.close();
  mp3End(); // fecha mp3File + libera o decodificador (evita estado de MP3 vazado)
  mjpegReader.reset();
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  uiHudClear(); // apaga só o HUD; o pôster permanece intacto no LCD
  playbackFinished = false;
  // Sem isto o "pausado" vazava para a próxima tela: entrar no Weather depois de
  // pausar um programa deixava o audioTask no ramo de pausa e a música nunca
  // tocava, sem erro nenhum na tela.
  paused = false;
  videoBehind = false; // senão o loop ficaria em 1 ms depois de sair da reprodução
  lastJpegUsed = 0;    // o conteúdo do jpegBuffer deixa de valer
  state = VIDEO_LIBRARY;
}
void videoTick() {
  videoBehind = false;
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
  // Ainda atrás do relógio depois de trabalhar: o loop não deve dormir 4 ms.
  videoBehind = playing && videoFrameIndex < target;
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

// Cache da lista de programas: evita re-varrer o SD inteiro a cada redesenho da
// biblioteca (antes drawLibrary chamava libraryProgramCount + uma varredura por
// linha visível, a cada tecla de navegação).
static constexpr int MAX_LIBRARY_ITEMS = 64;
String libraryPaths[MAX_LIBRARY_ITEMS];
int libraryCount = 0;
bool libraryScanned = false;

void scanLibrary() {
  libraryCount = 0;
  const String root = libraryRoot();
  File d = SD.open(root);
  if (!d) {
    libraryScanned = true;
    return;
  }
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    if (e.isDirectory()) {
      const String p = libraryChildPath(root, String(e.name()));
      if (isProgramFolder(p) && libraryCount < MAX_LIBRARY_ITEMS)
        libraryPaths[libraryCount++] = p;
    }
    e.close();
  }
  d.close();
  libraryScanned = true;
  Serial.printf("[M5RETRO] Biblioteca: %d programa(s) em %s\n", libraryCount, root.c_str());
}

int libraryProgramCount() {
  if (!libraryScanned)
    scanLibrary();
  return libraryCount;
}

String libraryProgramAt(int wantedIndex) {
  if (!libraryScanned)
    scanLibrary();
  return (wantedIndex >= 0 && wantedIndex < libraryCount) ? libraryPaths[wantedIndex] : String("");
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
  uiHud.drawRect(barX, barY, barW, 3, RCA_ACCENT);
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

// Apaga, dentro da faixa do OSD, só o que fica FORA do retângulo do vídeo (o
// letterbox preto). A parte sobre a imagem é restaurada pelo próximo quadro,
// ou por redrawCurrentFrame() com o vídeo pausado — assim o OSD some sem
// piscar uma tarja preta sobre o filme.
static void clearOsdLetterbox() {
  const int top = vcr::layout::kTopY;
  const int vx = (CRT_W - videoWidth) / 2, vy = (CRT_H - videoHeight) / 2;
  const int vr = vx + videoWidth, vb = vy + videoHeight;
  if (vb <= top || vy >= CRT_H) {
    rca.fillRect(0, top, CRT_W, CRT_H - top, TFT_BLACK);
    return;
  }
  const int y0 = max(top, vy);
  if (y0 > top)
    rca.fillRect(0, top, CRT_W, y0 - top, TFT_BLACK);
  if (vb < CRT_H)
    rca.fillRect(0, vb, CRT_W, CRT_H - vb, TFT_BLACK);
  const int h = min(vb, CRT_H) - y0;
  if (vx > 0)
    rca.fillRect(0, y0, vx, h, TFT_BLACK);
  if (vr < CRT_W)
    rca.fillRect(vr, y0, CRT_W - vr, h, TFT_BLACK);
}

// Redecodifica o quadro atual a partir do jpegBuffer (sem tocar no cartão).
// Custa um decode (~10-20 ms em 240x160) e só roda com o vídeo pausado, a
// cada meio segundo, para o PAUSE piscar sobre a imagem parada.
static bool redrawCurrentFrame() {
  if (!jpegBuffer || !lastJpegUsed)
    return false;
  if (!jpeg.openRAM(jpegBuffer, lastJpegUsed, jpegDraw))
    return false;
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  const bool ok = jpeg.decode(0, 0, 0);
  jpeg.close();
  return ok;
}

void drawPlaybackOsd() {
  // OSD de videocassete (include/VcrOsd.h): texto flutuando sobre o vídeo, sem
  // tarja, com contorno preto. Como não há fundo, cada repintura precisa partir
  // de uma imagem limpa: com o filme rodando, logo depois de um quadro novo;
  // pausado, depois de redecodificar o quadro parado.
  static bool visible = false, lastPaused = false, lastBlink = true;
  static uint32_t lastFrame = 0, lastDeadline = 0;
  const uint32_t now = millis();
  const bool isPaused = paused.load();
  const bool show = isPaused || !timeReached(now, osdUntil);
  // O VCR piscava o símbolo de PAUSE; 2 Hz como nos aparelhos da época.
  const bool blink = !isPaused || ((now / 500U) & 1U) == 0;

  if (visible && !show) {
    clearOsdLetterbox(); // o próximo quadro repinta o que ficou sobre a imagem
    visible = false;
    return;
  }
  if (!show)
    return;

  const bool newFrame = lastFrame != renderedSeq;
  const bool changed = !visible || lastPaused != isPaused || lastDeadline != osdUntil || lastBlink != blink;
  if (!newFrame && !changed)
    return;
  if (!newFrame) {
    // Sem quadro novo embaixo (pausado, ou mudança de estado entre quadros):
    // restaura a imagem para não empilhar tinta velha do OSD.
    redrawCurrentFrame();
  }
  clearOsdLetterbox();

  visible = true;
  lastFrame = renderedSeq;
  lastPaused = isPaused;
  lastDeadline = osdUntil;
  lastBlink = blink;

  vcr::State st;
  st.transport = isPaused ? vcr::Transport::Pause : vcr::Transport::Play;
  st.blinkOn = blink;
  st.seconds = sampleRate ? samplesPlayed.load() / sampleRate : 0;
  // "OSD estilo VHS" ligado: painel completo (símbolo, SP, contador). Desligado:
  // só o símbolo e o título, mais discreto sobre o filme.
  st.showCounter = settings.vhsOsd;
  st.speed = settings.vhsOsd ? vcr::Speed::SP : vcr::Speed::None;
  st.title = currentTitle.c_str(); // o OSD normaliza acentos por conta própria
  st.buttonLeft = PTBR::ANTERIOR;
  st.buttonCenter = isPaused ? "PLAY" : "PAUSA"; // vocabulário do painel do VCR
  st.buttonRight = PTBR::PROXIMO;
  vcr::draw(&rca, st);
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

// Encerra podcast/video/audio, desconecta do Wi-Fi e prepara o Core2 para ser
// desligado pelo AXP192. Usado tanto pelo botão físico Power quanto pelo item
// "DESLIGAR" do menu inicial.
void requestPowerOff() {
  if (powerOffPending)
    return;
  powerOffPending = true;
  powerOffAt = millis() + 120;
  playing = false;
  paused = true;
  if (portal.active())
    portal.stop();
  WiFi.disconnect(false, false);
  Serial.println("[M5RETRO] Desligamento solicitado");
  dualText("DESLIGANDO...", "APERTE POWER PARA LIGAR");
}

void servicePowerButton() {
  // M5.BtnPWR is M5Unified's debounced event for the Core2 side Power key.
  // M5.update() runs before this function, so one physical press becomes one
  // shutdown request. The AXP192 then restores power on the next Power press.
  if (!powerOffPending && M5.BtnPWR.wasClicked()) {
    requestPowerOff();
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
    uint32_t fill = hot ? RCA_ACCENT : TFT_BLUE;
    uint32_t ink = hot ? TFT_NAVY : TFT_WHITE;
    M5.Display.fillRoundRect(x, 190, w, 43, 4, fill);
    M5.Display.drawRoundRect(x, 190, w, 43, 4, TFT_WHITE);
    M5.Display.setTextColor(ink, fill);
    M5.Display.drawString(labels[i], x + w / 2, 211);
  }
  // Antes isto dependia do osdUntil do player, então a barra sumia sozinha em
  // telas que nada têm a ver com reprodução — e no boot nem aparecia. Durante o
  // playback quem manda é o OSD de videocassete, que tem legendas próprias.
  if (state != VIDEO_PLAYBACK) {
    rca.fillRect(0, BAR_Y, CRT_W, BAR_H, TFT_NAVY);
    rca.setTextDatum(middle_center);
    rca.setTextSize(1);
    rca.setTextColor(RCA_ACCENT, TFT_NAVY);
    rca.drawString(String("[ ") + left + " ]  [ " + center + " ]  [ " + right + " ]", CRT_W / 2,
                   BAR_Y + BAR_H / 2);
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

// Trunca `src` para caber em ~`maxChars` na fonte bitmap (size 1, fix-width).
// Retorna uma String já truncada, com "..." quando cortada.
static String truncateText(const char *src, int maxChars) {
  if (!src)
    return String("");
  size_t len = strlen(src);
  if ((int)len <= maxChars)
    return String(src);
  String out = String(src).substring(0, maxChars > 3 ? maxChars - 3 : maxChars);
  out += "...";
  return out;
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
    d->drawString(PTBR::MUSICA, SAFE_L, HEAD_Y);
    d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
    // breadcrumb do diretório corrente.
    String crumb = musicDir;
    crumb.replace(String(MUSIC_ROOT), "/");
    d->setTextSize(1);
    d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
    d->drawString(crumb, SAFE_L, HEAD_RULE_Y + 6);
    if (!musicEntryCount)
      d->drawString(PTBR::SEM_MUSICAS, SAFE_L + 6, BODY_Y + 48);
    const int first = (musicSelection / 4) * 4;
    for (int row = 0; row < 4 && first + row < musicEntryCount; ++row) {
      const int index = first + row, y = BODY_Y + 14 + row * 32;
      const bool selected = index == musicSelection;
      d->fillRoundRect(SAFE_L, y - 2, SAFE_W, 28, 4, selected ? RCA_ACCENT : TFT_NAVY);
      d->setTextColor(selected ? TFT_NAVY : (musicEntries[index].isFolder ? RCA_ACCENT : TFT_WHITE),
                      selected ? RCA_ACCENT : TFT_NAVY);
      String label = String(selected ? "> " : "  ") + musicEntries[index].name;
      if (musicEntries[index].isFolder)
        label += "/";
      d->drawString(label.substring(0, 40), SAFE_L + 8, y + 6);
    }
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(String(musicEntryCount ? musicSelection + 1 : 0) + " / " + musicEntryCount,
                  SAFE_R - 80, HEAD_Y);
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
  // Duração: usa o bitrate real do MP3 quando já conhecido (1º frame); senão a
  // estimativa inicial de 128 kbps gravada em wavDataEnd.
  uint32_t totalSec = (sampleRate && wavDataEnd >= wavDataStart) ? (wavDataEnd - wavDataStart) / (sampleRate * 4UL) : 0;
  if (mp3Mode && musicBitrateKbps > 0) {
    totalSec = (uint32_t)(mp3File.size() * 8ULL / (uint64_t)musicBitrateKbps / 1000ULL);
  }
  const uint32_t curSec = sampleRate ? samplesPlayed.load() / sampleRate : 0;
  const int pct = totalSec ? (int)((uint64_t)curSec * (SAFE_W - 2) / totalSec) : 0;
  // Nº da faixa corrente na fila (estilo iPod), para exibição.
  String trackNo = (musicQueueIndex >= 0 && musicQueueCount) ? String(musicQueueIndex + 1) + "/" + String(musicQueueCount) : String("");
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(top_left);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->setTextSize(2);
    d->drawString(PTBR::MUSICA, SAFE_L, HEAD_Y);
    d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
    d->setTextSize(1);
    // Capa do álbum (esquerda) com borda estilo VHS.
    const int coverY = BODY_Y + 8;
    if (coverSprite) {
      const int cw = coverSprite->width(), ch = coverSprite->height();
      const float sc = min(min(88.0f / cw, 88.0f / ch), 1.6f);
      coverSprite->setPivot(cw / 2.0f, ch / 2.0f);
      coverSprite->pushRotateZoom(d, SAFE_L + 44, coverY + 44, 0, sc, sc);
    }
    d->drawRect(SAFE_L, coverY, 88, 88, coverSprite ? RCA_ACCENT : TFT_DARKCYAN);
    // Título + tags (truncados para não estourar à direita).
    const int metaX = SAFE_L + 96;
    d->setTextColor(TFT_YELLOW, TFT_NAVY);
    d->drawString(truncateText(title, 24), metaX, coverY);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(truncateText(musicMeta.artist[0] ? musicMeta.artist : "---", 24), metaX, coverY + 30);
    d->drawString(truncateText(musicMeta.album[0] ? musicMeta.album : "---", 24), metaX, coverY + 50);
    if (musicMeta.year[0])
      d->drawString(musicMeta.year, metaX, coverY + 70);
    // Nº da faixa + bitrate (canto sup. direito, à esquerda do botão voltar).
    if (trackNo.length()) {
      String meta = trackNo;
      if (mp3Mode && musicBitrateKbps > 0)
        meta += " " + String(musicBitrateKbps) + "K";
      d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
      d->drawString(meta, SAFE_R - 96, HEAD_RULE_Y + 6);
    }
    // Progresso (com tempo total correto).
    d->drawRect(SAFE_L, 160, SAFE_W, 6, RCA_ACCENT);
    if (pct > 0)
      d->fillRect(SAFE_L + 1, 161, pct, 4, TFT_YELLOW);
    d->setTextColor(RCA_ACCENT, TFT_NAVY);
    char clockLine[48];
    snprintf(clockLine, sizeof(clockLine), "%s %02lu:%02lu / %02lu:%02lu", isPaused ? "PAUSA" : "PLAY",
             curSec / 60UL, curSec % 60UL, totalSec / 60UL, totalSec % 60UL);
    if (musicShuffle)
      strncat(clockLine, "  SHUFFLE", sizeof(clockLine) - strlen(clockLine) - 1);
    else if (musicRepeat)
      strncat(clockLine, "  REPETIR", sizeof(clockLine) - strlen(clockLine) - 1);
    d->drawString(clockLine, SAFE_L, 174);
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
      // MP3Decode retorna código de erro (0 = sucesso, <0 = erro). A CONTAGEM de
      // amostras de saída vem de MP3GetLastFrameInfo().outputSamps — não do retorno.
      const int err = MP3Decode(mp3Dec, &in, &bytesLeft, mp3FramePcm, 0);
      const size_t consumed = (size_t)(in - mp3In);
      if (consumed > 0) {
        memmove(mp3In, in, mp3InLen - consumed);
        mp3InLen -= consumed;
      }
      if (err == 0) {
        MP3FrameInfo info;
        MP3GetLastFrameInfo(mp3Dec, &info);
        mp3PcmCount = (size_t)info.outputSamps;
        mp3Phase = 0.0;
        mp3Rate = info.samprate;
        mp3Chans = info.nChans;
        if (!musicBitrateKbps && info.bitrate > 0)
          musicBitrateKbps = info.bitrate; // bitrate real do 1º frame (kbps)
      } else if (consumed == 0) {
        // Decoder não avançou (frame corrompido/desincronizado). Descarta 1 byte
        // e tenta-resincronizar, sem travar em busy-loop até o EOF.
        if (mp3InLen > 0) {
          memmove(mp3In, mp3In + 1, mp3InLen - 1);
          --mp3InLen;
        } else if (!mp3File.available()) {
          break; // EOF real
        }
        continue;
      } else {
        continue; // consumiu bytes mas falhou (ex.: frame curto): tenta o próximo
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
  musicBitrateKbps = 0; // recalculado no 1º frame decodificado
  if (isMp3) {
    ok = mp3Begin(path);
    if (ok) {
      sampleRate = 22050;
      wavChannels = 2;
      wavBlockAlign = 4;
      wavDataStart = 0;
      // Duração estimada pelo bitrate (128 kbps como padrão até o 1º frame
      // revelar o bitrate real via musicBitrateKbps).
      const uint32_t estSec = (uint32_t)(mp3File.size() * 8ULL / 128000ULL);
      wavDataEnd = estSec * 22050UL * 4UL;
      id3::readTags(mp3File, musicMeta);
      mp3File.seek(id3::audioStart(mp3File)); // pula a tag ID3v2 antes do decoder
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
  // Reconstrói a fila com as faixas do diretório da faixa tocando. Feito ANTES
  // de iniciar o áudio: o audioTask ainda está ocioso (sem ler o SD), então a
  // varredura e a capa não disputam o barramento com o decoder.
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
  playing = true;       // inicia o áudio SÓ depois das leituras do SD (fila + capa)
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
  // Fim natural da faixa: avança para a próxima (com repeat/shuffle), ou volta.
  if (musicQueueCount) {
    if (musicRepeat && musicQueueIndex >= 0) {
      // REPETIR: toca a mesma faixa de novo.
      if (!startMusic(musicQueue[musicQueueIndex])) {
        stopMusic();
        state = MUSIC_BROWSER;
        drawMusicBrowser();
      }
      return;
    }
    int next = -1;
    if (musicShuffle) {
      next = random(musicQueueCount); // embaralhado (pode repetir, aceitável)
    } else if (musicQueueIndex + 1 < musicQueueCount) {
      next = musicQueueIndex + 1;
    }
    if (next >= 0) {
      musicQueueIndex = next;
      if (!startMusic(musicQueue[musicQueueIndex])) {
        stopMusic();
        state = MUSIC_BROWSER;
        drawMusicBrowser();
      }
      return;
    }
  }
  stopMusic();
  state = MUSIC_BROWSER;
  drawMusicBrowser();
}

// ----------------------------------------------------------------------------
// Transferência de arquivos por Wi-Fi
//
// Esta tela SUSPENDE o resto do aparelho de propósito: reprodução, música do
// Weather e consultas de rede saem de cena. O cartão é disputado com a tarefa de
// áudio e a saída composta tem prazo rígido por linha de varredura — receber
// dezenas de MB com tudo isso rodando junto engasgaria os dois.
// ----------------------------------------------------------------------------
static xfer::State transferState() {
  static String endereco, rede;
  endereco = "http://" + fileTransfer.ip();
  rede = WiFi.SSID();
  xfer::State st;
  st.address = endereco.c_str();
  st.ssid = rede.c_str();
  st.user = fileTransfer.user();
  st.password = fileTransfer.password();
  st.fileName = fileTransfer.fileName();
  st.bytesReceived = (uint32_t)fileTransfer.received();
  st.bytesTotal = (uint32_t)fileTransfer.total();
  switch (fileTransfer.status()) {
  case xfer::TransferStatus::RECEBENDO: st.stage = xfer::Stage::Receiving; break;
  case xfer::TransferStatus::CONCLUIDO: st.stage = xfer::Stage::Done; break;
  case xfer::TransferStatus::FALHA:     st.stage = xfer::Stage::Error; break;
  default:                              st.stage = xfer::Stage::Waiting; break;
  }
  if (fileTransfer.status() == xfer::TransferStatus::FALHA)
    st.statusText = fileTransfer.lastError();
  return st;
}

void drawTransferFrame() {
  const xfer::State st = transferState();
  xfer::draw(&rca, st);
  xfer::draw(&M5.Display, st);
  lastTransferStage = st.stage;
  drawBackButton();
}

void startTransfer() {
  stopProgram();  // encerra vídeo E a música em loop do Weather
  stopWeather();
  if (!network.connected()) {
    setError(PTBR::SEM_WIFI);
    return;
  }
  // Sem isto o Wi-Fi fica em modem sleep: o rádio só acorda a cada DTIM, o ping
  // sobe para centenas de ms e o SYN de entrada se perde. Para receber dezenas
  // de MB o rádio precisa ficar acordado. Religado ao sair, para não custar
  // bateria nas outras telas.
  WiFi.setSleep(false);
  if (!fileTransfer.begin(sdMutex)) {
    WiFi.setSleep(true);
    setError(fileTransfer.lastError());
    return;
  }
  state = FILE_TRANSFER;
  lastTransferStage = xfer::Stage::Error; // garante o primeiro desenho
  lastTransferDraw = 0;
  drawTransferFrame();
  Serial.printf("[M5RETRO] Transferencia em http://%s:%u usuario:%s senha:%s escutando:%d\n",
                fileTransfer.ip().c_str(), (unsigned)fileTransfer.port(), fileTransfer.user(),
                fileTransfer.password(), (int)fileTransfer.listening());
}

void stopTransfer() {
  fileTransfer.stop();
  WiFi.setSleep(true); // devolve a economia de energia às demais telas
  state = HOME;
  drawHome();
}

void transferTick() {
  fileTransfer.handle(); // não-bloqueante
  const uint32_t now = millis();
  const xfer::Stage agora = transferState().stage;
  // Redesenho parcial a 4 Hz enquanto recebe; troca de estágio redesenha tudo.
  if (agora != lastTransferStage) {
    drawTransferFrame();
  } else if (agora == xfer::Stage::Receiving && now - lastTransferDraw >= 250) {
    lastTransferDraw = now;
    const xfer::State st = transferState();
    xfer::drawProgress(&rca, st);
    xfer::drawProgress(&M5.Display, st);
  }
}

// Data e hora com segundos, do relógio sincronizado por NTP. Antes do primeiro
// sincronismo o epoch fica em 1970, e aí mostrar "01/01/1970" seria pior que
// não mostrar nada — nesse caso sai um aviso.
static void drawHomeClock(bool limpar) {
  char texto[32];
  const time_t agora = time(nullptr);
  if (agora >= 1704067200) { // 2024-01-01: houve sincronismo NTP
    struct tm t;
    localtime_r(&agora, &t);
    snprintf(texto, sizeof(texto), "%02d/%02d/%04d  %02d:%02d:%02d", t.tm_mday, t.tm_mon + 1,
             t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    snprintf(texto, sizeof(texto), "%s", "RELOGIO NAO SINCRONIZADO");
  }
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    if (limpar)
      d->fillRect(SAFE_L, CLOCK_Y, SAFE_W, 18, TFT_NAVY);
    d->setFont(&fonts::Font2);
    d->setTextSize(1);
    d->setTextDatum(top_right);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(texto, SAFE_R - 1, CLOCK_Y);
    d->setTextDatum(top_left);
    d->setFont(&fonts::Font0);
  }
  lastClockDraw = millis();
}

void drawHome() {
  // 8 itens: VIDEOS, MUSICA, TRAFEGO, CONFIGURACOES, SISTEMA, TEMPO,
  // TRANSFERIR ARQUIVOS, DESLIGAR.
  const char *items[] = {PTBR::VIDEOS,       PTBR::MUSICA,  PTBR::TRAFEGO,
                         PTBR::CONFIGURACOES, PTBR::INFO_SISTEMA, PTBR::WEATHER,
                         PTBR::TRANSFERENCIA, PTBR::DESLIGAR};
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.drawString(PTBR::APP, 12, 8);
  M5.Display.drawFastHLine(8, 36, 304, RCA_ACCENT);
  M5.Display.setTextSize(2);
  for (int i = 0; i < HOME_COUNT; i++) {
    int y = HOME_LCD_Y0 + i * HOME_LCD_STEP;
    bool selected = i == homeSelection;
    if (selected)
      M5.Display.fillRoundRect(12, y - 2, 296, 18, 4, RCA_ACCENT);
    M5.Display.setTextColor(selected ? TFT_NAVY : TFT_WHITE, selected ? RCA_ACCENT : TFT_NAVY);
    M5.Display.drawString(String(selected ? "> " : "  ") + items[i], 24, y);
  }
  rca.fillScreen(TFT_NAVY);
  rca.setTextDatum(top_left);
  rca.setTextSize(2);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(PTBR::APP, SAFE_L, HEAD_Y);
  rca.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  rca.setTextSize(1);
  for (int i = 0; i < HOME_COUNT; i++) {
    // Passo de 16 px: com 8 itens e o relógio ocupando uma linha, 20 px faria o
    // último item invadir a barra de legendas em 202.
    int y = CLOCK_Y + 20 + i * 16;
    rca.setTextColor(i == homeSelection ? RCA_ACCENT : TFT_WHITE, TFT_NAVY);
    rca.drawString(String(i == homeSelection ? "> " : "  ") + items[i], SAFE_L + 4, y);
  }
  drawHomeClock(false); // a tela acabou de ser preenchida; não precisa limpar
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
    display->drawString(PTBR::VIDEOS, SAFE_L, HEAD_Y);
    display->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
    display->setTextSize(1);
    if (!count)
      display->drawString("SEM VIDEOS NO CARTAO", SAFE_L + 6, BODY_Y + 48);
    for (int row = 0; row < 4 && first + row < count; ++row) {
      const int index = first + row, y = BODY_Y + 14 + row * 32;
      String path = libraryProgramAt(index);
      // Nome de pasta vem do cartão e pode ter acento. As fontes bitmap são
      // ASCII: sem normalizar, cada letra acentuada em UTF-8 vira DOIS espaços
      // ("Nao Me Deixes" sairia "N  o Me Deixes").
      char title[48];
      ascii::normalize(title, sizeof(title), path.substring(path.lastIndexOf('/') + 1).c_str());
      const bool selected = index == librarySelection;
      display->fillRoundRect(SAFE_L, y - 2, SAFE_W, 28, 4, selected ? RCA_ACCENT : TFT_NAVY);
      display->setTextColor(selected ? TFT_NAVY : TFT_WHITE, selected ? RCA_ACCENT : TFT_NAVY);
      display->drawString(String(selected ? "> " : "  ") + title, SAFE_L + 8, y + 6);
    }
    display->setTextColor(TFT_WHITE, TFT_NAVY);
    display->drawString(String(count ? librarySelection + 1 : 0) + " / " + count, SAFE_R - 80, HEAD_Y);
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
    d->drawString(PTBR::APP, SAFE_L, SAFE_T);
    d->drawString(PTBR::TRAFEGO, SAFE_L, SAFE_T + 15);

    // Scope: anel externo (alcance total), anel interno (metade) e mira.
    // Centro/raio escolhidos para que os rótulos N/S/L/O e a legenda de
    // alcance caibam inteiros na área segura, sem encostar no painel lateral.
    const int cx = 96, cy = 118, R = 58;
    d->drawCircle(cx, cy, R, RCA_ACCENT);
    d->drawCircle(cx, cy, R / 2, TFT_DARKCYAN);
    d->drawFastHLine(cx - R, cy, 2 * R, TFT_DARKCYAN);
    d->drawFastVLine(cx, cy - R, 2 * R, TFT_DARKCYAN);

    // Pontos cardeais (fora do anel) + alcance na base do scope.
    d->setTextDatum(middle_center);
    d->setTextColor(RCA_ACCENT, TFT_NAVY);
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
        d->drawLine(cx, cy, px, py, RCA_ACCENT);
      drawAirplane(d, px, py, aircraft[i].heading_deg, sel ? RCA_ACCENT : TFT_YELLOW);
    }

    // Painel lateral de dados (à direita do scope), sempre acima da barra de
    // controle (y < 184).
    const int PX = SAFE_L + 144; // 168
    d->setTextDatum(top_left);
    if (radarSelection >= 0 && radarSelection < aircraftCount) {
      const Aircraft &p = aircraft[radarSelection];
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->drawString(p.callsign.length() ? p.callsign : p.icao, PX, SAFE_T + 18);
      d->setTextColor(RCA_ACCENT, TFT_NAVY);
      d->drawString("ALT FL" + String((int)(p.altitude_ft / 100)), PX, SAFE_T + 40);
      d->drawString("VEL " + String((int)p.speed_kt) + " KT", PX, SAFE_T + 58);
      double y = (p.latitude - settings.lat) * 111.0;
      double x = (p.longitude - settings.lon) * 111.0 * cos(settings.lat * DEG_TO_RAD);
      int distKm = (int)sqrt(x * x + y * y);
      int proa = (int)(atan2(x, y) * RAD_TO_DEG);
      if (proa < 0)
        proa += 360;
      d->drawString("DIST " + String(distKm) + " km  PROA " + String(proa), PX, SAFE_T + 76);
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      if (radarDetails) {
        d->drawString(p.icao, PX, SAFE_T + 104);
        d->drawString(p.aircraft_type, PX, SAFE_T + 120);
      }
    } else {
      d->setTextColor(RCA_ACCENT, TFT_NAVY);
      d->drawString(PTBR::AERONAVES_RASTREADAS, PX, SAFE_T + 32);
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->drawString(String(aircraftCount), PX, SAFE_T + 54);
    }
    d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
    d->drawString(String(settings.rangeKm) + " km de alcance", PX, SAFE_T + 142);
    d->setTextColor(RCA_ACCENT, TFT_NAVY);
    d->drawString(apiStatus.substring(0, 20), PX, SAFE_T + 158);
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

// Fonte: Open-Meteo (api.open-meteo.com) — API pública, sem cadastro nem chave.
// Substituiu o wttr.in: o j1 de Franca devolve ~39 KB e não cabia no buffer de
// 24 KB, então o JSON chegava truncado e toda consulta caía em "ERRO NA
// CONSULTA". A resposta do Open-Meteo abaixo tem ~800 bytes.
static const char *WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=-20.5386&longitude=-47.4008"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,"
    "wind_direction_10m&daily=weather_code,temperature_2m_max,temperature_2m_min"
    "&timezone=America%2FSao_Paulo&forecast_days=3";

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

static bool weatherParse(JsonDocument &doc, WeatherData &out) {
  JsonObject cur = doc["current"];
  if (cur.isNull())
    return false;
  if (!cur["temperature_2m"].is<float>())
    return false;
  out.tempC = (int)lroundf(cur["temperature_2m"] | 0.0f);
  out.humidity = (int)lroundf(cur["relative_humidity_2m"] | 0.0f);
  out.windKmph = (int)lroundf(cur["wind_speed_10m"] | 0.0f);
  out.wmoCode = cur["weather_code"] | -1;
  snprintf(out.cond, sizeof(out.cond), "%s", wmoConditionPt(out.wmoCode));
  snprintf(out.windDir, sizeof(out.windDir), "%s",
           windDirPt(cur["wind_direction_10m"] | 0.0f));

  JsonObject daily = doc["daily"];
  if (daily.isNull())
    return false;
  JsonArray date = daily["time"], tmax = daily["temperature_2m_max"],
            tmin = daily["temperature_2m_min"], dcode = daily["weather_code"];
  if (date.isNull() || tmax.isNull() || tmin.isNull() || date.size() < 3 ||
      tmax.size() < 3 || tmin.size() < 3)
    return false;
  for (int i = 0; i < 3; ++i) {
    snprintf(out.days[i].name, sizeof(out.days[i].name), "%s",
             weekdayPt(weekdayFromIso(date[i] | "")));
    out.days[i].maxC = (int)lroundf(tmax[i] | 0.0f);
    out.days[i].minC = (int)lroundf(tmin[i] | 0.0f);
    out.days[i].wmoCode = dcode.isNull() ? -1 : (dcode[i] | -1);
  }
  return true;
}

static bool weatherFetch(WeatherData &out) {
  WiFiClientSecure client;
  client.setInsecure(); // sem CA (economiza RAM)
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  bool ok = false;
  if (http.begin(client, WEATHER_URL)) {
    const int code = http.GET();
    if (code == HTTP_CODE_OK) {
      // A resposta tem ~800 bytes, então lê tudo para um buffer contíguo (o
      // leitor Stream do ArduinoJson 7.4.2 descarta escalares no ESP32) e
      // rejeita corpo truncado em vez de publicar dados pela metade.
      // O buffer fica na PSRAM, não na pilha: esta função roda na tarefa
      // WEATHER_HTTP, que tem 8 KB de pilha e ainda precisa acomodar os quadros
      // do HTTPClient e do mbedTLS. 4 KB de array local comiam metade dela.
      constexpr int cap = 4096;
      char *buf = (char *)ps_malloc(cap);
      if (!buf) {
        Serial.println("[M5RETRO] Tempo: sem memoria para a resposta");
        http.end();
        return false;
      }
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
        Serial.println("[M5RETRO] Tempo: resposta maior que o buffer");
      } else if (len) {
        JsonDocument doc;
        auto err = deserializeJson(doc, buf, DeserializationOption::NestingLimit(8));
        if (!err)
          ok = weatherParse(doc, out);
        else
          Serial.printf("[M5RETRO] Tempo: parse %s (%u bytes)\n", err.c_str(), (unsigned)len);
      } else {
        Serial.println("[M5RETRO] Tempo: corpo vazio");
      }
      free(buf);
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
    weatherStatus.store("ATUALIZADO");
    Serial.printf("[M5RETRO] Tempo: %dC umidade:%d%% %s\n", local.tempC, local.humidity, local.cond);
  } else {
    weatherStatus.store("ERRO NA CONSULTA");
    Serial.println("[M5RETRO] Tempo: falha na consulta");
    // Sem isso a tela fica presa em "CARREGANDO..." quando a PRIMEIRA consulta
    // falha: weatherTick() só redesenha quando a versão muda.
    if (!weatherReady.load())
      weatherVersion.fetch_add(1);
  }
  weatherBusy.store(false);
  vTaskDelete(nullptr);
}

void pollWeather() {
  if (state != WEATHER || weatherBusy.load())
    return;
  if (!network.connected()) {
    if (!weatherReady.load())
      weatherStatus.store("SEM WI-FI");
    return;
  }
  const uint32_t now = millis();
  // O backoff vale para toda tentativa, não só antes da primeira que der certo.
  // Antes, com dados já em mãos, o único portão era lastWeatherGood: se a
  // consulta falhasse ele não avançava, a condição seguia falsa e cada loop()
  // criava outra tarefa HTTP de 8 KB — dezenas por segundo com o roteador fora.
  if (lastWeatherAttempt && now - lastWeatherAttempt < WEATHER_RETRY_MS)
    return;
  if (weatherReady.load() && now - lastWeatherGood < WEATHER_REFRESH_MS)
    return;
  lastWeatherAttempt = now;
  weatherBusy.store(true);
  int next = 1 - weatherActive.load();
  if (xTaskCreatePinnedToCore(weatherNetworkTask, "WEATHER_HTTP", 8192, (void *)(intptr_t)next, 0,
                              nullptr, 1) != pdPASS) {
    weatherBusy.store(false);
    weatherStatus.store("SEM MEMORIA");
  }
}

// Fundo do Local Forecast: azul profundo estável com um gradiente vertical
// discreto, como o Weather Star 4000.
//
// O que havia aqui antes era um ciclo COMPLETO de matiz de 12 s — as três
// senoides defasadas levavam o fundo por verde, laranja e rosa. Era isso, e não
// artefato de PAL-M, que deixava a tela ciano-berrante no tubo. O Weather
// Channel dos anos 80 era azul e ficava azul; o movimento vinha do ticker e das
// transições entre páginas, nunca do fundo.
static constexpr uint16_t WX_TOP = 0x0010;    // azul quase preto (topo)
static constexpr uint16_t WX_BOTTOM = 0x18BF; // azul de cobalto (base)

static uint16_t weatherBackground() { return WX_BOTTOM; }

// Pinta o gradiente por linhas. Em RGB565 as 32 faixas de azul e 64 de verde
// dão passos imperceptíveis a 240 linhas; em RGB332 isso bandearia, mas o
// painel composto roda em 16 bits.
static inline uint16_t weatherGradientRow(int y) {
  const int r0 = (WX_TOP >> 11) & 0x1F, g0 = (WX_TOP >> 5) & 0x3F, b0 = WX_TOP & 0x1F;
  const int r1 = (WX_BOTTOM >> 11) & 0x1F, g1 = (WX_BOTTOM >> 5) & 0x3F, b1 = WX_BOTTOM & 0x1F;
  const int r = r0 + (r1 - r0) * y / (CRT_H - 1);
  const int g = g0 + (g1 - g0) * y / (CRT_H - 1);
  const int b = b0 + (b1 - b0) * y / (CRT_H - 1);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Pinta o gradiente em FAIXAS, não linha a linha. Entre 0x0010 e 0x18BF cabem só
// 16 cores distintas em 240 linhas, e elas saem contíguas: eram 240 chamadas de
// desenho para 16 retângulos. Como um slide repinta as duas páginas a cada passo
// (33 passos = 66 pinturas de fundo), a diferença é 15.840 chamadas contra 1.056.
static void weatherPaintBackground(lgfx::LovyanGFX *dst, int oy) {
  int start = 0;
  uint16_t color = weatherGradientRow(0);
  for (int y = 1; y < CRT_H; ++y) {
    const uint16_t c = weatherGradientRow(y);
    if (c == color)
      continue;
    dst->fillRect(0, oy + start, CRT_W, y - start, color);
    start = y;
    color = c;
  }
  dst->fillRect(0, oy + start, CRT_W, CRT_H - start, color);
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
  // Repete até o texto desenhado cobrir uma volta inteira MAIS a largura da
  // tela. Com só duas cópias, depois do wrap sobrava exatamente uma volta e
  // abria um vão preto na borda direita a cada ciclo.
  const String unit = tickerPayload + "      ";
  rca.setFont(&fonts::Font2);
  rca.setTextSize(1);
  tickerWrapAt = rca.textWidth(unit.c_str());
  tickerFull = unit;
  while (tickerWrapAt > 0 && rca.textWidth(tickerFull.c_str()) < tickerWrapAt + CRT_W)
    tickerFull += unit;
  tickerOffset = 0;
}

// Desenha o letreiro DIRETO no destino, com recorte na faixa.
//
// Antes isto era um LGFX_Sprite empurrado a 30 Hz. O problema não era o sprite:
// era a faixa ter DOIS donos. Os pintores preenchem a tela inteira, incluindo
// 206..239, e o sprite só repintava a faixa até 33 ms depois — nessa janela a
// faixa mostrava fundo em vez do letreiro. Medido com "diag scan": a faixa
// estava com fundo puro, zero pixels de ciano, enquanto a TV mostrava letreiro.
// Era esse conflito, e não rasgo de varredura, o glitch pior de todos na parte
// de baixo: é o único elemento com dois escritores assíncronos.
//
// Agora há um dono só. Os pintores chamam isto no fim, então toda repintura já
// sai coerente, e o tique de 33 ms só atualiza a mesma faixa. De quebra some o
// sprite de 320x16, devolvendo memória à SRAM interna, que é o recurso apertado.
static void weatherDrawTicker(lgfx::LovyanGFX *dst, int ox, int oy) {
  const int y = oy + TICKER_Y;
  dst->setClipRect(ox, y, CRT_W, TICKER_H);
  dst->fillRect(ox, y, CRT_W, TICKER_H, TFT_BLACK);
  dst->setFont(&fonts::Font2);
  dst->setTextSize(1);
  dst->setTextDatum(top_left);
  dst->setTextColor(RCA_ACCENT, TFT_BLACK);
  dst->drawString(tickerFull.c_str(), ox - tickerOffset, y);
  dst->clearClipRect();
}

static void weatherTickerTick() {
  if (tickerWrapAt > 0 && tickerOffset >= tickerWrapAt)
    tickerOffset -= tickerWrapAt;
  weatherDrawTicker(&rca, 0, 0);
}

// ----------------------------------------------------------------------------
// Local Forecast em duas páginas, alternadas com transição — a cadência do
// Weather Star 4000. Os pintores recebem (ox, oy) porque o slide desenha as
// duas páginas deslocadas no mesmo quadro; com deslocamento zero é o desenho
// normal. Ver include/ScreenFx.h.
// ----------------------------------------------------------------------------
static constexpr uint32_t WEATHER_PAGE_MS = 10000; // tempo de cada página
static int weatherPage = 0;
static uint32_t lastWeatherPageMs = 0;
// Fora da função: precisa ser zerado ao entrar na tela, senão a volta ao Weather
// reaproveita o estado da visita anterior e nada é redesenhado.
static uint32_t lastWeatherVersion = UINT32_MAX;
static crt::fx::Transition weatherFx;

// Cabeçalho comum: cidade + régua, a barra de título do WS4000.
static void weatherHeader(lgfx::LovyanGFX *dst, int ox, int oy, const char *title) {
  dst->setFont(&fonts::Font4);
  dst->setTextDatum(top_center);
  dst->setTextSize(1);
  dst->setTextColor(TFT_YELLOW);
  dst->drawString(title, ox + CRT_W / 2, oy + SAFE_T);
  dst->drawFastHLine(ox + SAFE_L, oy + SAFE_T + 30, SAFE_W, RCA_ACCENT);
  // A régua de cima do ticker também vive aqui: se ficar só no drawWeatherFrame,
  // a primeira transição repinta a tela e ela some pelo resto da sessão.
  dst->drawFastHLine(ox + SAFE_L, oy + TICKER_Y - 4, SAFE_W, RCA_ACCENT);
}

// Página 0 — condições atuais: ícone grande à esquerda, dados à direita.
static void paintCurrent(lgfx::LovyanGFX *dst, int ox, int oy, void *) {
  // startWrite/endWrite agrupam a página inteira numa transação só, em vez de
  // uma por primitiva. São dezenas de primitivas por pintura, e o slide pinta
  // duas páginas por passo.
  dst->startWrite();
  weatherPaintBackground(dst, oy);
  weatherHeader(dst, ox, oy, "FRANCA - SP");
  if (!weatherReady.load()) {
    dst->setFont(&fonts::Font2);
    dst->setTextSize(1);
    dst->setTextColor(TFT_WHITE);
    dst->setTextDatum(top_center);
    dst->drawString(weatherStatus.load(), ox + CRT_W / 2, oy + SAFE_T + 86);
    weatherDrawTicker(dst, ox, oy);
    dst->endWrite();
    return;
  }
  const WeatherData &w = weatherShadows[weatherActive.load()];
  wx::drawWeatherIcon(dst, ox + SAFE_L + 46, oy + SAFE_T + 96, 76, wx::iconFromWmo(w.wmoCode));

  char buf[32];
  const int col = ox + SAFE_L + 176; // coluna de texto, à direita do ícone
  dst->setFont(&fonts::Font2);
  dst->setTextSize(1);
  dst->setTextDatum(top_center);
  dst->setTextColor(TFT_WHITE);
  dst->drawString(w.cond, col, oy + SAFE_T + 46);
  snprintf(buf, sizeof(buf), "%d C", w.tempC);
  dst->setFont(&fonts::Font4);
  dst->setTextSize(2);
  dst->setTextColor(TFT_YELLOW);
  dst->drawString(buf, col, oy + SAFE_T + 70);
  dst->setFont(&fonts::Font2);
  dst->setTextSize(1);
  dst->setTextColor(TFT_WHITE);
  snprintf(buf, sizeof(buf), "UMIDADE  %d%%", w.humidity);
  dst->drawString(buf, ox + CRT_W / 2, oy + SAFE_T + 140);
  snprintf(buf, sizeof(buf), "VENTO  %d KM/H  %s", w.windKmph, w.windDir);
  dst->drawString(buf, ox + CRT_W / 2, oy + SAFE_T + 158);
  weatherDrawTicker(dst, ox, oy);
  dst->endWrite();
}

// Página 1 — previsão de três dias em colunas, com ícone por dia.
static void paintForecast(lgfx::LovyanGFX *dst, int ox, int oy, void *) {
  dst->startWrite();
  weatherPaintBackground(dst, oy);
  weatherHeader(dst, ox, oy, "PREVISAO 3 DIAS");
  if (!weatherReady.load()) {
    // Mesmo recurso da página 0: sem isto, voltar à tela já na página 1 e sem
    // rede deixava um azul vazio, só com o título, para sempre.
    dst->setFont(&fonts::Font2);
    dst->setTextSize(1);
    dst->setTextColor(TFT_WHITE);
    dst->setTextDatum(top_center);
    dst->drawString(weatherStatus.load(), ox + CRT_W / 2, oy + SAFE_T + 86);
    weatherDrawTicker(dst, ox, oy);
    dst->endWrite();
    return;
  }
  const WeatherData &w = weatherShadows[weatherActive.load()];
  const int colW = SAFE_W / 3;
  char buf[24];
  for (int i = 0; i < 3; ++i) {
    const int cx = ox + SAFE_L + colW * i + colW / 2;
    dst->setFont(&fonts::Font2);
    dst->setTextSize(1);
    dst->setTextDatum(top_center);
    dst->setTextColor(RCA_ACCENT);
    dst->drawString(w.days[i].name, cx, oy + SAFE_T + 42);
    wx::drawWeatherIcon(dst, cx, oy + SAFE_T + 96, 56, wx::iconFromWmo(w.days[i].wmoCode));
    dst->setTextColor(TFT_YELLOW);
    snprintf(buf, sizeof(buf), "%d", w.days[i].maxC);
    dst->drawString(buf, cx, oy + SAFE_T + 128);
    dst->setTextColor(TFT_WHITE);
    snprintf(buf, sizeof(buf), "%d", w.days[i].minC);
    dst->drawString(buf, cx, oy + SAFE_T + 148);
  }
  dst->setFont(&fonts::Font2);
  dst->setTextColor(RCA_ACCENT);
  dst->setTextDatum(top_center);
  dst->drawString("MAXIMA / MINIMA EM GRAUS C", ox + CRT_W / 2, oy + SAFE_T + 166);
  weatherDrawTicker(dst, ox, oy);
  dst->endWrite();
}

static crt::fx::Screen weatherScreen(int page) {
  return crt::fx::screen(page ? paintForecast : paintCurrent);
}

void drawWeatherFrame() {
  lastWeatherBg = weatherBackground();
  if (weatherPage)
    paintForecast(&rca, 0, 0, nullptr);
  else
    paintCurrent(&rca, 0, 0, nullptr);
}
void weatherTick() {
  const uint32_t now = millis();
  const uint32_t version = weatherVersion.load();

  // Uma transição em curso manda no quadro: nada mais desenha até ela acabar.
  if (weatherFx.busy()) {
    weatherFx.tick(now);
  } else if (version != lastWeatherVersion) {
    // Dados novos entram com cortina de cima para baixo, como o WS4000 fazia ao
    // trocar de cartela. Não precisa de buffer nenhum.
    lastWeatherVersion = version;
    weatherTickerBuild();
    weatherFx.attach(&rca);
    weatherFx.wipe(now, weatherScreen(weatherPage), crt::fx::DIR_DOWN);
    lastWeatherPageMs = now;
  } else if (weatherReady.load() && now - lastWeatherPageMs >= WEATHER_PAGE_MS) {
    // Rodízio das páginas: slide horizontal, a transição típica entre
    // "condições atuais" e "previsão estendida". Também sem buffer.
    const int next = weatherPage ? 0 : 1;
    weatherFx.attach(&rca);
    weatherFx.slide(now, weatherScreen(weatherPage), weatherScreen(next), crt::fx::DIR_LEFT);
    weatherPage = next;
    lastWeatherPageMs = now;
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
    weatherStatus.store("SEM MUSICA (TELA ATIVA)");
  }
  if (sdMutex)
    xSemaphoreGive(sdMutex);

  weatherAudio = ok; // música em loop apenas se o WAV abriu
  weatherReady = false;
  lastWeatherAttempt = 0; // força a primeira consulta imediata
  state = WEATHER;

  // O letreiro não usa mais sprite: weatherDrawTicker() desenha direto no
  // framebuffer, o que elimina o segundo escritor da faixa e devolve os 5.120
  // bytes de SRAM interna que o sprite ocupava.
  // Toda visita começa na página 0, com o rodízio e o controle de versão
  // zerados; senão a tela herda o estado da visita anterior.
  weatherPage = 0;
  lastWeatherPageMs = millis();
  lastWeatherVersion = UINT32_MAX;
  weatherFx.attach(&rca);
  weatherFx.skip();
  weatherTickerBuild();
  drawWeatherFrame();

  // No LCD do Core2, apenas uma placa simples (a experiência é na TV).
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.drawString(PTBR::WEATHER, 12, 8);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(RCA_ACCENT, TFT_NAVY);
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
                         PTBR::OSD_ESTILO_VHS, "WI-FI", "API", "CARTAO SD", "CONFIGURAR REDE"};
  String audio = settings.audioOutput == AudioOutput::RCA        ? PTBR::RCA
                 : settings.audioOutput == AudioOutput::INTERNAL ? PTBR::ALTO_FALANTE_INTERNO
                                                                 : PTBR::MUDO;
  String value = settingsSelection == 0   ? "NTSC"
                 : settingsSelection == 1 ? String(settings.volume) + "%"
                 : settingsSelection == 2 ? String(settings.rangeKm) + " km"
                 : settingsSelection == 3 ? String(settings.refreshSeconds) + " s"
                 : settingsSelection == 4 ? audio
                 : settingsSelection == 5 ? (settings.vhsOsd ? PTBR::ATIVADO : PTBR::DESATIVADO)
                 : settingsSelection == 6
                     ? (WiFi.status() == WL_CONNECTED ? PTBR::CONECTADO : PTBR::DESCONECTADO)
                 : settingsSelection == 7  ? apiStatus
                 : settingsSelection == 8  ? PTBR::DISPONIVEL
                                            : "ABRIR";
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.drawString(PTBR::CONFIGURACOES, 12, 8);
  M5.Display.drawFastHLine(8, 36, 304, RCA_ACCENT);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(RCA_ACCENT, TFT_NAVY);
  M5.Display.drawString(String(settingsEditing ? "> " : "  ") + names[settingsSelection], 20, 68);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.drawString(value, 34, 110);
  rca.fillScreen(TFT_NAVY);
  rca.setTextDatum(top_left);
  rca.setTextSize(2);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(PTBR::CONFIGURACOES, SAFE_L, HEAD_Y);
  rca.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  rca.setTextSize(1);
  rca.setTextColor(RCA_ACCENT, TFT_NAVY);
  rca.drawString(String(settingsEditing ? "> " : "  ") + names[settingsSelection], SAFE_L, BODY_Y + 26);
  rca.setTextColor(TFT_WHITE, TFT_NAVY);
  rca.drawString(value, SAFE_L + 10, BODY_Y + 58);
  drawControllerLabels(settingsEditing ? "-" : "ACIMA", settingsEditing ? "SALVAR" : "OK",
                       settingsEditing ? "+" : "ABAIXO");
}
void drawInfo() {
  // Página 0: hardware; página 1: rede. Desenha tudo com datum top_left e
  // linhas separadas, truncadas, para nunca vazar do LCD (320x240, fonte ASCII).
  for (auto *d : {static_cast<M5GFX *>(&rca), static_cast<M5GFX *>(&M5.Display)}) {
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(top_left);
    d->setTextSize(2);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(PTBR::INFO_SISTEMA, SAFE_L, HEAD_Y);
    d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
    d->setTextSize(1);
    const int y0 = BODY_Y;
    d->setTextColor(RCA_ACCENT, TFT_NAVY);
    if (!infoPage) {
      d->drawString("MEMORIA LIVRE", SAFE_L, y0);
      d->drawString("PSRAM LIVRE", SAFE_L, y0 + 26);
      d->drawString("VERSAO", SAFE_L, y0 + 52);
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->drawString(String(ESP.getFreeHeap()) + " bytes", SAFE_L + 116, y0);
      d->drawString(String(ESP.getFreePsram()) + " bytes", SAFE_L + 116, y0 + 26);
      d->drawString("core2", SAFE_L + 116, y0 + 52);
    } else {
      d->drawString(PTBR::STATUS_REDE, SAFE_L, y0);
      d->drawString(PTBR::SINAL, SAFE_L, y0 + 26);
      const String st = (WiFi.status() == WL_CONNECTED) ? PTBR::CONECTADO : PTBR::DESCONECTADO;
      d->setTextColor(TFT_WHITE, TFT_NAVY);
      d->drawString(st, SAFE_L + 144, y0);
      if (WiFi.status() == WL_CONNECTED) {
        d->drawString(String(WiFi.RSSI()) + " dBm", SAFE_L + 144, y0 + 26);
        d->setTextColor(RCA_ACCENT, TFT_NAVY);
        d->drawString(PTBR::ENDERECO_IP, SAFE_L, y0 + 52);
        d->setTextColor(TFT_WHITE, TFT_NAVY);
        d->drawString(WiFi.localIP().toString(), SAFE_L + 144, y0 + 52);
      } else {
        d->drawString("-", SAFE_L + 144, y0 + 26);
      }
    }
  }
  drawBackButton();
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
      homeSelection = (homeSelection + HOME_COUNT - 1) % HOME_COUNT;
    else if (a == NavAction::RIGHT)
      homeSelection = (homeSelection + 1) % HOME_COUNT;
    else if (a == NavAction::SELECT) {
      if (homeSelection == HOME_POWER_OFF) { // DESLIGAR: apaga o Core2 via AXP192.
        requestPowerOff();
        return;
      }
      state = homeTarget(homeSelection);
      if (state == VIDEO_LIBRARY) {
        libraryScanned = false; // revarre ao entrar na biblioteca
        drawLibrary();
      } else if (state == MUSIC_BROWSER)
        drawMusicBrowser();
      else if (state == AIRCRAFT_RADAR) {
        lastApiPoll = 0; // consulta imediata ao entrar no radar
        drawRadar();
      } else if (state == SETTINGS)
        drawSettings();
      else if (state == WEATHER)
        startWeather();
      else if (state == FILE_TRANSFER)
        startTransfer();
      else
        drawInfo();
      return;
    }
    drawHome();
    return;
  }
  if (state == FILE_TRANSFER) {
    // Qualquer botão sai: a tela não tem navegação interna.
    if (a != NavAction::NONE)
      stopTransfer();
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
    } else if (a == NavAction::LEFT || a == NavAction::RIGHT) {
      stopProgram();
      int count = libraryProgramCount();
      if (count) {
        int delta = (a == NavAction::LEFT) ? count - 1 : 1;
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
    return;
  }
  if (state == SETTINGS) {
    constexpr int SETTINGS_COUNT = 10;
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
      if (!settingsEditing && settingsSelection == 9) {
        startSetupPortal();
        return;
      }
      if (settingsSelection == 0 || (settingsSelection >= 6 && settingsSelection <= 8))
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
      // Um toque curto = PLAY/PAUSA; um comando de "alternar modo" é feito via
      // toque longo no centro (HOME/BACK), então aqui só PAUSA/RETOMA faz sentido.
      paused = !paused;
      drawMusicNowPlaying();
      return;
    }
    if (a == NavAction::LEFT || a == NavAction::RIGHT) {
      if (musicQueueCount) {
        // Mantém a posição e toca a anterior/próxima.
        const int delta = (a == NavAction::LEFT) ? -1 : 1;
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
      if (state == HOME && p.y >= HOME_LCD_Y0 &&
          p.y < HOME_LCD_Y0 + HOME_COUNT * HOME_LCD_STEP) {
        homeSelection = (p.y - HOME_LCD_Y0) / HOME_LCD_STEP;
        if (homeSelection >= HOME_COUNT)
          homeSelection = HOME_COUNT - 1;
        input.inject(NavAction::SELECT, InputSource::LCD_BUTTON);
      } else if (state == MUSIC_NOW_PLAYING && p.y >= 166 && p.y < 192 && p.x >= 16 && p.x < 304) {
        // Toque na região do progresso: alterna NORMAL -> SHUFFLE -> REPETIR -> NORMAL.
        if (musicRepeat) {
          musicShuffle = false;
          musicRepeat = false;
        } else if (musicShuffle) {
          musicShuffle = false;
          musicRepeat = true;
        } else {
          musicShuffle = true;
          musicRepeat = false;
        }
        drawMusicNowPlaying();
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
    if (command == "diag fb") {
      Serial.printf("[FB] profundidade=%d bits  %dx%d  bytes=%d\n",
                    (int)(rca.getColorDepth() & 0xFF), rca.width(), rca.height(),
                    rca.width() * rca.height() * ((rca.getColorDepth() & 0xFF) / 8));
      memset(line, 0, sizeof(line));
      continue;
    }
    if (command == "diag cores") {
      // Conta cores RGB565 distintas no framebuffer inteiro. Responde onde a cor
      // se perde: se o valor bater com o do arquivo, o caminho digital está
      // intacto e o que reduz cor é o elo analógico (largura de banda de croma
      // do NTSC), não o MJPEG.
      static uint16_t linha[CRT_W];
      static uint8_t vistos[8192]; // bitmap de 65536 valores
      memset(vistos, 0, sizeof(vistos));
      uint32_t distintas = 0;
      for (int y = 0; y < CRT_H; ++y) {
        rca.readRect(0, y, CRT_W, 1, linha);
        for (int x = 0; x < CRT_W; ++x) {
          const uint16_t c = (uint16_t)((linha[x] >> 8) | (linha[x] << 8)); // readRect troca os bytes
          if (!(vistos[c >> 3] & (1u << (c & 7)))) {
            vistos[c >> 3] |= (uint8_t)(1u << (c & 7));
            ++distintas;
          }
        }
      }
      Serial.printf("[CORES] distintas no framebuffer: %lu de 76800 pixels\n",
                    (unsigned long)distintas);
      memset(line, 0, sizeof(line));
      continue;
    }
    if (command == "diag scan") {
      // Lê o framebuffer de volta e diz em QUE LINHAS existe tinta do ticker
      // (ciano) e tinta clara. Se o texto do ticker aparecer fora da faixa,
      // o defeito está na memória (software); se estiver só na faixa, o que a
      // TV mostra fora dela é artefato de exibição (DMA/varredura).
      static uint16_t linha[CRT_W];
      Serial.println("[SCAN] linha: ciano claro  (faixa do ticker = 206..221)");
      for (int y = 150; y < CRT_H; ++y) {
        rca.readRect(0, y, CRT_W, 1, linha);
        int ciano = 0, claro = 0;
        for (int x = 0; x < CRT_W; ++x) {
          const uint16_t c = linha[x];
          const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
          if (g > 40 && b > 20 && r < 20)
            ++ciano;
          if (r > 20 && g > 40 && b > 20)
            ++claro;
        }
        if (ciano || claro)
          Serial.printf("[SCAN] %3d: %4d %4d\n", y, ciano, claro);
      }
      Serial.printf("[SCAN] tickerFull=%u chars offset=%d wrapAt=%d payload=%u\n",
                    (unsigned)tickerFull.length(), tickerOffset, tickerWrapAt,
                    (unsigned)tickerPayload.length());
      Serial.printf("[SCAN] largura do texto=%d  TICKER_Y=%d TICKER_H=%d\n",
                    rca.textWidth(tickerFull.c_str()), TICKER_Y, TICKER_H);
      for (int y = TICKER_Y - 2; y < TICKER_Y + TICKER_H + 2; ++y) {
        rca.readRect(0, y, CRT_W, 1, linha);
        int distintas = 0;
        uint16_t vistas[6] = {0};
        for (int x = 0; x < CRT_W; ++x) {
          bool nova = true;
          for (int k = 0; k < distintas; ++k)
            if (vistas[k] == linha[x])
              nova = false;
          if (nova && distintas < 6)
            vistas[distintas++] = linha[x];
        }
        Serial.printf("[SCAN] faixa y=%d cores=%d [%04X %04X %04X]\n", y, distintas, vistas[0],
                      vistas[1], vistas[2]);
      }
      memset(line, 0, sizeof(line));
      continue;
    }
    if (command == "diag colors") {
      testPixelColors();
      continue;
    }
    if (command == "diag bench" || command.startsWith("diag bench ")) {
      // "diag bench" mede o programa selecionado na biblioteca; com argumento,
      // mede a pasta indicada. O segundo argumento limita a contagem de quadros
      // (padrao 150, o bastante para a media estabilizar sem prender o aparelho).
      String arg = command.length() > 11 ? command.substring(11) : String();
      arg.trim();
      uint32_t limit = 150;
      const int sp = arg.lastIndexOf(' ');
      if (sp > 0) {
        const long n = arg.substring(sp + 1).toInt();
        if (n > 0) {
          limit = uint32_t(n);
          arg = arg.substring(0, sp);
          arg.trim();
        }
      } else if (arg.length() && arg.toInt() > 0) {
        limit = uint32_t(arg.toInt());
        arg = "";
      }
      String dir = arg.length() ? normalizeSdPath(arg) : libraryProgramAt(librarySelection);
      if (!dir.length())
        Serial.println("[BENCH] nenhum programa selecionado; use: diag bench /M5RETRO/videos/<pasta>");
      else
        runVideoBenchmark(dir, limit);
      memset(line, 0, sizeof(line));
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
      stopProgram(); // já encerra a música do Weather Channel, se estiver ativa
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
    M5.Display.println(PTBR::FALHA_NTSC);
    state = ERROR_SCREEN;
    drawBackButton();
    return;
  }
  // RGB565. O padrão do Panel_CVBS é RGB332, de 256 cores: medido no aparelho,
  // um quadro com 5.584 cores no arquivo chegava ao framebuffer com 74 a 98. E
  // como a JPEGDEC entrega RGB565, cada pixel ainda pagava uma conversão — o
  // blit custava 193 ns/pixel, 46 ciclos a 240 MHz, para o que deveria ser
  // cópia. Com psram_half_use o consumo de SRAM interna continua o mesmo
  // (76.800 B), porque metade das linhas vai para a PSRAM com cache de linha.
  rca.setColorDepth(16);
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
  dualText(PTBR::APP, PTBR::INICIANDO_NTSC);
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
  if (state == FILE_TRANSFER) {
    transferTick();
  }
  if (state == HOME && millis() - lastClockDraw >= 1000)
    drawHomeClock(true);
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
  // Soneca adaptativa. A cadência normal de 4 ms é folgada para 30 quadros/s
  // (33 ms cada) e mantém baixo o polling I2C do touch que o M5.update() faz.
  // Só quando o vídeo já está atrás do relógio de PCM é que 4 ms de latência
  // passam a custar quadro, e aí o loop praticamente não dorme.
  delay(state == VIDEO_PLAYBACK && videoBehind ? 1 : 4);
}