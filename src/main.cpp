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
#include "fj/Platform.h"
#include "fj/Gfx.h"
#include "fj/Board.h"
#include "fj/Display.h"
#include "fj/AudioOut.h"
#include "fj/Net.h"
#include "fj/Storage.h"

#include "SafeArea.h"
#include "Config.h"
#include "BootSplash.h"
#include "RtcClock.h"
#include "ChannelMode.h"
#include "PhotoShow.h"
#include "TestPattern.h"
#include "RadioStream.h"
#include "RadioScreen.h"
#include "Subtitles.h"
#include "BurnIn.h"
#include "VhsFx.h"
#include "AudioScope.h"
#include "VcrOsd.h"
#include "WeatherIcons.h"
#include "ScreenFx.h"
#include "FileTransfer.h"
#include "TransferScreen.h"
#include <SD.h>
#include <WiFiNINA.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include "InputManager.h"
#include "LocalizationPTBR.h"
#include "SecretsManager.h"
#include "ConfigurationPortal.h"
#ifdef FRUITJAM_LAUNCHER_BUILD
// So o env "fruitjam-launcher" grava o watchdog scratch register do
// protocolo com o lancador (ver enterEmulators() em baixo); a build
// standalone nao inclui hardware/watchdog.h nem referencia watchdog_hw.
#include "hardware/watchdog.h"
#endif

// Adafruit Fruit Jam (RP2350B): DVI pelo HSTX, DAC TLV320DAC3100, Wi-Fi no
// ESP32-C6. Fork do m5-retro-tv (M5Stack Core2 + modulo RCA); ver PORTING.md.

// Os nomes e os valores de JSON ficam os do Core2 por compatibilidade com o
// settings.json antigo. No Fruit Jam: RCA = fone P2 do DAC (vai a TV),
// INTERNAL = alto-falante da placa (PORTING.md 3.4).
enum class AudioOutput : uint8_t { RCA, INTERNAL, MUTED };

struct Settings {
  double lat = 0, lon = 0;
  int rangeKm = 250, refreshSeconds = 10, volume = 75;
  bool vhsOsd = true;
  // Era a profundidade do framebuffer do CVBS (RGB565 x RGB332). O DVI do Fruit
  // Jam e sempre RGB565: o campo continua sendo lido e gravado so para o
  // settings.json do Core2 nao perder a chave, e e ignorado (PORTING.md 3.5).
  bool color16 = true;
  vhs::Wear vhsWear = vhs::Wear::Gasta;
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
void handleNavigation(NavAction action);
void drawControllerLabels(const char *left, const char *center, const char *right);
void drawPlaybackOsd();
void setAudioOutput(AudioOutput output);
void servicePowerOff();
void requestPowerOff();
void stopRadio();
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

// Pinos: ver include/fj/Board.h. Vídeo, áudio, cartão e rádio têm barramentos
// próprios no Fruit Jam; nenhuma das disputas de pino do Core2 existe aqui.
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
// HOME_ITEM_COUNT e HOME_ROWS vivem em UiLogic.h, junto de homeTarget() e
// homeHit(), para desenho, navegacao e toque nunca discordarem.
static constexpr int HOME_COUNT = HOME_ITEM_COUNT;
static constexpr int HOME_POWER_OFF = HOME_COUNT - 1;
// Duas colunas: 11 itens em uma coluna so nao cabem. Na TV seriam 70 + 11*16 =
// 246, muito alem da barra em 202. Com 6 linhas para em 166.
static constexpr int HOME_RCA_COL_W = 124;

// Transferência de arquivos por Wi-Fi (include/FileTransfer.h + TransferScreen.h).
xfer::FileTransfer fileTransfer;
static uint32_t lastTransferDraw = 0;
// Relógio do menu inicial. Redesenhado a cada segundo, só na sua faixa — a tela
// inteira não é repintada, senão a TV piscaria uma vez por segundo.
static uint32_t lastClockDraw = 0;
static constexpr int CLOCK_Y = crt::HEAD_RULE_Y + 4; // 50..66, acima dos itens
static xfer::Stage lastTransferStage = xfer::Stage::Error; // força o 1º desenho
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
const char *PHOTOS = "/M5RETRO/fotos";

// O canvas da TV (`tv`) mora em fj/Display.cpp: é um LGFX_Sprite apontado para
// o framebuffer do DVI. No firmware original era o M5ModuleRCA do CVBS.
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
// caso o loop encurta a soneca: fora disso 4 ms sobram para 30 quadros/s e o
// núcleo fica livre para a tarefa de áudio e a rede.
bool videoBehind = false;
uint64_t benchBlitUs = 0;
uint32_t benchBlitCalls = 0;
// Ordem de bytes que o JPEGDEC entrega no quadro corrente. Big-endian é a ordem
// que o LGFX_Sprite guarda (fj/Display.h), então o blit vira cópia; mas o filtro
// de fita (VhsFx.h) faz conta de cor no bloco e espera RGB565 nativo. Por isso
// o formato é escolhido por quadro, e o jpegDraw casa o tipo do ponteiro com
// ele — o LovyanGFX decide a conversão pelo TIPO (AGENTS.md, armadilha 11).
bool videoBigEndian = false;
std::atomic<bool> weatherAudio{false}; // música do Weather Channel em loop, flag de áudio em loop

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
uint32_t lastRadarDraw = 0, lastApiPoll = 0, lastApiGood = 0, lastStats = 0,
         jpegDecodeTotalMs = 0, jpegDecodeMaxMs = 0;
String lastError, apiStatus = "NAO CONFIGURADA";
struct RadarRequest {
  SecretsConfig config;
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
// Modo canal (reproducao continua) e temporizador de desligar. Aqui em cima
// porque startProgram(), bem antes de scanLibrary(), ja precisa deles.
channel::Channel tvChannel;
sleeptimer::SleepTimer sleepTimer;
// Protecao contra queima do tubo. Fosforo de CRT com menu e relogio parados por
// horas marca de verdade, e isso nao volta atras.
burnin::Manager idleMgr;
// Artefatos de fita sobre a reproducao. 972 B de SRAM (tabela de deslocamento
// por linha + um buffer de uma linha). Global, nunca na pilha de tarefa.
vhs::Filter vhsFilter;
// Visualizador de audio. O Tap e escrito pelo audioTask no core 0 e lido pelo
// loop no core 1: a entrega e por seqlock de banco duplo, sem mutex e sem
// espera -- segurar o audioTask para salvar um quadro de analise trocaria um
// defeito invisivel por um audivel. 1.152 B de SRAM no total.
audioscope::Tap audioTap;
audioscope::Scope audioScope;
// Instrumentacao do radio. Tres hipoteses minhas sobre o arrasto falharam
// seguidas; estes contadores medem em vez de deduzir. Quadros entregues por
// segundo contra os 22050 exigidos da o fator de arrasto direto.
std::atomic<uint32_t> radioPcmUs{0}, radioPcmCalls{0}, radioPcmMaxUs{0};
std::atomic<uint32_t> radioWriteUs{0}, radioFramesOut{0}, radioSince{0};
bool musicScopeOn = true;
// Tom de referencia de 1 kHz do padrao de barras. Atomico porque quem liga e o
// loop (core 1) e quem consome e o audioTask (core 0).
std::atomic<bool> toneActive{false};
testpattern::Tone1k testTone;
// 0 = barras SMPTE, 1 = cartaz de encerramento, 2 = chuvisco.
int testPatternPage = 0;
// Radio pela internet. O anel de rede vive na PSRAM (ver RadioStream.h); em
// SRAM ficam so os descritores.
radio::Stream radioStream;
std::atomic<bool> radioActive{false};
// Legendas .srt ao lado do video. A base de tempo e o relogio de AUDIO
// (samplesPlayed/sampleRate), nao millis(): e o audio que manda na sincronia.
subs::Subtitles subtitles;
File srtFile;
bool srtOpen = false;
uint32_t radioLastDraw = 0;
// Cache da repintura parcial da tela do radio. Sem ele, cada passada redesenha
// a tela inteira.
radioui::Cache radioCache;

// Apresentacao de fotos. O catalogo guarda os nomes num pool continuo, com um
// vetor de deslocamentos -- nada de String por foto. 128 nomes de ~20 chars
// cabem em 2560 B; o teto de entradas e o que limita, nao o pool.
// A decodificacao reaproveita o jpegBuffer do video (128 KB em PSRAM): nao ha
// as duas coisas rodando ao mesmo tempo.
// Catalogo de fotos na PSRAM, nao na SRAM. Sao 2.816 B que so servem enquanto
// se navega fotos, e em .bss eles encolhiam a heap -- o que importa para criar
// tarefa nao e o total livre e sim o maior bloco CONTIGUO de 8 bits, e ele
// estava em 8.180 B contra os 8.192 da pilha da WEATHER_HTTP. Doze bytes.
static constexpr size_t PHOTO_POOL_BYTES = 2560;
static constexpr size_t PHOTO_MAX_ENTRIES = 128;
static char *photoPool = nullptr;
static uint16_t *photoOffsets = nullptr;
photo::Show photoShow;
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

void dualText(const String &line1, const String &line2 = "") {
  // Trunca as duas linhas para caber na área segura com a fonte bitmap: size 2
  // tem 12 px por letra, e 20 letras são 240 px dentro dos 272 da caixa segura
  // (cabe "FRUIT JAM RETRO TV"); size 1, 6 px, 44 letras.
  const String l1 = line1.length() > 20 ? line1.substring(0, 20) : line1;
  const String l2 = line2.length() > 44 ? line2.substring(0, 41) + "..." : line2;
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(middle_center);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.setTextSize(2);
  tv.drawString(l1, 160, 94);
  tv.setTextColor(RCA_ACCENT, TFT_NAVY);
  tv.setTextSize(1);
  tv.drawString(l2, 160, 130);
}
// Etapa do boot na abertura (BootSplash.h): a primeira chamada pinta a tela
// inteira com o nome do dono; as seguintes trocam só a linha de status, sem
// piscar. Erro de boot continua saindo por setError()/dualText().
static uint32_t bootSplashSince = 0;
static void bootStep(const char *status) {
  static bool painted = false;
  if (!painted) {
    bootsplash::draw(&tv, cfg::OWNER, PTBR::APP, status, cfg::VERSION_SHORT);
    bootSplashSince = millis();
    painted = true;
  } else
    bootsplash::drawStatus(&tv, status);
}

// Segura a abertura por um mínimo de tempo: com cartão rápido e sem Wi-Fi o boot
// acaba em menos de um segundo, e o nome piscaria sem dar para ler.
static void bootSplashHold() {
  constexpr uint32_t MIN_MS = 2000;
  while (millis() - bootSplashSince < MIN_MS)
    delay(20);
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
  r.color16 = settings.color16;
  r.vhsWear = (int)settings.vhsWear;
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
  settings.color16 = r.color16;
  settings.vhsWear = (vhs::Wear)constrain(r.vhsWear, 0, 3);
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
      // Era configTime(0, 0, ...), que monta TZ="UTC0DST0": o relogio da tela
      // inicial mostrava UTC, 3 h adiantado. Isto pede os mesmos servidores com
      // o fuso de Brasilia (UTC-3, sem horario de verao desde 2019).
      rtcclock::configTimeBrazil("pool.ntp.org", "time.cloudflare.com");
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
    tv.setTextDatum(top_left);
    tv.setTextColor(TFT_WHITE, TFT_NAVY);
    tv.setTextSize(1);
    tv.drawString("SENHA: " + portal.apPassword(), SAFE_L, 144);
    tv.drawString("ABRA: 192.168.4.1", SAFE_L, 162);
    tv.drawString(portal.status() == PortalStatus::CONFIGURANDO ? "CONFIGURACAO ATIVA"
                                                                 : "AGUARDANDO CELULAR...",
                   SAFE_L, 180);
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
  // O ESP32-C6 com NINA não faz ponto de acesso e estação ao mesmo tempo: a
  // conexão com a rede nova só pode começar depois que o portal derrubar o AP.
  // Quem faz isso é o loop(), um instante depois, para o celular ainda receber
  // a página de "salvo" (ver o bloco SETUP_PORTAL do loop).
  networkConfigPresent = true;
  portalSavePending = true;
  portalSaveStarted = millis();
  apiStatus = "CONECTANDO WI-FI";
  dualText("CONFIGURACAO SALVA", "CONECTANDO AO WI-FI...");
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

// Rota do DAC para cada valor do enum herdado do Core2 (PORTING.md 3.4).
static audioout::Route audioRouteFor(AudioOutput output) {
  return output == AudioOutput::RCA        ? audioout::Route::HEADPHONE
         : output == AudioOutput::INTERNAL ? audioout::Route::SPEAKER
                                           : audioout::Route::MUTED;
}

static const char *audioOutputLabel(AudioOutput output) {
  return output == AudioOutput::RCA ? "fone P2 (TV)" : output == AudioOutput::INTERNAL ? "alto-falante" : "mudo";
}

// Espera o ritmo do relógio de amostras depois de entregar `frames` quadros à
// taxa `rate`. O audioout::write() volta assim que o bloco cabe no buffer do
// I2S, ANTES de ele soar; sem esta espera o samplesPlayed correria na frente do
// som pela profundidade do buffer, e o vídeo junto (AGENTS.md 3.5).
static void pacePcm(int64_t &pcmDueUs, uint32_t frames, uint32_t rate) {
  if (!pcmDueUs)
    pcmDueUs = esp_timer_get_time();
  pcmDueUs += ((int64_t)frames * 1000000LL) / rate;
  const int64_t waitUs = pcmDueUs - esp_timer_get_time();
  if (waitUs > 0)
    vTaskDelay(pdMS_TO_TICKS((waitUs + 999) / 1000));
  else if (waitUs < -50000) {
    pcmDueUs = esp_timer_get_time();
    audioUnderruns++;
  }
}

void audioTask(void *) {
  // Um bloco só, na SRAM. O audioout::write() copia o PCM para o buffer do I2S
  // antes de voltar, então acabou o rodízio de três blocos que o M5.Speaker do
  // Core2 exigia (ele guardava o ponteiro até tocar).
  uint8_t *buf = (uint8_t *)malloc(AUDIO_CHUNK);
  if (!buf) {
    audioFailed = true;
    audioIdle = true;
    vTaskDelete(nullptr);
    return;
  }
  int16_t *pcm = reinterpret_cast<int16_t *>(buf);
  AudioOutput active = audioOutput.load();
  audioout::setRoute(audioRouteFor(active));
  audioReady = true;
  // Taxa aplicada por último ao DAC; 0 força a primeira fonte a aplicar a sua.
  uint32_t activeRate = 0;
  bool outputPaused = false;
  int64_t pcmDueUs = 0;
  for (;;) {
    audioIdle = false;
    const AudioOutput wanted = audioOutput.load();
    if (wanted != active) {
      // Fone e alto-falante são saídas do mesmo DAC: trocar é mudar registrador,
      // não desmontar o I2S1 como no Core2. O I2S segue correndo.
      audioout::setRoute(audioRouteFor(wanted));
      Serial.printf("[M5RETRO] Audio: %s\n", audioOutputLabel(wanted));
      active = wanted;
      pcmDueUs = 0;
    }
    // Pausa tem de calar a saída física, não só parar de ler o cartão: o
    // flushSilence() descarta o PCM já enfileirado no I2S.
    // `!wavFile && !mp3Mode` (e não apenas `!wavFile`) porque, no MP3, o arquivo
    // aberto é `mp3File` (wavFile fica nulo) — senão o MP3 nunca tocaria.
    // Rádio e tom não vêm do cartão e por isso não satisfazem `wavFile ||
    // mp3Mode`: sem excluir os dois aqui, a saída era PAUSADA embaixo deles.
    // `paused` vale para TODAS as fontes. No Core2 o rádio pausado caía no
    // caminho do cartão com o arquivo fechado, e o setAudioOutput() esperava
    // um audioIdle que nunca vinha.
    const bool fonteViva = radioActive.load() || toneActive.load();
    if (paused.load() || (!fonteViva && ((!playing && !weatherAudio.load()) || (!wavFile && !mp3Mode)))) {
      if (!outputPaused)
        audioout::flushSilence();
      outputPaused = true;
      pcmDueUs = 0;
      audioIdle = true;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (outputPaused) {
      outputPaused = false;
      pcmDueUs = 0;
    }
    // Rádio, tom e MP3 saem já em 22050 Hz (o MP3 passa pelo reamostrador); o
    // WAV sai na taxa do cabeçalho. Trocar a taxa reprograma o I2S e o PLL do
    // DAC, por isso só acontece quando a fonte muda de taxa.
    const bool doCartao = !fonteViva;
    const uint32_t wantRate = (doCartao && !mp3Mode) ? sampleRate : 22050;
    if (wantRate != activeRate) {
      if (!audioout::setRate(wantRate)) {
        Serial.printf("[M5RETRO] ERRO: DAC recusou %lu Hz\n", (unsigned long)wantRate);
        activeRate = 0; // a próxima fonte tenta de novo
        if (doCartao) {
          audioStreamError = true;
          playbackFinished = true;
          playing = false;
          weatherAudio = false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      activeRate = wantRate;
      pcmDueUs = 0;
    }
    // Rádio: como o tom, não vem do cartão, então não toma o sdMutex. readPcm
    // devolve 0 enquanto o anel não tem pré-buffer; aí é só não escrever nada e
    // voltar, que o I2S repete o silêncio em vez de estalar.
    if (radioActive.load()) {
      const int64_t tDec = esp_timer_get_time();
      const size_t amostras = radioStream.readPcm(pcm, AUDIO_CHUNK / sizeof(int16_t));
      {
        const uint32_t us = (uint32_t)(esp_timer_get_time() - tDec);
        radioPcmUs.fetch_add(us);
        radioPcmCalls.fetch_add(1);
        if (us > radioPcmMaxUs.load())
          radioPcmMaxUs.store(us);
        radioFramesOut.fetch_add(amostras / 2);
      }
      if (!amostras) {
        // Anel vazio: nada foi entregue, então não há o que pacear. Rebasear o
        // relógio é essencial -- dormindo sem mexer no pcmDueUs, o déficit se
        // acumula, cruza os 50 ms e conta underruns falsos.
        pcmDueUs = 0;
        // 1 tick (a granularidade mínima do FreeRTOS): cada espera destas é
        // tempo em que nada sai para o I2S.
        vTaskDelay(1);
        continue;
      }
      playback::scalePcm(pcm, amostras, playbackVolume.load());
      audioTap.publish(pcm, amostras, 2);
      const int64_t tW = esp_timer_get_time();
      audioout::write(pcm, amostras / 2);
      radioWriteUs.fetch_add((uint32_t)(esp_timer_get_time() - tW));
      // Sem o ritmo o laço gira o mais rápido que a prioridade 4 deixa e mata de
      // fome as tarefas de menor prioridade (no Core2, o cão de guarda
      // reiniciava o aparelho logo depois de "Radio: no ar").
      pacePcm(pcmDueUs, amostras / 2, 22050);
      continue;
    }
    if (toneActive.load()) {
      const size_t amostras = testTone.fillPcm(pcm, AUDIO_CHUNK / sizeof(int16_t));
      playback::scalePcm(pcm, amostras, playbackVolume.load());
      audioTap.publish(pcm, amostras, 2);
      audioout::write(pcm, amostras / 2);
      pacePcm(pcmDueUs, amostras / 2, 22050);
      continue;
    }
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      audioUnderruns++;
      continue;
    }
    size_t bytes;
    if (mp3Mode) {
      // MP3: decodifica para PCM 22050 Hz estéreo (resampler linear).
      const size_t samps = mp3ReadPcm(pcm, AUDIO_CHUNK / 2);
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
    playback::scalePcm(pcm, bytes / sizeof(int16_t), playbackVolume.load());
    audioTap.publish(pcm, bytes / sizeof(int16_t), wavChannels);
    // O readWav() só aceita PCM 16 bits estéreo, então um bloco do arquivo já é
    // o formato intercalado L, R que o audioout::write() recebe.
    const size_t frames = bytes / wavBlockAlign;
    const size_t delivered = audioout::write(pcm, frames);
    if (delivered != frames) {
      audioUnderruns++;
      audioStreamError = true;
      playbackFinished = true;
      playing = false;
    }
    // Relógio do player pelo PCM ENTREGUE, não por millis(): é ele que dita o
    // quadro alvo no videoTick() (AGENTS.md 3.5).
    pacePcm(pcmDueUs, delivered, sampleRate);
    // O relógio só avança depois que o bloco teve tempo de sair: bloco apenas
    // enfileirado não pode fazer o contador e o vídeo pularem.
    if (playing)
      samplesPlayed.fetch_add(delivered);
  }
}

// Escolhe a ordem de bytes do próximo decode (ver videoBigEndian). Chamar entre
// o jpeg.openRAM() e o jpeg.decode(): o jpegDraw lê a mesma flag, então formato
// entregue e tipo do ponteiro nunca discordam.
static void selectVideoPixelType() {
  videoBigEndian = !vhsFilter.config().enabled;
  jpeg.setPixelType(videoBigEndian ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);
}

int jpegDraw(JPEGDRAW *draw) {
  // Escreve direto no `tv`, que é o próprio framebuffer do DVI: não há cópia
  // depois, nem espelho em outra tela.
  const int64_t t0 = benchActive ? esp_timer_get_time() : 0;
  const int x = draw->x + (CRT_W - videoWidth) / 2, y = draw->y + (CRT_H - videoHeight) / 2;
  if (videoBigEndian) {
    // Filtro de fita desligado: o bloco já vem na ordem do sprite, e o tipo
    // swap565_t diz isso ao LovyanGFX, que então só copia.
    tv.pushImage(x, y, draw->iWidth, draw->iHeight,
                 reinterpret_cast<const lgfx::swap565_t *>(draw->pPixels));
  } else {
    // O filtro de fita fica DENTRO da medição do bench de propósito: o custo
    // dele é custo de blit, e esconder isso faria o diag bench mentir.
    vhsFilter.pushBlock(&tv, x, y, draw->iWidth, draw->iHeight, draw->pPixels);
  }
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

// Lê o próximo quadro do cartão. Com render=false só pula o quadro (catch-up do
// videoTick, sem tocar no jpegBuffer); com render=true copia o JPEG para o
// jpegBuffer e devolve o tamanho em `used`. Roda sempre no loop(): o cartão é
// disputado com a tarefa de áudio pelo sdMutex.
static bool readVideoFrame(bool render, size_t &used) {
  used = 0;
  if (!mjpegFile || !jpegBuffer)
    return false;
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
  return true;
}

// Decodifica o JPEG que está no jpegBuffer direto no framebuffer. Não mexe em
// contador nenhum que o loop() leia: devolve o sucesso, e quem publica o quadro
// é finishVideoFrame(). Por isso pode rodar tanto no loop() quanto na tarefa de
// decode do núcleo 1 (ver videoDecodeTask).
static bool decodeVideoFrame(size_t used, bool firstFrame) {
  if (!jpeg.openRAM(jpegBuffer, used, jpegDraw))
    return false;
  const int width = jpeg.getWidth(), height = jpeg.getHeight();
  if (width < 1 || height < 1 || width > 320 || height > 240) {
    jpeg.close();
    return false;
  }
  // Limpa a tela no primeiro quadro (ou quando a resolução muda), para o
  // letterbox em volta de um vídeo menor que 320x240 começar preto.
  if (width != videoWidth || height != videoHeight || firstFrame) {
    videoWidth = width;
    videoHeight = height;
    tv.fillScreen(TFT_BLACK);
  }
  selectVideoPixelType();
  // Sorteia os artefatos deste quadro antes de decodificar: o pushBlock consulta
  // a tabela de deslocamento por linha enquanto os blocos chegam.
  vhsFilter.beginFrame(millis(), (CRT_W - videoWidth) / 2, (CRT_H - videoHeight) / 2, videoWidth,
                       videoHeight);
  const bool decoded = jpeg.decode(0, 0, 0);
  jpeg.close();
  if (decoded)
    vhsFilter.drawOverlay(&tv); // faixa de troca de cabeca e banda de tracking
  return decoded;
}

// Publica o resultado de um decode (no loop(), sempre). Só depois de um decode
// bem-sucedido o par (jpegBuffer, tamanho) está comprovadamente coerente, que é
// a pré-condição do redrawCurrentFrame().
static bool finishVideoFrame(bool decoded, size_t used, uint32_t elapsedMs) {
  if (!decoded) {
    lastJpegUsed = 0;
    jpegErrors++;
    videoReadError = true;
    return false;
  }
  lastJpegUsed = used;
  decodedFrames++;
  renderedFrames++;
  ++renderedSeq;
  jpegDecodeTotalMs += elapsedMs;
  jpegDecodeMaxMs = max(jpegDecodeMaxMs, elapsedMs);
  return true;
}

// Caminho síncrono: lê e decodifica no próprio loop(). Serve ao primeiro quadro
// do startProgram() e ao descarte de quadros (render=false). O ritmo normal do
// filme passa pela tarefa do núcleo 1 (videoTick).
bool readAndShowOneFrame(bool render) {
  size_t used = 0;
  if (!readVideoFrame(render, used))
    return false;
  if (!render)
    return true;
  const uint32_t started = millis();
  const bool decoded = decodeVideoFrame(used, videoFrameIndex == 0);
  return finishVideoFrame(decoded, used, millis() - started);
}

// ============================================================================
// Decode no núcleo 1
//
// No Fruit Jam o núcleo 0 carrega o loop() e a interrupção de linha do DVI (que
// copia o framebuffer para os buffers de linha, ~10% do núcleo), e o núcleo 1
// só tem o áudio (~1%). Um quadro MJPEG 320x240 custa ~31-37 ms estimados
// (docs/codecs-video-fruitjam.md): no núcleo 0 isso fica no limite dos 30 fps e
// ainda trava botões e OSD enquanto decodifica. Aqui o loop() lê o quadro do
// cartão e entrega o decode a uma tarefa no núcleo 1, com prioridade ABAIXO da
// do áudio (o áudio é quem dita o relógio do filme).
//
// Regra que mantém isto correto com um framebuffer só: enquanto um decode está
// em voo, o loop() NÃO desenha no `tv` nem usa o `jpeg`. Quem precisa desenhar
// durante o vídeo (OSD, legenda, vinheta, parada) chama waitVideoDecodeIdle()
// antes. O loop() é o único que submete trabalho, então depois dessa espera
// ninguém mais decodifica até ele mesmo submeter de novo.
// ============================================================================
enum class DecodeJob : uint8_t { IDLE, BUSY, DONE };
static std::atomic<DecodeJob> decodeJob{DecodeJob::IDLE};
static TaskHandle_t videoDecodeHandle = nullptr;
// Parâmetros e resultado do job. Escritos antes do store(BUSY)/store(DONE) e
// lidos depois do load() correspondente: o atomic (seq_cst) publica os dois lados.
static size_t decodeUsed = 0;
static bool decodeFirst = false, decodeOk = false;
static uint32_t decodeElapsedMs = 0;
static constexpr uint32_t DECODE_STACK_BYTES = 8192;
static constexpr UBaseType_t DECODE_PRIO = 2; // áudio é 4, rede 0-1

static void videoDecodeTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (decodeJob.load() != DecodeJob::BUSY)
      continue;
    const uint32_t started = millis();
    decodeOk = decodeVideoFrame(decodeUsed, decodeFirst);
    decodeElapsedMs = millis() - started;
    decodeJob.store(DecodeJob::DONE);
  }
}

static bool startVideoDecodeTask() {
  if (videoDecodeHandle)
    return true;
  return xTaskCreatePinnedToCore(videoDecodeTask, "VIDEO_DEC", DECODE_STACK_BYTES, nullptr,
                                 DECODE_PRIO, &videoDecodeHandle, 1) == pdPASS;
}

static bool videoDecodeBusy() { return decodeJob.load() == DecodeJob::BUSY; }


// Lê o quadro e entrega o decode ao núcleo 1. Sem a tarefa (falha de memória no
// boot) cai no caminho síncrono, que é o comportamento do upstream.
static bool submitVideoFrame() {
  if (!videoDecodeHandle)
    return readAndShowOneFrame(true);
  size_t used = 0;
  if (!readVideoFrame(true, used))
    return false;
  decodeUsed = used;
  decodeFirst = videoFrameIndex == 0;
  decodeJob.store(DecodeJob::BUSY);
  xTaskNotifyGive(videoDecodeHandle);
  return true;
}

// Colhe um decode terminado e publica o quadro (renderedSeq, lastJpegUsed,
// estatísticas). Quadro que não decodificou encerra o filme, como no caminho
// síncrono. Devolve true se havia um quadro para colher.
static bool collectVideoFrame() {
  if (decodeJob.load() != DecodeJob::DONE)
    return false;
  const bool ok = finishVideoFrame(decodeOk, decodeUsed, decodeElapsedMs);
  decodeJob.store(DecodeJob::IDLE);
  if (!ok) {
    playbackFinished = true;
    playing = false;
  }
  return true;
}

// Espera o decode em voo terminar e já publica o quadro. Depois disto ninguém
// escreve no `tv` nem usa o `jpeg` até o loop() submeter de novo, e o
// lastJpegUsed volta a descrever o que está no jpegBuffer — sem colher, um
// redrawCurrentFrame() logo depois decodificaria o quadro novo com o tamanho do
// velho.
static void waitVideoDecodeIdle() {
  while (decodeJob.load() == DecodeJob::BUSY)
    vTaskDelay(1);
  collectVideoFrame();
}

// ============================================================================
// Benchmark do caminho de vídeo ("diag bench")
//
// Responde na bancada a pergunta que nenhum simulador de PC responde: até que
// resolução e taxa ESTE aparelho sustenta. Mede as três etapas separadamente,
// em microssegundos (millis() tem resolução grossa demais para um quadro de
// ~20 ms), rodando o mais rápido possível, sem cadência de áudio e sem descarte
// de quadros:
//
//   leitura   -> tirar o JPEG do cartão (mjpegReader.next)
//   decode    -> JPEGDEC, já descontado o blit
//   blit      -> pushImage no framebuffer do DVI, cronometrado dentro do jpegDraw
//
// O DVI varre 60 quadros/s, mas o conteúdo foi pensado para TV: acima de
// 320x240 a 30 quadros/s não há o que ganhar. Este benchmark diz se o RP2350
// alcança esse teto com a mídia que você preparou.
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

  // Reaproveita o leitor global: um MjpegReader local são 4104 bytes, e mesmo
  // com esta função rodando na pilha de 16 KB da tarefa APP (não a CORE0 de
  // 4 KB do arduino-pico, que só chama setup()/loop() e dorme — ver o
  // xTaskCreatePinnedToCore(appTask, ...) no fim deste arquivo), não vale a
  // pena mais um array desse tamanho ao lado do FAT, do deserializeJson e dos
  // printf com float do relatório. O stopProgram() acima já resetou o leitor.
  playback::MjpegReader &reader = mjpegReader;
  reader.reset();
  tv.fillScreen(TFT_BLACK);
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
    selectVideoPixelType();
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
  Serial.printf("[BENCH] blit DVI       : medio %lu us   (%lu blocos, %s)\n", avgBlit, benchBlitCalls,
                videoBigEndian ? "copia direta" : "filtro de fita");
  Serial.printf("[BENCH] quadro inteiro : medio %lu us   pior caso %lu us\n", avgTotal, maxTotalUs);
  Serial.printf("[BENCH] FPS sustentado : %.1f   pior caso %.1f\n", sustainedFps, peakFps);
  // O DVI varre 60 quadros/s; acima disso o monitor nao tem como mostrar.
  const float ceiling = min(sustainedFps, 60.0f);
  Serial.printf("[BENCH] teto util      : %.1f fps (limitado por %s)\n", ceiling,
                sustainedFps < 60.0f ? "decodificacao/cartao" : "varredura do DVI");
  if (sustainedFps < metaFps)
    Serial.printf("[BENCH] ATENCAO: abaixo dos %.2f fps do meta.json; havera descarte de quadros\n",
                  metaFps);
  Serial.println("[BENCH] ----------------------------------------");

  // stopProgram() já levou o estado para VIDEO_LIBRARY; sem redesenhar, a TV
  // ficava com o último quadro do teste e os botões passavam a agir como se
  // estivessem na biblioteca.
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
  tv.fillScreen(TFT_BLACK);
  osdUntil = millis() + 3000;
  Serial.printf("[M5RETRO] Reproduzindo: %s video:%lu bytes wav:%lu bytes\n", dir.c_str(),
                (unsigned long)mjpegFile.size(), (unsigned long)(wavDataEnd - wavDataStart));
  if (!readAndShowOneFrame()) {
    stopProgram();
    return false;
  }
  videoFrameIndex = 1;
  {
    // Legenda opcional: video.mjpeg -> video.srt, na mesma pasta. Ausente e o
    // caso normal e nao e erro.
    char caminho[160];
    const String base = dir + "/" + video;
    subtitles.reset();
    srtOpen = false;
    if (subs::makeSrtPath(base.c_str(), caminho, sizeof(caminho)) && SD.exists(caminho)) {
      srtFile = SD.open(caminho, FILE_READ);
      srtOpen = (bool)srtFile;
      Serial.printf("[M5RETRO] Legenda: %s\n", srtOpen ? caminho : "falhou ao abrir");
    }
  }
  playing = true;
  // Registro do canal DEPOIS de playing: startProgram() comeca chamando
  // stopProgram(), entao registrar antes seria apagado. Pelo mesmo motivo
  // stopProgram() nao limpa o canal -- cancelaria a corrente recem agendada.
  tvChannel.started(librarySelection, millis());
  vhsFilter.configure(vhs::presetFor(settings.vhsWear));
  vhsFilter.begin(0xC0FFEE);
  return true;
}
void stopProgram() {
  playing = false;
  // O núcleo 1 pode estar no meio de um quadro, escrevendo no framebuffer que a
  // próxima tela vai desenhar.
  waitVideoDecodeIdle();
  if (srtOpen) {
    srtFile.close();
    srtOpen = false;
  }
  subtitles.reset();
  // A música em loop do Weather Channel também segura o áudio: se ela continuar
  // ligada, a condição de ocioso do audioTask nunca vale e a espera abaixo não
  // termina nunca. Tem que cair ANTES do while, não depois.
  weatherAudio = false;
  // Espera o consumidor confirmar ocioso antes de fechar ou trocar arquivos.
  // Rádio e tom também seguram o áudio fora do ocioso, mas não tocam em arquivo
  // nenhum: esperar por eles aqui congelava o aparelho (mesma armadilha 3 da
  // música do Weather), e um desligamento vindo da tela do rádio nunca voltava.
  while (audioReady && !audioIdle && !radioActive.load() && !toneActive.load())
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
  // Decode em voo no núcleo 1: nada a fazer até ele terminar. O loop() segue
  // livre para botões e rede.
  if (videoDecodeBusy()) {
    // Soneca curta: o quadro termina a qualquer momento, e cada ms que ele
    // espera para ser colhido e o próximo ser submetido sai do orçamento de
    // 33 ms do quadro seguinte.
    videoBehind = true;
    return;
  }
  // Quadro recém-decodificado: publica e devolve a vez ao loop(), para o OSD e a
  // legenda pintarem por cima ANTES do próximo decode começar a escrever no
  // mesmo framebuffer.
  const bool fresh = collectVideoFrame();
  if (!playing || paused)
    return;
  const uint32_t target = uint32_t((double(samplesPlayed.load()) * fps) / sampleRate);
  if (!fresh) {
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
      if (submitVideoFrame())
        videoFrameIndex++;
      else {
        playbackFinished = true;
        playing = false;
      }
    }
  }
  // Legendas por cima do quadro recem desenhado. A posicao vem do relogio de
  // audio; a leitura do .srt e sequencial e precisa do mutex do cartao, como
  // todo acesso ao SD. Só com quadro recém-colhido: é o único momento em que o
  // núcleo 1 não está escrevendo no framebuffer.
  if (fresh && srtOpen && playing) {
    const uint32_t posMs =
        sampleRate ? (uint32_t)((uint64_t)samplesPlayed.load() * 1000ULL / sampleRate) : 0;
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      subtitles.update(srtFile, posMs);
      xSemaphoreGive(sdMutex);
    }
    subtitles.draw(&tv, 0, 0, posMs);
  }
  // Ainda atrás do relógio, ou com um decode em voo: o loop não deve dormir 4 ms.
  videoBehind = playing && (videoDecodeBusy() || videoFrameIndex < target);
}

// Texto de API vai para a tela: passa por ascii::normalize aqui, na única porta
// de entrada, para que callsign, ICAO e modelo nunca cheguem com acento (um
// UTF-8 acentuado são dois bytes sem glifo, ver AGENTS.md 2.7). 48 bytes cobrem
// com folga qualquer um dos três campos; mais que isso nem cabe no painel.
static String asciiField(const char *src) {
  char buf[48];
  ascii::normalize(buf, sizeof(buf), src ? src : "");
  return String(buf);
}
String stringAlias(JsonObject o, const char *a, const char *b = nullptr, const char *c = nullptr) {
  if (o[a].is<const char *>())
    return asciiField(o[a].as<const char *>());
  if (b && o[b].is<const char *>())
    return asciiField(o[b].as<const char *>());
  if (c && o[c].is<const char *>())
    return asciiField(o[c].as<const char *>());
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

// Teto do corpo da resposta do radar. A API em uso é a do MeuLabApp
// (/api/adsb/aircraft): na bancada de 20/09/2026 ela devolveu 22 aeronaves.
// Cada item tem ~10 campos (icao, callsign, lat, lon, altitude_ft, speed_kt,
// heading, model...), algo entre 250 e 700 bytes conforme o provedor — o
// aircraft.json do readsb é o mais verboso. O parser só aproveita 64
// aeronaves, e 64 x ~700 B dão ~45 KB. 64 KiB é o mesmo limite que o firmware
// original já impunha pelo Content-Length ("RESPOSTA MUITO GRANDE"), então o
// contrato com a API não muda. Na PSRAM, alocado só durante a consulta.
static constexpr size_t RADAR_BODY_CAP = 64 * 1024;

// Pilha das tarefas de rede. No Core2 eram 8 KB porque o handshake mbedTLS
// rodava na própria tarefa (4 a 6 KB só nele, AGENTS.md 2.3). No Fruit Jam o
// TLS roda DENTRO do ESP32-C6: o RP2350 só troca comandos SPI com o NINA, e o
// net::httpGet() promete caber em 4 KB. O que sobra por cima é nosso:
//  - deserializeJson é recursivo; com NestingLimit(12) são ~12 quadros de
//    ~100 B, ~1,2 KB no pior caso;
//  - Serial.printf passa pelo vsnprintf do newlib, ~1 KB de pilha.
// 6 KB cobrem 4 + 1,2 + 1 com margem pequena mas real, e devolvem 2 KB de SRAM
// por tarefa em relação aos 8 KB antigos. Não medido no aparelho: a tarefa
// imprime a marca d'água da pilha no serial para que o primeiro teste diga se
// 6 KB é folga ou aperto.
static constexpr uint32_t NET_TASK_STACK = 6144;

// Alocador do ArduinoJson na PSRAM. O documento do radar copia cada string da
// resposta para o próprio pool (a versão 7 não tem mais modo zero-copy); com
// 64 aeronaves isso chega a dezenas de KB, que na SRAM competiriam com as
// pilhas e com o framebuffer do DVI. O free()/realloc() do arduino-pico
// reconhecem ponteiro de PSRAM e devolvem ao heap certo.
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override { return ps_malloc(size); }
  void deallocate(void *ptr) override { free(ptr); }
  void *reallocate(void *ptr, size_t size) override {
    // realloc(nullptr, n) cairia no malloc da SRAM; o primeiro bloco precisa
    // nascer na PSRAM para que os seguintes o acompanhem.
    return ptr ? realloc(ptr, size) : ps_malloc(size);
  }
};
static PsramJsonAllocator psramJsonAllocator;

void radarNetworkTask(void *argument) {
  auto *request = static_cast<RadarRequest *>(argument);
  auto *result = new RadarResponse;
  // O operator= do JsonDocument troca (swap) com o temporário, allocator
  // inclusive: daqui em diante o pool deste documento mora na PSRAM.
  result->data = JsonDocument(&psramJsonAllocator);
  const uint32_t started = millis();
  const SecretsConfig &c = request->config;
  // Cabeçalho de autenticação montado aqui, com "\r\n" no fim, no formato que
  // o net::httpGet() espera. O portal já recusa CR/LF nos campos.
  String headers;
  if (c.authMode == "bearer")
    headers = "Authorization: Bearer " + c.token + "\r\n";
  else if (c.authMode == "x-api-key")
    headers = "X-API-Key: " + c.token + "\r\n";
  else if (c.authMode == "custom-header" && c.authHeader.length())
    headers = c.authHeader + ": " + c.authPrefix + c.token + "\r\n";
  const String url = c.baseUrl + c.endpoint;

  // Buffer do corpo na PSRAM, nunca na pilha: 64 KiB não cabem em pilha
  // nenhuma deste aparelho.
  char *body = (char *)ps_malloc(RADAR_BODY_CAP);
  if (!body) {
    result->status = "SEM MEMORIA PARA API";
  } else {
    // 12 s de teto: o handshake TLS agora é feito pelo ESP32-C6 e é mais lento
    // que no ESP32 nativo; a bancada do Core2 já mediu 9,5 s ponta a ponta.
    const net::HttpResult r =
        net::httpGet(url.c_str(), body, RADAR_BODY_CAP, 12000, headers.length() ? headers.c_str() : nullptr);
    result->httpCode = r.status;
    if (r.status == 200) {
      if (r.truncated)
        result->status = "RESPOSTA MUITO GRANDE";
      else {
        auto error = deserializeJson(result->data, body, r.length, DeserializationOption::NestingLimit(12));
        result->parsed = !error;
        result->status = error ? "RESPOSTA INVALIDA" : "CONECTADA";
      }
    } else if (r.status == 401 || r.status == 403)
      result->status = "ERRO AUTENTICACAO";
    else
      result->status = "SEM SERVIDOR";
    free(body);
  }
  result->elapsedMs = millis() - started;
  delete request;
  Serial.printf("[RADAR] pilha livre minima: %u B de %u\n",
                (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                (unsigned)NET_TASK_STACK);
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
  // Backoff: lastApiPoll avança em TODA tentativa (aqui e na chegada da
  // resposta), com sucesso ou não. Um portão que só andasse no sucesso viraria
  // laço apertado criando tarefas quando a rede cai (AGENTS.md, armadilha 4).
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
  // Sem ca.pem e sem portão de hora: o TLS roda no ESP32-C6, que valida o
  // certificado com o bundle de raízes do próprio firmware NINA e tem relógio
  // próprio. O ca.pem do cartão e o allow_insecure_tls deixaram de ter efeito
  // (continuam no JSON por compatibilidade). A API precisa, portanto, de um
  // certificado emitido por uma CA pública que o NINA conheça.
  auto *request = new RadarRequest;
  request->config = currentSecrets();
  radarBusy = true;
  apiStatus = "CONSULTANDO";
  // Fora do núcleo 0, onde ficam a interrupção de linha do DVI e a tarefa de
  // áudio. Prioridade ociosa: a espera pelo ESP32-C6 é longa e não deve roubar
  // tempo de ninguém; o loop() aplica a resposta.
  if (xTaskCreatePinnedToCore(radarNetworkTask, "RADAR_HTTPS", NET_TASK_STACK, request, 0, nullptr, 1) !=
      pdPASS) {
    radarBusy = false;
    delete request;
    apiStatus = "SEM MEMORIA PARA API";
  }
}

String normalizeSdPath(const String &path) {
  // No ESP32 as entradas de diretório podiam vir com o prefixo interno do VFS
  // (/sd). No arduino-pico o File::name() devolve só o nome e o fullName() já é
  // o caminho do cartão, então isto não dispara aqui. Fica porque é barato e
  // porque o mesmo caminho pode chegar de fora (serial, controle web) escrito
  // no formato antigo.
  if (path.startsWith("/sd/"))
    return path.substring(3);
  if (path == "/sd")
    return "/";
  return path;
}

String libraryChildPath(const String &root, const String &entryName) {
  // A iteração de diretório do FAT devolve só o nome do filho ("primeiro-teste"),
  // não o caminho absoluto: no arduino-pico o File::name() corta tudo até a
  // última barra, como no ESP32 2.x. Por isso o caminho do cartão é sempre
  // reconstruído a partir da raiz que está sendo varrida.
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

void scanLibrary(); // definida logo abaixo; channelPathAt() precisa dela antes

// Modo canal e temporizador de desligar.
//
// O adaptador aponta para libraryPaths[i] de proposito, e NAO para
// libraryProgramAt(), que devolve String por valor: um .c_str() naquele
// temporario morreria no fim da expressao. libraryPaths[] vive ate o proximo
// scanLibrary().
static const char *channelPathAt(int i, void *) {
  if (!libraryScanned)
    scanLibrary();
  return (i >= 0 && i < libraryCount) ? libraryPaths[i].c_str() : nullptr;
}

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
  tvChannel.setCount(libraryCount);
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

// Apaga, dentro da faixa do OSD, só o que fica FORA do retângulo do vídeo (o
// letterbox preto). A parte sobre a imagem é restaurada pelo próximo quadro,
// ou por redrawCurrentFrame() com o vídeo pausado — assim o OSD some sem
// piscar uma tarja preta sobre o filme.
static void clearOsdLetterbox() {
  const int top = vcr::layout::kTopY;
  const int vx = (CRT_W - videoWidth) / 2, vy = (CRT_H - videoHeight) / 2;
  const int vr = vx + videoWidth, vb = vy + videoHeight;
  if (vb <= top || vy >= CRT_H) {
    tv.fillRect(0, top, CRT_W, CRT_H - top, TFT_BLACK);
    return;
  }
  const int y0 = max(top, vy);
  if (y0 > top)
    tv.fillRect(0, top, CRT_W, y0 - top, TFT_BLACK);
  if (vb < CRT_H)
    tv.fillRect(0, vb, CRT_W, CRT_H - vb, TFT_BLACK);
  const int h = min(vb, CRT_H) - y0;
  if (vx > 0)
    tv.fillRect(0, y0, vx, h, TFT_BLACK);
  if (vr < CRT_W)
    tv.fillRect(vr, y0, CRT_W - vr, h, TFT_BLACK);
}

// Redecodifica o quadro atual a partir do jpegBuffer (sem tocar no cartão).
// Custa um decode e só roda com o vídeo pausado, a cada meio segundo, para o
// PAUSE piscar sobre a imagem parada.
static bool redrawCurrentFrame() {
  waitVideoDecodeIdle(); // o jpeg e o framebuffer são do núcleo 1 durante um decode
  if (!jpegBuffer || !lastJpegUsed)
    return false;
  if (!jpeg.openRAM(jpegBuffer, lastJpegUsed, jpegDraw))
    return false;
  selectVideoPixelType();
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
    waitVideoDecodeIdle();
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
  // Vai pintar: nada de decode em voo por baixo. A espera também colhe um quadro
  // que tenha acabado, e aí ele conta como quadro novo (imagem limpa embaixo).
  waitVideoDecodeIdle();
  const bool newFrameNow = lastFrame != renderedSeq;
  if (!newFrameNow) {
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
  vcr::draw(&tv, st);
}

void setAudioOutput(AudioOutput output) {
  // Deixa o bloco de PCM corrente terminar antes de gravar no cartão, que o
  // audioTask disputa pelo sdMutex. Com o áudio pausado a posição no arquivo e
  // o relógio do vídeo ficam parados durante a gravação; a troca de rota em si
  // é só um registrador do DAC, feita pelo audioTask na volta seguinte.
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

// DESLIGAR. O Fruit Jam não tem PMIC para cortar a própria alimentação (o Core2
// tinha o AXP192), então é um desligamento suave: encerra o que estiver
// tocando, derruba o Wi-Fi, cala o DAC, apaga a tela e dorme até um botão, que
// reinicia a placa (PORTING.md 3.2). Os NeoPixels não precisam ser apagados:
// o firmware nunca os acende. Usado pelo item DESLIGAR do menu inicial e pelo
// temporizador de desligar.
void requestPowerOff() {
  if (powerOffPending)
    return;
  powerOffPending = true;
  powerOffAt = millis() + 120;
  // Rádio, tom e música do Weather seguram o áudio fora do ocioso: caem antes
  // de qualquer espera por audioIdle (AGENTS.md, armadilha 3).
  if (radioActive.load())
    stopRadio();
  toneActive = false;
  weatherAudio = false;
  playing = false;
  paused = true;
  waitVideoDecodeIdle(); // o dualText abaixo não pode dividir a tela com um decode
  if (portal.active())
    portal.stop();
  network.disconnect();
  Serial.println("[M5RETRO] Desligamento solicitado");
  dualText("DESLIGANDO...", "APERTE UM BOTAO PARA LIGAR");
}

// Conclui o desligamento pedido por requestPowerOff(). Os 120 ms de folga deixam
// a mensagem aparecer e o audioTask ver a pausa. Daqui não se volta: o botão
// que acorda a placa chama rp2040.reboot(), e o boot refaz tudo do zero, na
// ordem certa do DAC e do rádio (PORTING.md 2.1).
void servicePowerOff() {
  if (!powerOffPending || !timeReached(millis(), powerOffAt))
    return;
  stopProgram();
  audioout::setRoute(audioout::Route::MUTED);
  audioout::flushSilence();
  tv.fillScreen(TFT_BLACK);
  Serial.println("[M5RETRO] Desligamento suave: tela preta, audio mudo, Wi-Fi fora");
  Serial.flush();
  // Primeiro espera soltar o botão que escolheu DESLIGAR, senão ele mesmo
  // religaria a placa na hora. O delay() do FreeRTOS entrega o núcleo à tarefa
  // ociosa, que dorme em WFI: é esse o "dormir" possível sem PMIC.
  while (InputManager::anyDown())
    delay(20);
  delay(50); // repique do contato ao soltar
  while (!InputManager::anyDown())
    delay(20);
  // Reinicia só depois de SOLTAR: o botão 1 do Fruit Jam é também o BOOT, e
  // reiniciar com ele apertado pode cair no modo de gravação (drive RP2350)
  // em vez de subir o firmware.
  while (InputManager::anyDown())
    delay(20);
  Serial.println("[M5RETRO] Botao apertado: reiniciando");
  Serial.flush();
  rp2040.reboot();
}

void drawControllerLabels(const char *left, const char *center, const char *right) {
  // Legenda dos três botões na faixa de baixo da TV. Antes isto dependia do
  // osdUntil do player, então a barra sumia sozinha em telas que nada têm a ver
  // com reprodução — e no boot nem aparecia. Durante o playback quem manda é o
  // OSD de videocassete, que tem legendas próprias.
  if (state != VIDEO_PLAYBACK) {
    tv.fillRect(0, BAR_Y, CRT_W, BAR_H, TFT_NAVY);
    tv.setTextDatum(middle_center);
    tv.setTextSize(1);
    tv.setTextColor(RCA_ACCENT, TFT_NAVY);
    tv.drawString(String("[ ") + left + " ]  [ " + center + " ]  [ " + right + " ]", CRT_W / 2,
                  BAR_Y + BAR_H / 2);
  }
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
  // Só a TV: o Fruit Jam não tem tela local.
  GfxTarget *d = &tv;
  d->fillScreen(TFT_NAVY);
  d->setTextDatum(top_left);
  d->setTextColor(TFT_WHITE, TFT_NAVY);
  d->setTextSize(2);
  d->drawString(PTBR::MUSICA, SAFE_L, HEAD_Y);
  d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  // Breadcrumb do diretório corrente. O caminho é nome de pasta do cartão, e
  // pode ter acento: sem normalizar, cada letra acentuada vira dois buracos na
  // fonte bitmap (armadilha 9 do AGENTS.md).
  String crumb = musicDir;
  crumb.replace(String(MUSIC_ROOT), "/");
  char crumbAscii[64];
  ascii::normalize(crumbAscii, sizeof(crumbAscii), crumb.c_str());
  d->setTextSize(1);
  d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
  d->drawString(crumbAscii, SAFE_L, HEAD_RULE_Y + 6);
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
  drawControllerLabels("ACIMA", "OK", "ABAIXO");
}

// O JPEGDEC entrega RGB565_BIG_ENDIAN (ver loadAlbumCover), que é exatamente
// como o LGFX_Sprite de 16 bits guarda o pixel. Declarar os blocos como
// lgfx::swap565_t diz isso ao LovyanGFX, e ele copia sem converter. O tipo do
// ponteiro é o que escolhe o formato (armadilha 11): um uint16_t* cru seria lido
// como RGB565 nativo e a capa sairia com as cores trocadas, sem aviso.
static int coverDraw(JPEGDRAW *draw) {
  if (coverTarget)
    coverTarget->pushImage(draw->x, draw->y, draw->iWidth, draw->iHeight,
                           reinterpret_cast<const lgfx::swap565_t *>(draw->pPixels));
  return 1;
}

// Pixels da capa, alocados por nós na PSRAM e emprestados ao sprite com
// setBuffer(). O LovyanGFX no RP2040/RP2350 implementa o setPsram(true) com um
// malloc comum: uma capa de 250x250 iria parar em 125 KB de SRAM, disputando com
// o framebuffer do DVI. Como o buffer é "preallocated", o deleteSprite() não o
// libera: quem libera é freeCover().
static uint16_t *coverPixels = nullptr;

static void freeCover() {
  if (coverSprite) {
    coverSprite->deleteSprite();
    delete coverSprite;
    coverSprite = nullptr;
  }
  if (coverPixels) {
    free(coverPixels); // o free() do arduino-pico reconhece ponteiro da PSRAM
    coverPixels = nullptr;
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
  jpeg.setPixelType(RGB565_BIG_ENDIAN); // ordem do LGFX_Sprite; ver coverDraw()
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

  // Na PSRAM (é só leitura depois de pronta, e a SRAM fica para o
  // framebuffer do DVI) e em 16 bits, a mesma profundidade do `tv`: sprite com
  // profundidade diferente do painel dá cor errada em silêncio (armadilha 10).
  coverPixels = (uint16_t *)ps_calloc((size_t)w * (size_t)h, sizeof(uint16_t));
  if (!coverPixels) {
    jpeg.close();
    free(buf);
    return false;
  }
  coverSprite = new LGFX_Sprite();
  coverSprite->setColorDepth(16);
  coverSprite->setBuffer(coverPixels, w, h, 16);
  coverTarget = coverSprite;
  const bool ok = jpeg.decode(0, 0, options);
  coverTarget = nullptr;
  jpeg.close();
  free(buf);
  if (!ok)
    freeCover();
  return ok;
}

// Faixa do progresso e do relógio da faixa, 160..182. Separada do resto da tela
// para o musicTick() repintar só ela uma vez por segundo: no Core2 quem mostrava
// o tempo andando era o LCD, e sem ele a TV ficaria com o relógio parado.
static constexpr int MUSIC_PROGRESS_Y = 160, MUSIC_PROGRESS_H = 24;
static uint32_t musicProgressDrawn = 0;

static void drawMusicProgress(GfxTarget *d, bool limpar) {
  const bool isPaused = paused.load();
  // Duração: usa o bitrate real do MP3 quando já conhecido (1º frame); senão a
  // estimativa inicial de 128 kbps gravada em wavDataEnd.
  uint32_t totalSec = (sampleRate && wavDataEnd >= wavDataStart) ? (wavDataEnd - wavDataStart) / (sampleRate * 4UL) : 0;
  if (mp3Mode && musicBitrateKbps > 0) {
    totalSec = (uint32_t)(mp3File.size() * 8ULL / (uint64_t)musicBitrateKbps / 1000ULL);
  }
  const uint32_t curSec = sampleRate ? samplesPlayed.load() / sampleRate : 0;
  int pct = totalSec ? (int)((uint64_t)curSec * (SAFE_W - 2) / totalSec) : 0;
  if (pct > SAFE_W - 2)
    pct = SAFE_W - 2; // a duração do MP3 é estimada e pode ficar curta
  if (limpar)
    d->fillRect(SAFE_L, MUSIC_PROGRESS_Y, SAFE_W, MUSIC_PROGRESS_H, TFT_NAVY);
  d->setTextDatum(top_left);
  d->setTextSize(1);
  d->drawRect(SAFE_L, MUSIC_PROGRESS_Y, SAFE_W, 6, RCA_ACCENT);
  if (pct > 0)
    d->fillRect(SAFE_L + 1, MUSIC_PROGRESS_Y + 1, pct, 4, TFT_YELLOW);
  d->setTextColor(RCA_ACCENT, TFT_NAVY);
  char clockLine[48];
  snprintf(clockLine, sizeof(clockLine), "%s %02lu:%02lu / %02lu:%02lu", isPaused ? "PAUSA" : "PLAY",
           (unsigned long)(curSec / 60UL), (unsigned long)(curSec % 60UL),
           (unsigned long)(totalSec / 60UL), (unsigned long)(totalSec % 60UL));
  if (musicShuffle)
    strncat(clockLine, "  SHUFFLE", sizeof(clockLine) - strlen(clockLine) - 1);
  else if (musicRepeat)
    strncat(clockLine, "  REPETIR", sizeof(clockLine) - strlen(clockLine) - 1);
  d->drawString(clockLine, SAFE_L, MUSIC_PROGRESS_Y + 14);
  musicProgressDrawn = millis();
}

void drawMusicNowPlaying() {
  char title[64] = "MUSICA";
  if (musicMeta.title[0])
    strcpy(title, musicMeta.title); // o Id3.h já entrega ASCII normalizado
  else if (musicQueueIndex >= 0 && musicQueueIndex < musicQueueCount) {
    const String p = musicQueue[musicQueueIndex];
    const int s = p.lastIndexOf('/');
    ascii::normalize(title, sizeof(title), (s >= 0 ? p.substring(s + 1) : p).c_str());
  }
  // Nº da faixa corrente na fila (estilo iPod), para exibição.
  String trackNo = (musicQueueIndex >= 0 && musicQueueCount) ? String(musicQueueIndex + 1) + "/" + String(musicQueueCount) : String("");
  // Só a TV: o Fruit Jam não tem tela local.
  GfxTarget *d = &tv;
  d->fillScreen(TFT_NAVY);
  d->setTextDatum(top_left);
  d->setTextColor(TFT_WHITE, TFT_NAVY);
  d->setTextSize(2);
  d->drawString(PTBR::MUSICA, SAFE_L, HEAD_Y);
  d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  d->setTextSize(1);
  // Capa do álbum (esquerda) com borda estilo VHS.
  const int coverY = BODY_Y + 8;
  if (musicScopeOn) {
    // O visualizador ocupa a faixa da capa e dos metadados: a capa vai de 62 a
    // 150, o progresso de 160 a 182 e a faixa de botões começa em 202, e não
    // sobra vão de 48 linhas em canto nenhum. No Core2 o nome da faixa ficava no
    // LCD; aqui a TV é a única tela, então título e artista sobem para as duas
    // linhas acima do painel (58 e 70), que terminam antes dele em 82.
    d->setTextColor(TFT_YELLOW, TFT_NAVY);
    d->drawString(truncateText(title, 28), SAFE_L, BODY_Y + 4);
    char who[96];
    snprintf(who, sizeof(who), "%s%s%s", musicMeta.artist[0] ? musicMeta.artist : "---",
             musicMeta.album[0] ? " - " : "", musicMeta.album);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(truncateText(who, 44), SAFE_L, BODY_Y + 16);
    audioScope.x = SAFE_L + 8;
    audioScope.y = coverY + 20; // 82..130, centrado na faixa da capa
    audioscope::drawStatic(d, 0, 0, audioScope);
    audioscope::drawModeLabel(d, 0, 0, audioScope);
  } else {
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
  }
  // Nº da faixa + bitrate (canto superior direito, abaixo da régua).
  if (trackNo.length()) {
    String meta = trackNo;
    if (mp3Mode && musicBitrateKbps > 0)
      meta += " " + String(musicBitrateKbps) + "K";
    d->setTextColor(TFT_DARKCYAN, TFT_NAVY);
    d->drawString(meta, SAFE_R - 96, HEAD_RULE_Y + 6);
  }
  drawMusicProgress(d, false); // a tela acabou de ser preenchida
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
  // Estado do visualizador zerado ao entrar: sem isto a faixa nova herdaria as
  // barras da anterior por alguns quadros (armadilha 6 do AGENTS.md).
  audioTap.clear();
  audioscope::reset(audioScope);
  // Garante a tarefa de áudio ociosa: encerra qualquer vídeo/weather em
  // andamento. O PCM (WAV ou MP3 decodificado) sai pela tarefa de áudio via
  // audioout; daqui só se abre o arquivo.
  playing = false;
  while (audioReady && !audioIdle && !audioFailed && !radioActive.load() && !toneActive.load())
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
  while (audioReady && !audioIdle && !audioFailed && !radioActive.load() && !toneActive.load())
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
  if (!playbackFinished) {
    // Relógio da faixa a 1 Hz, repintando só a sua faixa: a tela inteira a cada
    // segundo piscaria e apagaria o visualizador. Com o protetor de tela no ar
    // não se desenha nada: a interface não está na tela.
    if (idleMgr.stage() != burnin::STAGE_BLANK && millis() - musicProgressDrawn >= 1000)
      drawMusicProgress(&tv, true);
    return;
  }
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
// Endereço e rede da tela, lidos UMA vez em startTransfer(). O transferTick()
// monta o estado a cada volta do loop, e no Fruit Jam cada WiFi.SSID() ou
// localIP() é uma transação SPI com o ESP32-C6 (sob net::Lock) — perguntar isso
// centenas de vezes por segundo disputaria o barramento com o próprio servidor
// que está recebendo o arquivo.
static String transferAddress, transferSsid;

static xfer::State transferState() {
  const String &endereco = transferAddress, &rede = transferSsid;
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
  xfer::draw(&tv, st);
  lastTransferStage = st.stage;
}

// Controle remoto pelo navegador. Chamado da tarefa do servidor HTTP, entao so
// enfileira: quem desenha e o loop, e desenhar daqui seria desenhar de outro
// contexto em cima do que o loop esta pintando.
static bool webCommand(const char *acao, void *) {
  struct Mapa {
    const char *nome;
    NavAction acao;
  };
  static const Mapa MAPA[] = {
      {"acima", NavAction::LEFT},  {"abaixo", NavAction::RIGHT},
      {"ok", NavAction::SELECT},   {"voltar", NavAction::BACK},
      {"inicio", NavAction::HOME},
  };
  for (const Mapa &m : MAPA) {
    if (!strcmp(acao, m.nome)) {
      input.inject(m.acao, InputSource::WEB);
      Serial.printf("[M5RETRO] controle web: %s\n", acao);
      return true;
    }
  }
  return false;
}

void startTransfer() {
  stopProgram();  // encerra vídeo E a música em loop do Weather
  stopWeather();
  // Liga /controle e /cmd no mesmo servidor. Sem esta chamada as duas rotas
  // respondem 404 e o servidor se comporta como antes.
  fileTransfer.setCommandHandler(webCommand, nullptr);
  if (!network.connected()) {
    setError(PTBR::SEM_WIFI);
    return;
  }
  // Sem isto o Wi-Fi pode ficar em modem sleep: o rádio só acorda a cada DTIM,
  // o ping sobe para centenas de ms e o SYN de entrada se perde. Para receber
  // dezenas de MB o rádio precisa ficar acordado. No NINA isso é o
  // noLowPowerMode(). Não se religa a economia ao sair: o Fruit Jam não tem
  // bateria, e o rádio pela internet também quer o Wi-Fi acordado.
  {
    net::Lock trava;
    WiFi.noLowPowerMode();
    transferSsid = WiFi.SSID();
  }
  if (!fileTransfer.begin(sdMutex)) {
    setError(fileTransfer.lastError());
    return;
  }
  {
    // O net::Lock é recursivo: tomar aqui é seguro mesmo que o ip() já o tome.
    net::Lock trava;
    transferAddress = "http://" + fileTransfer.ip();
  }
  state = FILE_TRANSFER;
  lastTransferStage = xfer::Stage::Error; // garante o primeiro desenho
  lastTransferDraw = 0;
  drawTransferFrame();
  Serial.printf("[M5RETRO] Transferencia em %s:%u usuario:%s senha:%s escutando:%d\n",
                transferAddress.c_str(), (unsigned)fileTransfer.port(), fileTransfer.user(),
                fileTransfer.password(), (int)fileTransfer.listening());
}

void stopTransfer() {
  fileTransfer.stop();
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
    xfer::drawProgress(&tv, st);
  }
}

// Data e hora com segundos, do relógio sincronizado por NTP. O Fruit Jam não tem
// RTC com bateria: a hora só existe depois que o ESP32-C6 sincronizou (RtcClock.h
// acerta o relógio do sistema com settimeofday). Antes disso o epoch fica em
// 1970, e mostrar "01/01/1970" seria pior que não mostrar nada — sai "--:--",
// como um videocassete recém-ligado na tomada.
static void drawHomeClock(bool limpar) {
  char texto[32];
  const time_t agora = time(nullptr);
  if (agora >= 1704067200) { // 2024-01-01: houve sincronismo NTP
    struct tm t;
    localtime_r(&agora, &t);
    snprintf(texto, sizeof(texto), "%02d/%02d/%04d  %02d:%02d:%02d", t.tm_mday, t.tm_mon + 1,
             t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    snprintf(texto, sizeof(texto), "%s", "--:--");
  }
  GfxTarget *d = &tv;
  if (limpar)
    d->fillRect(SAFE_L, CLOCK_Y, SAFE_W, 18, TFT_NAVY);
  d->setFont(&fonts::Font2);
  d->setTextSize(1);
  d->setTextDatum(top_right);
  d->setTextColor(TFT_WHITE, TFT_NAVY);
  d->drawString(texto, SAFE_R - 1, CLOCK_Y);
  d->setTextDatum(top_left);
  d->setFont(&fonts::Font0);
  lastClockDraw = millis();
}

void drawHome() {
  // 12 itens, na MESMA ordem de homeTarget() em UiLogic.h -- as duas listas
  // andam juntas e trocar uma sem a outra manda o usuario para a tela errada.
  // Coluna da esquerda: 0..5. Coluna da direita: 6..11. (No Core2 havia uma
  // terceira lista, a grade de toque do LCD; sem tela local ela saiu, e o menu
  // e so a TV.)
  const char *items[] = {PTBR::VIDEOS,  PTBR::MUSICA,        PTBR::FOTOS,
                         PTBR::RADIO,   PTBR::WEATHER,       PTBR::TRAFEGO,
                         PTBR::PADRAO_TESTE, PTBR::TRANSFERENCIA, PTBR::CONFIGURACOES,
                         PTBR::INFO_SISTEMA, PTBR::EMULADORES, PTBR::DESLIGAR};
  static_assert(sizeof(items) / sizeof(items[0]) == HOME_COUNT,
                "rotulos do menu inicial fora de sincronia com HOME_COUNT");
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(top_left);
  tv.setTextSize(2);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString(PTBR::APP, SAFE_L, HEAD_Y);
  tv.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  tv.setTextSize(1);
  for (int i = 0; i < HOME_COUNT; i++) {
    // 6 linhas de 16 px a partir de CLOCK_Y+20 = 70 terminam em 166, com folga
    // ate a barra de legendas em 202.
    const int x = SAFE_L + 4 + homeColumn(i) * HOME_RCA_COL_W;
    const int y = CLOCK_Y + 20 + homeRow(i) * 16;
    tv.setTextColor(i == homeSelection ? RCA_ACCENT : TFT_WHITE, TFT_NAVY);
    tv.drawString(String(i == homeSelection ? ">" : " ") + items[i], x, y);
  }
  drawHomeClock(false); // a tela acabou de ser preenchida; não precisa limpar
  drawControllerLabels("ACIMA", "OK", "ABAIXO");
}
void drawLibrary() {
  const int count = libraryProgramCount();
  librarySelection = count ? constrain(librarySelection, 0, count - 1) : 0;
  const int first = (librarySelection / 4) * 4;
  // Só a TV: o Fruit Jam não tem tela local.
  GfxTarget *display = &tv;
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
  drawControllerLabels("ANTERIOR", "PLAY", "PROXIMO");
}

// Desenha um "aviaozinho" top-down orientado pela proa (0 = norte, horario).
// Coordenadas locais: lx = direita, ly = frente (nariz). Rotaciona por heading.
static void drawAirplane(GfxTarget *d, int px, int py, double headingDeg, uint16_t color) {
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
  // Só a TV: o Fruit Jam não tem tela local (PORTING.md 3.2).
  GfxTarget *d = &tv;
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
// HTTP simples, nao HTTPS, de proposito: e previsao publica, sem credencial e
// sem nada a proteger. No Core2 o motivo era o heap do mbedTLS; no Fruit Jam o
// TLS roda no ESP32-C6 e nao custa SRAM ao RP2350, mas ainda custa um handshake
// lento no coprocessador e ocupa o SPI1 (net::Lock) por mais tempo, disputando
// com o radio. A resposta e byte a byte a mesma: 816 bytes.
static const char *WEATHER_URL =
    "http://api.open-meteo.com/v1/forecast?latitude=-20.5386&longitude=-47.4008"
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
  // A resposta tem ~800 bytes; 4 KB de teto dão folga para o Open-Meteo
  // crescer sem aceitar lixo. O buffer fica na PSRAM, não na pilha: esta
  // função roda na tarefa WEATHER_HTTP, e um array de KB numa pilha de tarefa
  // de rede já estourou este firmware duas vezes (AGENTS.md 2.3).
  constexpr size_t cap = 4096;
  char *buf = (char *)ps_malloc(cap);
  if (!buf) {
    Serial.println("[M5RETRO] Tempo: sem memoria para a resposta");
    return false;
  }
  bool ok = false;
  // O net::httpGet lê o corpo inteiro (Content-Length ou chunked) e avisa se
  // cortou: corpo truncado é rejeitado em vez de publicar dados pela metade.
  const net::HttpResult r = net::httpGet(WEATHER_URL, buf, cap, 10000);
  if (r.status == 200) {
    if (r.truncated) {
      Serial.println("[M5RETRO] Tempo: resposta maior que o buffer");
    } else if (r.length) {
      JsonDocument doc(&psramJsonAllocator);
      auto err = deserializeJson(doc, buf, r.length, DeserializationOption::NestingLimit(8));
      if (!err)
        ok = weatherParse(doc, out);
      else
        Serial.printf("[M5RETRO] Tempo: parse %s (%u bytes)\n", err.c_str(), (unsigned)r.length);
    } else {
      Serial.println("[M5RETRO] Tempo: corpo vazio");
    }
  } else {
    Serial.printf("[M5RETRO] Tempo: HTTP %d\n", r.status);
  }
  free(buf);
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
  // Ver NET_TASK_STACK: a pilha caiu de 8 para 6 KB sem medição no aparelho.
  Serial.printf("[WEATHER] pilha livre minima: %u B de %u\n",
                (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                (unsigned)NET_TASK_STACK);
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
  // criava outra tarefa HTTP — dezenas por segundo com o roteador fora.
  if (lastWeatherAttempt && now - lastWeatherAttempt < WEATHER_RETRY_MS)
    return;
  if (weatherReady.load() && now - lastWeatherGood < WEATHER_REFRESH_MS)
    return;
  lastWeatherAttempt = now;
  weatherBusy.store(true);
  int next = 1 - weatherActive.load();
  // Mesmo núcleo e prioridade da tarefa do radar; ver NET_TASK_STACK.
  if (xTaskCreatePinnedToCore(weatherNetworkTask, "WEATHER_HTTP", NET_TASK_STACK, (void *)(intptr_t)next,
                              0, nullptr, 1) != pdPASS) {
    weatherBusy.store(false);
    weatherStatus.store("SEM MEMORIA");
    Serial.printf("[WEATHER] criacao da tarefa falhou: livre=%u interno=%u maior_interno=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                             MALLOC_CAP_8BIT));
  } else {
    Serial.println("[WEATHER] tarefa criada");
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
// canvas do DVI é sempre RGB565.
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
  tv.setFont(&fonts::Font2);
  tv.setTextSize(1);
  tickerWrapAt = tv.textWidth(unit.c_str());
  tickerFull = unit;
  while (tickerWrapAt > 0 && tv.textWidth(tickerFull.c_str()) < tickerWrapAt + CRT_W)
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
  weatherDrawTicker(&tv, 0, 0);
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
    paintForecast(&tv, 0, 0, nullptr);
  else
    paintCurrent(&tv, 0, 0, nullptr);
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
    weatherFx.attach(&tv);
    weatherFx.wipe(now, weatherScreen(weatherPage), crt::fx::DIR_DOWN);
    lastWeatherPageMs = now;
  } else if (weatherReady.load() && now - lastWeatherPageMs >= WEATHER_PAGE_MS) {
    // Rodízio das páginas: slide horizontal, a transição típica entre
    // "condições atuais" e "previsão estendida". Também sem buffer.
    const int next = weatherPage ? 0 : 1;
    weatherFx.attach(&tv);
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
  while (audioReady && !audioIdle && !audioFailed && !radioActive.load() && !toneActive.load())
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
  weatherFx.attach(&tv);
  weatherFx.skip();
  weatherTickerBuild();
  drawWeatherFrame();
  // A placa "EXIBINDO NA TV" do LCD do Core2 saiu: o Fruit Jam só tem a TV.
}

void stopWeather() {
  weatherAudio = false;
  playing = false;
  while (audioReady && !audioIdle && !audioFailed && !radioActive.load() && !toneActive.load())
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

// Itens de CONFIGURAÇÕES, na ordem da tela. Nome em vez de índice literal: o
// CORES saiu do meio da lista no port (PORTING.md 3.5), e com os números
// espalhados por drawSettings() e handleNavigation() bastaria esquecer um para
// o botão editar um item e a tela mostrar outro.
enum SettingsItem : int {
  SET_VIDEO,
  SET_VOLUME,
  SET_RADAR_RANGE,
  SET_RADAR_REFRESH,
  SET_AUDIO_OUT,
  SET_VHS_OSD,
  SET_WIFI,
  SET_API,
  SET_SD_CARD,
  SET_SETUP_PORTAL,
  SET_CHANNEL_MODE,
  SET_SLEEP_TIMER,
  SET_VHS_WEAR,
  SET_COUNT
};

void drawSettings() {
  // Mesma ordem de SettingsItem.
  const char *names[SET_COUNT] = {
      "VIDEO",    "VOLUME", "ALCANCE RADAR", "ATUALIZACAO",     PTBR::SAIDA_AUDIO, PTBR::OSD_ESTILO_VHS,
      "WI-FI",    "API",    "CARTAO SD",     "CONFIGURAR REDE", "MODO CANAL",      "DESLIGAR EM",
      "FITA VHS",
  };
  // Rótulos da saída de som no Fruit Jam. Os valores do enum e do JSON ("rca",
  // "interno") ficam por compatibilidade com o settings.json antigo; o que muda
  // é o destino: RCA = fone P2 (que vai à TV), INTERNAL = alto-falante da placa.
  const char *audio = settings.audioOutput == AudioOutput::RCA        ? "TV (P2)"
                      : settings.audioOutput == AudioOutput::INTERNAL ? "ALTO-FALANTE"
                                                                      : "MUDO";
  String value;
  switch (settingsSelection) {
  case SET_VIDEO:
    value = "DVI 640X480";
    break;
  case SET_VOLUME:
    value = String(settings.volume) + "%";
    break;
  case SET_RADAR_RANGE:
    value = String(settings.rangeKm) + " km";
    break;
  case SET_RADAR_REFRESH:
    value = String(settings.refreshSeconds) + " s";
    break;
  case SET_AUDIO_OUT:
    value = audio;
    break;
  case SET_VHS_OSD:
    value = settings.vhsOsd ? PTBR::ATIVADO : PTBR::DESATIVADO;
    break;
  // network.connected() e não WiFi.status(): o estado já está em cache no
  // NetworkManager, e cada WiFi.* é uma transação no SPI1 do ESP32-C6.
  case SET_WIFI:
    value = network.connected() ? PTBR::CONECTADO : PTBR::DESCONECTADO;
    break;
  case SET_API:
    value = apiStatus;
    break;
  // A chave mecânica do soquete diz a verdade; antes o item dizia sempre
  // DISPONIVEL.
  case SET_SD_CARD:
    value = board::cardInserted() ? PTBR::DISPONIVEL : PTBR::INDISPONIVEL;
    break;
  case SET_SETUP_PORTAL:
    value = "ABRIR";
    break;
  case SET_CHANNEL_MODE:
    value = tvChannel.modeLabel();
    break;
  case SET_SLEEP_TIMER:
    value = sleepTimer.active() ? String(sleepTimer.minutes()) + " MIN" : String("DESLIGADO");
    break;
  default:
    value = vhs::wearLabel(settings.vhsWear);
    break;
  }
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(top_left);
  tv.setTextSize(2);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString(PTBR::CONFIGURACOES, SAFE_L, HEAD_Y);
  tv.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  tv.setTextSize(1);
  tv.setTextColor(RCA_ACCENT, TFT_NAVY);
  const int sel = (settingsSelection >= 0 && settingsSelection < SET_COUNT) ? settingsSelection : 0;
  tv.drawString(String(settingsEditing ? "> " : "  ") + names[sel], SAFE_L, BODY_Y + 26);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString(value, SAFE_L + 10, BODY_Y + 58);
  drawControllerLabels(settingsEditing ? "-" : "ACIMA", settingsEditing ? "SALVAR" : "OK",
                       settingsEditing ? "+" : "ABAIXO");
}
// ---------------------------------------------------------------------------
//  Apresentacao de fotos (/M5RETRO/fotos)
// ---------------------------------------------------------------------------
//
// O formato que o aparelho exibe e estreito, e nao por capricho: o JPEGDEC
// desta versao devolve JPEG_DECODE_ERROR em chroma 4:4:4 e, pior, aceita um
// JPEG progressivo SEM erro desenhando so 40x30 (os coeficientes DC do
// primeiro scan). O teto de 128 KB vem do jpegBuffer. tools/prepare_photos.py
// resolve os tres na origem; aqui a politica so recusa com mensagem na tela.

void drawPhotos() {
  const uint32_t agora = millis();
  if (photoShow.count() == 0) {
    GfxTarget *d = &tv; // so a TV: o Fruit Jam nao tem tela local
    d->fillScreen(TFT_NAVY);
    d->setTextDatum(top_left);
    d->setTextSize(2);
    d->setTextColor(TFT_WHITE, TFT_NAVY);
    d->drawString(PTBR::FOTOS, SAFE_L, HEAD_Y);
    d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
    d->setTextSize(1);
    d->drawString(PTBR::SEM_FOTOS, SAFE_L + 6, BODY_Y + 40);
    d->drawString("ENVIE PELO MENU TRANSFERIR ARQUIVOS", SAFE_L + 6, BODY_Y + 60);
    drawControllerLabels(PTBR::VOLTAR, "", "");
    return;
  }
  // A leitura do cartao tem de acontecer sob o mutex: o audioTask le o mesmo
  // cartao. O decode em si e sobre o buffer ja em memoria, fora do mutex.
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  // No arduino-pico o SD e so uma fachada (SDClass, que nao e fs::FS); o
  // sistema de arquivos do cartao que o PhotoShow espera e o SDFS.
  photoShow.load(SDFS, agora);
  if (sdMutex)
    xSemaphoreGive(sdMutex);

  // paint() ja limpa a tela, decodifica, poe a legenda e o rodape.
  photoShow.paint(&tv, 0, 0);
  drawControllerLabels(PTBR::ANTERIOR, "TEMPO", PTBR::PROXIMO);
}

void enterPhotos() {
  stopProgram(); // nada de video ou musica disputando o cartao e o buffer
  if (!photoPool)
    photoPool = (char *)ps_malloc(PHOTO_POOL_BYTES);
  if (!photoOffsets)
    photoOffsets = (uint16_t *)ps_malloc(PHOTO_MAX_ENTRIES * sizeof(uint16_t));
  if (!photoPool || !photoOffsets) {
    setError(PTBR::MEMORIA_INSUFICIENTE);
    return;
  }
  photoShow.attachStorage(photoPool, PHOTO_POOL_BYTES, photoOffsets, PHOTO_MAX_ENTRIES);
  photoShow.attachBuffer(jpegBuffer, MAX_JPEG);
  photoShow.attachDecoder(&jpeg);
  photoShow.setFolder(PHOTOS);
  const uint32_t agora = millis();
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (!SD.exists(PHOTOS))
    SD.mkdir(PHOTOS);
  const int n = photoShow.scan(SDFS, agora); // SDFS, nao SD: ver drawPhotos()
  if (sdMutex)
    xSemaphoreGive(sdMutex);
  photoShow.afterScan(agora);
  Serial.printf("[M5RETRO] Fotos: %d em %s (%d fora do pool)\n", n, PHOTOS,
                photoShow.catalog().dropped());
  state = PHOTO_SHOW;
  drawPhotos();
}

// ---------------------------------------------------------------------------
//  Padrao de teste: barras SMPTE + tom de 1 kHz
// ---------------------------------------------------------------------------

void drawTestPattern() {
  // As barras usam o QUADRO INTEIRO de proposito: a graca e ver o quanto o tubo
  // corta. So o texto respeita a area segura.
  if (testPatternPage == 0)
    testpattern::drawBars(&tv, 0, 0);
  else if (testPatternPage == 1)
    testpattern::drawSlate(&tv, 0, 0);
  else
    testpattern::drawSnow(&tv, 0, 0);
  // No Core2 o nome do padrao e o estado do tom iam para o LCD. Sem tela local
  // nada disso e repetido na TV: texto por cima das barras atrapalharia
  // justamente a leitura que se quer fazer nelas. O tom de 1 kHz sai pela
  // tarefa de audio (audioout), puxado do testTone enquanto toneActive.
  drawControllerLabels(PTBR::VOLTAR, "PADRAO", "");
}

void enterTestPattern() {
  stopProgram();
  testPatternPage = 0;
  testTone.reset();
  testTone.setAmplitude(testpattern::AMP_MINUS20DB);
  toneActive = true; // o audioTask passa a puxar do gerador
  state = TEST_PATTERN;
  drawTestPattern();
}

void stopTestPattern() {
  toneActive = false;
}

void cycleTestPattern() {
  testPatternPage = (testPatternPage + 1) % 3;
  // Tom so com as barras: e o par "bars and tone". No chuvisco seria ruido em
  // cima de ruido, e no cartaz de encerramento atrapalha.
  toneActive = (testPatternPage == 0);
  drawTestPattern();
}

// ---------------------------------------------------------------------------
//  Radio pela internet
// ---------------------------------------------------------------------------
//
// A tela e so o logotipo da emissora e um relogio grande em hora local, como
// pedido. O logotipo vem de include/DiarioLogo.h; sem esse arquivo a tela cai
// no nome da estacao em texto e continua correta.

void drawRadioScreen(bool completo) {
  radioui::State st;
  time_t agora = time(nullptr);
  struct tm tmv;
  if (agora > 1704067200 && localtime_r(&agora, &tmv)) {
    st.hour = (uint8_t)tmv.tm_hour;
    st.minute = (uint8_t)tmv.tm_min;
    st.second = (uint8_t)tmv.tm_sec;
    st.day = (uint8_t)tmv.tm_mday;
    st.month = (uint8_t)(tmv.tm_mon + 1);
    st.year = (uint16_t)(tmv.tm_year + 1900);
    st.weekday = (uint8_t)tmv.tm_wday;
    st.timeValid = true;
  }
  char titulo[96];
  radioStream.copyTitle(titulo, sizeof(titulo));
  st.stationName = radioStream.stationName();
  st.statusText = radioStream.stateLabel();
  st.title = titulo;
  // tick() repinta SO o que mudou e devolve 0 quando nada mudou -- nesse caso
  // nao escreve um pixel. A draw() daqui de cima chama drawBackground() toda
  // vez, ou seja tela cheia: usada a cada 250 ms ela pisca o tubo inteiro.
  // Zerar o cache faz o proprio tick() fazer a primeira pintura completa.
  if (completo)
    radioui::cacheReset(radioCache);
  radioui::tick(&tv, 0, 0, st, radioCache, millis());
}

void enterRadio() {
  stopProgram(); // o radio nao convive com o video: nao ha SRAM para os dois
  mp3End();      // garante que o player local soltou os buffers abaixo
  // Empresta ao radio os buffers de trabalho do player de MP3 local. Eles sao
  // estaticos em SRAM e ficam parados enquanto o radio toca (os dois nunca
  // tocam juntos), entao o decodificador ganha memoria rapida a custo zero.
  // Na PSRAM o libhelix nao acompanha 44100 Hz estereo e o audio arrasta.
  radioStream.useWorkBuffers(mp3In, sizeof(mp3In), mp3FramePcm,
                             sizeof(mp3FramePcm) / sizeof(mp3FramePcm[0]));
  if (!radioStream.begin(0)) {
    setError(PTBR::SEM_SERVIDOR);
    return;
  }
  radioActive = true;
  radioLastDraw = 0;
  audioTap.clear();
  audioscope::reset(audioScope);
  radioPcmUs = 0; radioPcmCalls = 0; radioPcmMaxUs = 0;
  radioWriteUs = 0; radioFramesOut = 0; radioSince = millis();
  // No Core2 o radio baixava o painel para RGB332 enquanto tocava, para sumir
  // com a task_memcpy do psram_half_use. O DVI do Fruit Jam e sempre RGB565 em
  // SRAM e nao tem essa tarefa: nada a trocar aqui.
  state = RADIO;
  drawControllerLabels(PTBR::VOLTAR, "", "");
  drawRadioScreen(true);
}

void stopRadio() {
  radioActive = false;
  radioStream.end();
}

// ---------------------------------------------------------------------------
//  EMULADORES (PORTING.md, secao "Emuladores")
// ---------------------------------------------------------------------------
//
// Esta TV pode compartilhar a flash do Fruit Jam com o fork fruitjam-retro-tv
// do pico-bootLoader (GPLv3, repositorio separado): ele fica residente nos
// 512 KB iniciais (0x10000000) e reserva 4 MB no topo (0x10C00000) para uma
// copia desta TV, gravada com o env "fruitjam-launcher" -- ver
// platformio.ini e boards/fruitjam_launcher_memmap.ld. A build "fruitjam"
// (env padrao, standalone em 0x10000000) nao tem lancador por baixo: nao ha
// para onde voltar depois de abrir um emulador, entao o item so explica.
//
// Protocolo com o lancador, REIMPLEMENTADO aqui (nao copiado -- o codigo do
// lancador fica no repositorio dele; isto e so a interface numerica que ele
// ja publica para qualquer app que rode sob ele, o mesmo par de watchdog
// scratch registers que um emulador usa para "voltar ao menu"):
//   escrever 0xB007BACE no scratch[7] do watchdog e reiniciar por
//   watchdog_reboot() (aqui, rp2040.reboot(), que e' exatamente isso -- ver
//   RP2040Support.h) faz o lancador, no boot seguinte, mostrar o picker de
//   emuladores em vez de retomar a regiao que estava ativa antes. E o MESMO
//   caminho que um emulador usa para devolver o controle ao menu: o
//   lancador nao precisa saber quem pediu.
//
// O que NAO esta implementado: pedir um emulador especifico direto (o
// lancador so mostra o menu; escolher o emulador ainda e manual, com os
// tres botoes, na tela dele). O enunciado desta tarefa deixava isso como
// "ou" opcional -- ver PORTING.md para o raciocinio.
#ifdef FRUITJAM_LAUNCHER_BUILD
static constexpr int LOADER_RETURN_SCRATCH = 7;
static constexpr uint32_t LOADER_RETURN_MAGIC = 0xB007BACEu;
#endif

void drawEmulatorsInfo() {
  // So a build standalone chega aqui (ver enterEmulators()). Mesma
  // diagramacao das outras telas de aviso (ex.: drawPhotos() sem fotos):
  // cabecalho + corpo em ASCII dentro da area segura.
  GfxTarget *d = &tv;
  d->fillScreen(TFT_NAVY);
  d->setTextDatum(top_left);
  d->setTextSize(2);
  d->setTextColor(TFT_WHITE, TFT_NAVY);
  d->drawString(PTBR::EMULADORES, SAFE_L, HEAD_Y);
  d->drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  d->setTextSize(1);
  d->setTextColor(RCA_ACCENT, TFT_NAVY);
  d->drawString(PTBR::EMULADORES_PRECISA_LANCADOR, SAFE_L + 6, BODY_Y + 20);
  d->setTextColor(TFT_WHITE, TFT_NAVY);
  d->drawString(PTBR::EMULADORES_EXPLICACAO_1, SAFE_L + 6, BODY_Y + 44);
  d->drawString(PTBR::EMULADORES_EXPLICACAO_2, SAFE_L + 6, BODY_Y + 60);
  d->drawString(PTBR::EMULADORES_EXPLICACAO_3, SAFE_L + 6, BODY_Y + 76);
  d->drawString(PTBR::EMULADORES_EXPLICACAO_4, SAFE_L + 6, BODY_Y + 92);
  drawControllerLabels(PTBR::VOLTAR, "", "");
}

// Entra em EMULADORES a partir do menu inicial. As duas builds tratam a
// mesma UiState de formas completamente diferentes -- ver o comentario
// grande acima.
void enterEmulators() {
  state = EMULATORS;
#ifndef FRUITJAM_LAUNCHER_BUILD
  drawEmulatorsInfo();
#else
  // Mesma ordem de requestPowerOff() (AGENTS.md, armadilha 3): radio/tom/
  // musica do Weather primeiro, sao eles que seguram audioIdle. So depois
  // esperar o decode -- ANTES do dualText abaixo, que nao pode dividir a
  // tela com um quadro decodificando por baixo (mesmo raciocinio de
  // requestPowerOff()) -- e so entao desligar rede/portal/audio: nada disso
  // pode continuar disputando o cartao ou a I2S depois que o lancador
  // reescrever a flash de um emulador.
  if (radioActive.load())
    stopRadio();
  toneActive = false;
  weatherAudio = false;
  playing = false;
  paused = true;
  waitVideoDecodeIdle();
  dualText(PTBR::EMULADORES_ABRINDO_MENU);
  if (portal.active())
    portal.stop();
  network.disconnect();
  audioout::setRoute(audioout::Route::MUTED);
  audioout::flushSilence();
  Serial.println("[M5RETRO] EMULADORES: pedindo o menu do lancador e reiniciando");
  Serial.flush();
  delay(150); // deixa a mensagem aparecer e o audioTask ver o mudo, como o DESLIGAR
  watchdog_hw->scratch[LOADER_RETURN_SCRATCH] = LOADER_RETURN_MAGIC;
  rp2040.reboot(); // watchdog_reboot(0,0,10) por baixo -- nao volta
#endif
}

void drawInfo() {
  // Página 0: hardware; página 1: rede. Datum top_left e uma linha por item,
  // com o valor cortado em 26 caracteres: da coluna de valores (x = 140) até a
  // borda direita da área segura (296) cabem 26 glifos de 6 px.
  constexpr int VAL_X = SAFE_L + 116, STEP = 22, VAL_MAX = 26;
  tv.fillScreen(TFT_NAVY);
  tv.setTextDatum(top_left);
  tv.setTextSize(2);
  tv.setTextColor(TFT_WHITE, TFT_NAVY);
  tv.drawString(PTBR::INFO_SISTEMA, SAFE_L, HEAD_Y);
  tv.drawFastHLine(SAFE_L, HEAD_RULE_Y, SAFE_W, RCA_ACCENT);
  tv.setTextSize(1);
  const int y0 = BODY_Y;
  auto row = [&](int i, const char *label, const String &value) {
    tv.setTextColor(RCA_ACCENT, TFT_NAVY);
    tv.drawString(label, SAFE_L, y0 + i * STEP);
    tv.setTextColor(TFT_WHITE, TFT_NAVY);
    tv.drawString(value.substring(0, VAL_MAX), VAL_X, y0 + i * STEP);
  };
  if (!infoPage) {
    // Relógio real, não o nominal: o DVHSTX sobe o RP2350 para 240 MHz por
    // conta própria, apesar do f_cpu de 150 MHz no platformio.ini.
    row(0, "PROCESSADOR", "RP2350B " + String(ESP.getCpuFreqMHz()) + " MHz");
    row(1, "SRAM LIVRE", String(ESP.getFreeHeap() / 1024) + " KB");
    row(2, "PSRAM LIVRE", String(ESP.getFreePsram() / 1024) + " KB");
    String cartao = "AUSENTE";
    if (board::cardInserted()) {
      // size64() pode ir ao cartão: toma o sdMutex por esta operação só, como
      // todo acesso ao SD que disputa com a tarefa de áudio.
      if (sdMutex)
        xSemaphoreTake(sdMutex, portMAX_DELAY);
      const uint64_t bytes = SD.size64();
      if (sdMutex)
        xSemaphoreGive(sdMutex);
      cartao = bytes ? String((uint32_t)(bytes / (1024ULL * 1024ULL))) + " MB" : String("SEM LEITURA");
    }
    row(3, "CARTAO SD", cartao);
    row(4, "VERSAO", "fruitjam");
    // Sem RTC com bateria no Fruit Jam: ou a hora veio do NTP do ESP32-C6, ou
    // não há hora — e aí a tela diz "--:--" em vez de inventar 1970.
    String hora = "--:--";
    const time_t agora = time(nullptr);
    if (rtcclock::synced() && rtcclock::isPlausibleEpoch(agora)) {
      struct tm local;
      char hhmm[8];
      localtime_r(&agora, &local);
      snprintf(hhmm, sizeof(hhmm), "%02d:%02d", local.tm_hour, local.tm_min);
      hora = hhmm;
    }
    row(5, "HORA", hora + "  " + rtcclock::sourceLabel());
  } else {
    const bool conectado = network.connected();
    String ssid, ip = "-", sinal = "-", nina;
    {
      // Toda chamada WiFiNINA passa pelo net::Lock (o SPI1 é compartilhado
      // com as tarefas de rede). Uma tomada só para as quatro leituras.
      net::Lock lock;
      nina = net::firmwareVersion();
      if (conectado) {
        ssid = WiFi.SSID();
        ip = WiFi.localIP().toString();
        sinal = String(WiFi.RSSI()) + " dBm";
      }
    }
    if (!conectado)
      ssid = network.ssid(); // a rede configurada, para saber qual falhou
    // SSID e versão vêm de fora (usuário e firmware do C6): ASCII antes da tela.
    char buf[40];
    ascii::normalize(buf, sizeof(buf), ssid.c_str());
    row(0, PTBR::STATUS_REDE, conectado ? PTBR::CONECTADO : PTBR::DESCONECTADO);
    row(1, "WI-FI", buf[0] ? String(buf) : String("-"));
    row(2, PTBR::SINAL, sinal);
    row(3, PTBR::ENDERECO_IP, ip);
    ascii::normalize(buf, sizeof(buf), nina.c_str());
    row(4, "ESP32-C6 NINA", buf[0] ? String(buf) : String("-"));
  }
  drawControllerLabels(PTBR::ANTERIOR, PTBR::DETALHES, PTBR::PROXIMO);
}

void handleNavigation(NavAction a) {
  // Conta como atividade antes de qualquer coisa: mesmo uma acao que esta tela
  // vai ignorar significa que tem gente na frente do aparelho.
  if (a != NavAction::NONE)
    idleMgr.notifyActivity(millis());
  if (a == NavAction::NONE)
    return;
  // Qualquer comando adia o desligamento automatico: ninguem quer a TV apagando
  // no meio de um filme so porque o prazo venceu enquanto se assistia.
  if (sleepTimer.active())
    sleepTimer.restart(millis());
  osdUntil = millis() + 3000;
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
    // Este ramo retorna ANTES dos ramos por estado, entao o stopRadio() que
    // mora no ramo do RADIO nunca rodava ao sair pelo HOME: a tarefa RADIO_ICY
    // ficava viva com sua pilha de 8 KB, o socket aberto e radioActive em true.
    // Justamente 8 KB e o que a WEATHER_HTTP precisa, e a previsao passava a
    // dizer SEM MEMORIA depois de uma visita ao radio.
    if (radioActive.load())
      stopRadio();
    stopTestPattern(); // idem para o tom de 1 kHz
    // Sair de proposito cancela o modo canal: sem isto o proximo programa
    // comecaria sozinho depois que o usuario ja saiu da reproducao.
    tvChannel.stop();
    if (playing || wavFile)
      stopProgram();
    waitVideoDecodeIdle(); // decode pendente de um filme que já tinha acabado
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
      if (homeSelection == HOME_POWER_OFF) { // DESLIGAR: soft-off (tela preta, dorme ate um botao).
        requestPowerOff();
        return;
      }
      state = homeTarget(homeSelection);
      if (state == VIDEO_LIBRARY) {
        libraryScanned = false; // revarre ao entrar na biblioteca
        drawLibrary();
      } else if (state == MUSIC_BROWSER) {
        // Revarre ao entrar, como a biblioteca de videos: sem isto a lista so
        // era montada pelo "diag music" e o menu abria em SEM MUSICAS.
        musicScanDir();
        drawMusicBrowser();
      } else if (state == AIRCRAFT_RADAR) {
        lastApiPoll = 0; // consulta imediata ao entrar no radar
        drawRadar();
      } else if (state == SETTINGS)
        drawSettings();
      else if (state == WEATHER)
        startWeather();
      else if (state == FILE_TRANSFER)
        startTransfer();
      else if (state == PHOTO_SHOW)
        enterPhotos();
      else if (state == TEST_PATTERN)
        enterTestPattern();
      else if (state == RADIO)
        enterRadio();
      else if (state == EMULATORS)
        enterEmulators();
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
    // Itens nomeados por SettingsItem (logo acima de drawSettings()). O CORES
    // saiu no port: o canvas do DVI é sempre RGB565 (PORTING.md 3.5).
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
      if (!settingsEditing && settingsSelection == SET_SETUP_PORTAL) {
        startSetupPortal();
        return;
      }
      // Itens só de leitura: não entram em edição.
      if (settingsSelection == SET_VIDEO || settingsSelection == SET_WIFI || settingsSelection == SET_API ||
          settingsSelection == SET_SD_CARD)
        return;
      settingsEditing = !settingsEditing;
      if (!settingsEditing) {
        saveSettings();
        if (state == ERROR_SCREEN)
          return;
      }
    } else if (!settingsEditing && (a == NavAction::LEFT || a == NavAction::RIGHT))
      settingsSelection = (settingsSelection + (a == NavAction::LEFT ? SET_COUNT - 1 : 1)) % SET_COUNT;
    else if (settingsEditing && (a == NavAction::LEFT || a == NavAction::RIGHT)) {
      int d = a == NavAction::LEFT ? -1 : 1;
      if (settingsSelection == SET_VOLUME)
        settings.volume = constrain(settings.volume + d * 5, 0, 100);
      else if (settingsSelection == SET_RADAR_RANGE) {
        int v[] = {50, 100, 250, 500}, i = 0;
        while (i < 3 && v[i] != settings.rangeKm)
          i++;
        settings.rangeKm = v[(i + d + 4) % 4];
      } else if (settingsSelection == SET_RADAR_REFRESH) {
        int v[] = {5, 10, 30}, i = 0;
        while (i < 2 && v[i] != settings.refreshSeconds)
          i++;
        settings.refreshSeconds = v[(i + d + 3) % 3];
      } else if (settingsSelection == SET_AUDIO_OUT) {
        AudioOutput next = settings.audioOutput == AudioOutput::RCA        ? AudioOutput::INTERNAL
                           : settings.audioOutput == AudioOutput::INTERNAL ? AudioOutput::MUTED
                                                                           : AudioOutput::RCA;
        setAudioOutput(next);
        if (state == ERROR_SCREEN)
          return;
      } else if (settingsSelection == SET_VHS_OSD)
        settings.vhsOsd = !settings.vhsOsd;
      else if (settingsSelection == SET_CHANNEL_MODE)
        tvChannel.cycleMode();
      else if (settingsSelection == SET_SLEEP_TIMER)
        sleepTimer.cycle(millis());
      else if (settingsSelection == SET_VHS_WEAR) {
        settings.vhsWear = d > 0 ? vhs::nextWear(settings.vhsWear) : vhs::prevWear(settings.vhsWear);
        // Se ja esta tocando, o filtro tem de saber agora: sem isto a mudanca so
        // valeria no proximo programa.
        if (playing)
          vhsFilter.configure(vhs::presetFor(settings.vhsWear));
      }
    }
    drawSettings();
    return;
  }
  if (state == PHOTO_SHOW) {
    if (a == NavAction::BACK) {
      state = HOME;
      drawHome();
      return;
    }
    const uint32_t agora = millis();
    if (a == NavAction::LEFT)
      photoShow.advance(-1, agora);
    else if (a == NavAction::RIGHT)
      photoShow.advance(1, agora);
    else if (a == NavAction::SELECT)
      photoShow.cycleDwell(agora); // 3 / 5 / 10 / 30 s
    drawPhotos();
    return;
  }
  if (state == TEST_PATTERN) {
    if (a == NavAction::BACK || a == NavAction::HOME) {
      stopTestPattern();
      state = HOME;
      drawHome();
    } else if (a == NavAction::SELECT)
      cycleTestPattern();
    return;
  }
  if (state == RADIO) {
    if (a == NavAction::BACK || a == NavAction::HOME) {
      stopRadio();
      state = HOME;
      drawHome();
    }
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
      // Um toque curto = PLAY/PAUSA; segurar o botão do meio já é VOLTAR/INÍCIO,
      // então aqui só PAUSA/RETOMA faz sentido.
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
  if (state == EMULATORS) {
    // So alcancavel aqui na build standalone: a fruitjam-launcher ja
    // reiniciou a placa dentro de enterEmulators(), antes de devolver o
    // controle para este loop.
    if (a != NavAction::NONE) {
      state = HOME;
      drawHome();
    }
    return;
  }
}
void togglePlayerAudio() {
  if (state != VIDEO_PLAYBACK)
    return;
  // setAudioOutput() já reabre o OSD por 3 s: é na TV que se vê a troca.
  setAudioOutput(audioOutput == AudioOutput::INTERNAL ? AudioOutput::RCA : AudioOutput::INTERNAL);
}

// Diagnóstico pela USB. Os comandos nunca imprimem credenciais nem gravam
// ajustes de teste.
void diagnosticStatus() {
  JsonDocument d;
  d["build"] = "fruitjam-2026-09-25";
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
  // SRAM e PSRAM pela camada fj/Platform.h. O arduino-pico não guarda o mínimo
  // histórico da heap, então "min_heap" saiu: repetiria o livre atual.
  d["heap"] = ESP.getFreeHeap();
  d["heap_total"] = ESP.getHeapSize();
  d["psram"] = ESP.getFreePsram();
  d["psram_total"] = ESP.getPsramSize();
  d["cpu_mhz"] = ESP.getCpuFreqMHz();
  d["audio_output"] = int(audioOutput.load());
  d["audio_rate"] = audioout::rate();
  d["volume"] = playbackVolume.load();
  d["title"] = currentTitle;
  d["error"] = lastError;
  d["wifi_connected"] = network.connected();
  d["ip"] = network.connected() ? network.ip() : String();
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
  // Mesmo mutex de saveSettings(): sem ele, um provisionamento pelo serial
  // durante um vídeo disputaria o FatFS com a tarefa de áudio sem exclusão
  // mútua nenhuma.
  if (sdMutex)
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  bool saved = ca.length() ? storage::replace(SD, String(CA_FILE), ca) : true;
  saved = saved && secretsStore.save(cfg, currentSettings());
  if (sdMutex)
    xSemaphoreGive(sdMutex);
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
// Confere os dois caminhos de cor do jpegDraw: RGB565 nativo (rgb565_t, com o
// filtro de fita) e RGB565 com os bytes trocados (swap565_t, o
// RGB565_BIG_ENDIAN do JPEGDEC). Os dois têm de chegar ao sprite com a mesma
// cor; se só o segundo falhar, o tipo do ponteiro e o formato do JPEGDEC
// discordam (AGENTS.md, armadilhas 10 e 11).
void testPixelColors() {
  LGFX_Sprite sample;
  sample.setColorDepth(16);
  if (!sample.createSprite(3, 2)) {
    Serial.println("[DIAG] RGB565 colors FAIL (sem memoria)");
    return;
  }
  uint16_t values[] = {0xf800, 0x07e0, 0x001f};
  uint16_t swapped[3];
  for (int i = 0; i < 3; ++i)
    swapped[i] = (uint16_t)((values[i] >> 8) | (values[i] << 8));
  sample.pushImage(0, 0, 3, 1, reinterpret_cast<const lgfx::rgb565_t *>(values));
  sample.pushImage(0, 1, 3, 1, reinterpret_cast<const lgfx::swap565_t *>(swapped));
  bool nativo = true, trocado = true;
  for (int i = 0; i < 3; ++i) {
    nativo = nativo && sample.readPixel(i, 0) == values[i];
    trocado = trocado && sample.readPixel(i, 1) == values[i];
  }
  sample.deleteSprite();
  Serial.printf("[DIAG] RGB565 colors %s (nativo:%s big-endian:%s)\n", nativo && trocado ? "PASS" : "FAIL",
                nativo ? "ok" : "ERRO", trocado ? "ok" : "ERRO");
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
    if (command == "diag mem") {
      // SRAM e PSRAM livres pela camada fj/Platform.h. O arduino-pico não expõe
      // o maior bloco contíguo nem o mínimo histórico, então este comando não
      // os imprime: um palpite aqui já decidiria errado se a pilha de uma
      // tarefa cabe.
      Serial.printf("[MEM] sram_livre=%u de %u  psram_livre=%u de %u  cpu=%u MHz  previsao=%s\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
                    (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize(),
                    (unsigned)ESP.getCpuFreqMHz(), weatherStatus.load());
      return;
    }
    if (command == "diag radio") {
      {
        const uint32_t ch = radioPcmCalls.load(), fr = radioFramesOut.load();
        const uint32_t dt = millis() - radioSince.load();
        const unsigned long fps = dt ? (unsigned long)((uint64_t)fr * 1000 / dt) : 0;
        Serial.printf("[PERF] chamadas=%lu decode_medio=%luus max=%luus i2s_medio=%luus  "
                      "quadros/s=%lu (precisa 22050)\n",
                      (unsigned long)ch, (unsigned long)(ch ? radioPcmUs.load() / ch : 0),
                      (unsigned long)radioPcmMaxUs.load(),
                      (unsigned long)(ch ? radioWriteUs.load() / ch : 0), fps);
      }
      Serial.printf("[RADIO] ativo=%d estado=%s anel=%u%% taxa=%d canais=%d bitrate=%d "
                    "recebidos=%lu reconexoes=%lu underruns=%lu\n",
                    (int)radioActive.load(), radioStream.stateLabel(),
                    (unsigned)radioStream.bufferPercent(), radioStream.sourceRate(),
                    radioStream.sourceChannels(), radioStream.bitrateKbps(),
                    (unsigned long)radioStream.bytesReceived(),
                    (unsigned long)radioStream.reconnects(), (unsigned long)audioUnderruns);
      return;
    }
    if (command == "diag time") {
      const time_t agora = time(nullptr);
      struct tm local, utc;
      localtime_r(&agora, &local);
      gmtime_r(&agora, &utc);
      char sl[32], su[32];
      strftime(sl, sizeof(sl), "%Y-%m-%d %H:%M:%S", &local);
      strftime(su, sizeof(su), "%Y-%m-%d %H:%M:%S", &utc);
      // Sem RTC no Fruit Jam: a fonte é o NTP do ESP32-C6 ou nenhuma.
      Serial.printf("[HORA] local=%s  utc=%s  fonte=%s  TZ=%s  offset=%lds\n", sl, su,
                    rtcclock::sourceLabel(), rtcclock::timezoneString(), rtcclock::utcOffsetSeconds());
      return;
    }
    if (command == "diag fb") {
      Serial.printf("[FB] profundidade=%d bits  %dx%d  bytes=%d\n",
                    (int)(tv.getColorDepth() & 0xFF), tv.width(), tv.height(),
                    tv.width() * tv.height() * ((tv.getColorDepth() & 0xFF) / 8));
      memset(line, 0, sizeof(line));
      continue;
    }
    if (command == "diag cores") {
      // Conta cores RGB565 distintas no framebuffer inteiro. Se o valor bater
      // com o do arquivo, o caminho digital (cartão, JPEGDEC, blit) está intacto.
      static uint16_t linha[CRT_W];
      static uint8_t vistos[8192]; // bitmap de 65536 valores
      memset(vistos, 0, sizeof(vistos));
      uint32_t distintas = 0;
      for (int y = 0; y < CRT_H; ++y) {
        tv.readRect(0, y, CRT_W, 1, linha);
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
        tv.readRect(0, y, CRT_W, 1, linha);
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
                    tv.textWidth(tickerFull.c_str()), TICKER_Y, TICKER_H);
      for (int y = TICKER_Y - 2; y < TICKER_Y + TICKER_H + 2; ++y) {
        tv.readRect(0, y, CRT_W, 1, linha);
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
    if (command == "diag audio toggle") {
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
    } else if (command == "diag emulators") {
      // Na build fruitjam-launcher isto reinicia a placa de verdade (ver
      // enterEmulators()) -- util para testar o protocolo com o lancador
      // sem precisar navegar ate o item EMULADORES pelos tres botoes.
      stopProgram();
      enterEmulators();
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
    // "rca"/"internal" ficam pelos scripts do Core2; "fone"/"alto" são os
    // nomes do Fruit Jam. Nada disto é gravado no cartão.
    else if (command == "diag audio rca" || command == "diag audio tv" || command == "diag audio fone")
      audioOutput = AudioOutput::RCA;
    else if (command == "diag audio internal" || command == "diag audio alto")
      audioOutput = AudioOutput::INTERNAL;
    else if (command == "diag audio mute" || command == "diag audio mudo")
      audioOutput = AudioOutput::MUTED;
    else {
      Serial.println("[DIAG] ERROR unknown command");
      continue;
    }
    osdUntil = millis() + 3000;
    diagnosticStatus();
  }
}

static void appSetup() {
  // USB CDC: quando o buffer da placa enche, o host segura o envio, então o
  // JSON de provisionamento (com o ca.pem) não se perde. No ESP32 a UART
  // precisava de setRxBufferSize(4096); aqui não existe nem faz falta.
  Serial.begin(115200);
  // Ordem FIXA (PORTING.md 2.1). O reset limpo dos periféricos vem antes de
  // tudo; o DVI sobe antes de qualquer tarefa, porque a interrupção de linha
  // fica no núcleo que chamou; e o DAC só é configurado DEPOIS de acordar o
  // ESP32-C6, que pulsa o GPIO 22 e zera o TLV320 junto — ao contrário, o áudio
  // morre mudo sem erro nenhum.
  board::begin();
  if (!display::begin()) {
    // Sem framebuffer não há tela onde mostrar o erro: sobra o serial.
    lastError = PTBR::FALHA_VIDEO;
    state = ERROR_SCREEN;
    Serial.printf("[M5RETRO] ERRO: %s\n", PTBR::FALHA_VIDEO);
    return;
  }
  // Aplica o fuso de Brasília. Sem RTC no Fruit Jam, a hora fica desconhecida
  // até o NTP do ESP32-C6 responder, e a interface mostra --:--.
  rtcclock::begin();
  // Semente do modo aleatório. Sem RTC o time() do boot é sempre o mesmo; quem
  // varia a cada ligada é o gerador de hardware.
  tvChannel.seed((uint32_t)time(nullptr) ^ (uint32_t)esp_random());
  input.begin();
  input.setAutoRepeat(true);
  jpegBuffer = (uint8_t *)ps_malloc(MAX_JPEG);
  bootStep(PTBR::INICIANDO);
  if (!jpegBuffer) {
    setError(PTBR::MEMORIA_INSUFICIENTE);
    return;
  }
  bootStep(PTBR::VERIFICANDO_SD);
  // SDIO próprio: o cartão não divide barramento com nada, ao contrário do
  // Core2, onde o VSPI era do LCD também.
  if (!storage::begin()) {
    setError(PTBR::CARTAO_SD_NAO_ENCONTRADO);
    return;
  }
  Serial.println("[M5RETRO] SD montado (SDIO)");
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
  // Rádio antes do DAC (ver acima). Falhar aqui não impede o uso offline: o
  // NetworkManager só vai continuar sem conectar.
  bootStep(PTBR::CONECTANDO_WIFI);
  if (!net::beginRadio())
    Serial.println("[M5RETRO] ERRO: ESP32-C6 nao respondeu; seguindo sem rede");
  bootStep(PTBR::INICIANDO_AUDIO);
  // O volume do usuário é aplicado em software (playback::scalePcm), como no
  // original. O volume digital do DAC fica em 0 dB: somar os dois atenuaria em
  // dobro.
  audioout::setVolume(100);
  if (!audioout::begin(22050)) {
    setError(PTBR::FALHA_AUDIO);
    return;
  }
  bootStep(PTBR::CARREGANDO_CONFIG);
  if (!loadConfiguration()) {
    apiStatus = PTBR::CONFIG_REDE_AUSENTE;
    dualText(PTBR::CONFIG_REDE_AUSENTE, PTBR::EDITE_SECRETS);
  }
  // NetworkManager tenta conectar sem travar a interface.
  audioOutput = settings.audioOutput;
  // Núcleo 1: o loop() (decodificação e desenho) roda no 0, junto da interrupção
  // de linha do DVI. Pilha em bytes, como no ESP-IDF (fj/Platform.h).
  if (xTaskCreatePinnedToCore(audioTask, "RCA_PCM", 4096, nullptr, 4, &audioTaskHandle, 1) != pdPASS) {
    setError(PTBR::FALHA_AUDIO);
    return;
  }
  while (!audioReady && !audioFailed)
    delay(1);
  if (audioFailed) {
    setError(PTBR::MEMORIA_INSUFICIENTE);
    return;
  }
  // Decode de vídeo no núcleo 1. Sem ela o player cai no decode síncrono do
  // upstream (mais lento, mas funciona), então a falha só vai para o serial.
  if (!startVideoDecodeTask())
    Serial.println("[M5RETRO] ERRO: tarefa de decode nao subiu; decode no loop()");
  bootStep(PTBR::SISTEMA_PRONTO);
  bootSplashHold();
  // Offline playback must not be hidden behind network setup. The portal stays
  // available from CONFIGURACOES when the owner wants to add Wi-Fi later.
  bootReady = true;
  idleMgr.reset(millis());
  state = HOME;
  drawHome();
}
// Repinta a tela corrente do zero. Usada quando o gerente anti-queima sai de
// ESCURO ou APAGADO: nesses estagios o conteudo foi escurecido ou apagado, e
// nao existe um "desfazer" -- tem de desenhar de novo.
static void redrawCurrentScreen() {
  switch (state) {
  case HOME: drawHome(); break;
  case VIDEO_LIBRARY: drawLibrary(); break;
  case AIRCRAFT_RADAR: drawRadar(); break;
  case SETTINGS: drawSettings(); break;
  case SYSTEM_INFO: drawInfo(); break;
  case MUSIC_BROWSER: drawMusicBrowser(); break;
  case MUSIC_NOW_PLAYING: drawMusicNowPlaying(); break;
  case FILE_TRANSFER: drawTransferFrame(); break;
  case PHOTO_SHOW: drawPhotos(); break;
  case RADIO: drawRadioScreen(true); break;
  case TEST_PATTERN: drawTestPattern(); break;
  case EMULATORS: drawEmulatorsInfo(); break;
  // WEATHER repinta sozinha no proprio ritmo; VIDEO_PLAYBACK e TEST_PATTERN
  // nunca chegam aqui porque o gerente nao escala nelas. SETUP_PORTAL e
  // ERROR_SCREEN sao transitorias. Na build fruitjam-launcher, EMULATORS
  // reinicia a placa antes que o gerente anti-queima tenha chance de repintar.
  default: break;
  }
}

static void appLoop() {
  if (state == ERROR_SCREEN && !display::ready()) {
    // Boot parou antes do vídeo: não há tela nem cartão, só o serial.
    delay(100);
    return;
  }
  servicePowerOff();
  if (powerOffPending)
    return;
  serviceDiagnostics();
  input.setAutoRepeat(state != VIDEO_PLAYBACK);
  input.update();
  // Acompanha a hora do NTP (limitado a 1 Hz por dentro).
  rtcclock::poll();
  // Temporizador de desligar, como o dos videocassetes. timeReached() por
  // dentro, entao a volta do millis() nao adia o desligamento por 49 dias.
  // Relogio do radio: so os digitos que mudaram, uma vez por segundo. Repintar
  // a tela inteira a cada segundo pisca e gasta a toa.
  if (state == RADIO) {
    const uint32_t agora = millis();
    if (!radioLastDraw || agora - radioLastDraw >= 250) {
      radioLastDraw = agora;
      drawRadioScreen(false);
    }
  }
  if (sleepTimer.expired(millis())) {
    sleepTimer.cancel();
    Serial.println("[M5RETRO] temporizador venceu; desligando");
    requestPowerOff();
  }
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
    // Configuração salva: o NINA não faz ponto de acesso e estação juntos, então
    // o AP cai antes de a rede nova ser tentada. 1,5 s de folga para o celular
    // receber a página de "salvo"; depois disso o NetworkManager conecta (com
    // o próprio backoff) e o resultado aparece no menu e em SISTEMA.
    if (portalSavePending && millis() - portalSaveStarted >= 1500) {
      portalSavePending = false;
      portal.stop();
      network.begin(currentSecrets());
      apiStatus = "CONECTANDO WI-FI";
      state = HOME;
      drawHome();
    }
  } else {
    serviceWiFi();
    handleNavigation(input.getAction());
  }
  if (state == VIDEO_PLAYBACK) {
    videoTick();
    // Vinheta entre programas do modo canal. drawBumper() so repinta o cartao
    // inteiro na primeira chamada de cada vinheta; depois so a barra cresce,
    // entao chamar a cada volta do loop nao pisca.
    if (tvChannel.bumperActive()) {
      waitVideoDecodeIdle();
      const uint32_t agora = millis();
      tvChannel.drawBumper(&tv, 0, 0, nullptr, agora);
      const int proximo = tvChannel.ready(agora);
      if (proximo >= 0) {
        librarySelection = proximo;
        if (!startProgram(libraryProgramAt(proximo)) && !tvChannel.failed(millis()))
          setError(PTBR::VIDEO_CORROMPIDO); // failed() ja tentou os seguintes
      }
    }
    drawPlaybackOsd();
    if (playbackFinished) {
      playbackFinished = false;
      stopProgram();
      if (audioStreamError)
        setError(PTBR::AUDIO_INVALIDO);
      else if (videoReadError)
        setError(PTBR::VIDEO_CORROMPIDO);
      else if (tvChannel.finished(millis())) {
        // Modo canal ligado: fica em VIDEO_PLAYBACK exibindo a vinheta, e o
        // bloco do loop abaixo inicia o programa seguinte quando ela acabar.
        Serial.println("[M5RETRO] fim do programa; canal emenda o proximo");
      } else
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
    if (musicScopeOn)
      audioscope::tick(&tv, 0, 0, audioScope, audioTap, millis());
  }
  if (state == FILE_TRANSFER) {
    transferTick();
  }
  // Protecao do tubo. A reproducao de video e o padrao de teste se defendem
  // sozinhos -- imagem em movimento nao queima fosforo -- entao o gerente so
  // escala nas telas paradas.
  const bool telaParada = state != VIDEO_PLAYBACK && state != TEST_PATTERN;
  const burnin::Stage estagio = telaParada ? idleMgr.tick(millis()) : burnin::STAGE_ACTIVE;
  if (!telaParada)
    idleMgr.notifyActivity(millis());
  if (idleMgr.takeRepaint()) {
    redrawCurrentScreen();
    // Sem backlight para baixar, o estágio ESCURO escurece o próprio quadro,
    // uma vez, logo depois do redesenho (BurnIn.h, dimFrame).
    if (estagio == burnin::STAGE_DIM)
      burnin::dimFrame((uint16_t *)tv.getBuffer(), (size_t)tv.width() * tv.height());
  }
  if (estagio == burnin::STAGE_BLANK) {
    // Protetor de tela: um bloco andando no preto. Apaga onde estava e pinta
    // onde esta, 192 px de cada, a cada 200 ms -- barato o bastante para ficar
    // horas ligado.
    if (idleMgr.takeSaverStep()) {
      tv.fillRect(idleMgr.saverPrevX(), idleMgr.saverPrevY(), burnin::SAVER_W, burnin::SAVER_H,
                   (uint16_t)TFT_BLACK);
      tv.fillRect(idleMgr.saverX(), idleMgr.saverY(), burnin::SAVER_W, burnin::SAVER_H,
                   burnin::SAVER_COLOR);
    }
  } else if (state == HOME && estagio != burnin::STAGE_DIM && millis() - lastClockDraw >= 1000)
    // No ESCURO o relógio para: redesenhá-lo sairia em brilho normal por cima
    // do quadro escurecido.
    drawHomeClock(true);
  pollAircraft();
  if (state == AIRCRAFT_RADAR && radarDirty) {
    radarDirty = false;
    drawRadar();
  }
  if (millis() - lastStats >= 1000) {
    lastStats = millis();
    uint32_t avg = decodedFrames ? jpegDecodeTotalMs / decodedFrames : 0;
    // RSSI só com a rede de pé: cada leitura é uma transação no SPI do ESP32-C6,
    // disputada com as tarefas de rede (net::Lock).
    const int rssi = network.connected() ? network.rssi() : 0;
    Serial.printf(
        "[M5RETRO] FPS TV:%lu FPS JPEG:%lu frames descartados:%lu JPEG medio:%lu JPEG "
        "maximo:%lu underruns audio:%lu SRAM livre:%u PSRAM livre:%u RSSI:%d API:%s\n",
        (unsigned long)renderedFrames, (unsigned long)decodedFrames, (unsigned long)droppedFrames,
        (unsigned long)avg, (unsigned long)jpegDecodeMaxMs, (unsigned long)audioUnderruns.load(),
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), rssi, apiStatus.c_str());
    renderedFrames = decodedFrames = 0;
    jpegDecodeTotalMs = jpegDecodeMaxMs = 0;
  }
  // Soneca adaptativa. A cadência normal de 4 ms é folgada para 30 quadros/s
  // (33 ms cada) e devolve o núcleo às outras tarefas. Só quando o vídeo já está
  // atrás do relógio de PCM é que 4 ms de latência passam a custar quadro, e aí
  // o loop praticamente não dorme.
  delay(state == VIDEO_PLAYBACK && videoBehind ? 1 : 4);
}
// Pilha do loop(): no arduino-pico com FreeRTOS, a tarefa CORE0 que roda setup()
// e loop() nasce com 1024 PALAVRAS (4 KB), valor fixo em freertos-main.cpp. No
// ESP32 eram 8 KB, e este firmware conta com isso: deserializeJson recursivo,
// printf com float no diag bench, desenho com fontes. Em vez de remendar o
// framework, setup() só cria uma tarefa com pilha folgada no MESMO núcleo 0 —
// display::begin() precisa rodar no núcleo 0, que é onde a interrupção de linha
// do DVI fica — e o loop() do Arduino apenas dorme.
static constexpr uint32_t APP_STACK_BYTES = 16 * 1024;

static void appTask(void *) {
  appSetup();
  for (;;)
    appLoop();
}

void setup() {
  xTaskCreatePinnedToCore(appTask, "APP", APP_STACK_BYTES, nullptr, configMAX_PRIORITIES / 2,
                          nullptr, 0);
}

void loop() { vTaskDelay(portMAX_DELAY); }
