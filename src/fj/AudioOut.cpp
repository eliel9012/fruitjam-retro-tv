// Saída de áudio do Fruit Jam: TLV320DAC3100 + I2S em PIO. Contrato em
// fj/AudioOut.h.
//
// ============================================================================
// Quem gera o relógio — e por que é o DAC
// ============================================================================
//
// O caminho "óbvio" (e o dos exemplos da Adafruit) é o RP2350 ser mestre do I2S
// e o PLL do DAC travar no BCLK. Não serve aqui, por três motivos:
//
// 1. O datasheet exige PLL_CLKIN/P >= 512 kHz. Com 16 bits estéreo o BCLK é
//    32 x fs: 8000 Hz dá 256 kHz e 11025 Hz dá 352,8 kHz — fora da faixa, e o
//    rádio usa essas taxas.
// 2. O PIO do arduino-pico chega às taxas de áudio com divisor FRACIONÁRIO: o
//    BCLK treme um ciclo de 240 MHz. A própria Adafruit documenta (driver
//    CircuitPython do TLV320) que no Fruit Jam o PLL não trava nesse BCLK e o
//    som sai chiado e distorcido, pior abaixo de 22050 Hz — a taxa dos vídeos.
// 3. O MCLK por PIO do arduino-pico usa divisor INTEIRO do clk_sys; a 240 MHz
//    (DVHSTX) isso erra o MCLK em vários por cento.
//
// Solução: o DAC vira MESTRE do I2S.
//
//   cristal 12 MHz --GPOUT3 (div 1)--> GPIO 25 = MCLK --PLL do DAC--> BCLK/WS
//                                                                   |
//   RP2350: PIO em modo escravo, só desloca DOUT no ritmo que chega <-+
//
// - MCLK = o próprio XOSC de 12 MHz, pelo gerador de clock GPOUT3 do RP2350
//   (que sai exatamente no GPIO 25). Não depende do clk_sys, que o DVHSTX mexe,
//   nem ocupa fatia de PWM.
// - 12 MHz é a entrada "de livro" do TLV320: as duas famílias de taxa saem com
//   J.D exato, sem erro nenhum de frequência.
// - fs sai do cristal, exato. O DMA do I2S consome no ritmo do DAC, então não
//   há deriva entre quem manda e quem toca (sem amostra pulada/repetida).
//
// ============================================================================
// Tabela de relógios (MCLK = 12 MHz, P = 1, R = 1, NDAC = 8, MDAC = 2)
// ============================================================================
//
//   família   J.D      PLL_CLK       taxas (DOSR)
//   48 kHz    8,1920   98,304 MHz    48000 (128), 32000 (192), 24000 (256),
//                                    16000 (384),  8000 (768)
//   44,1 kHz  7,5264   90,3168 MHz   44100 (128), 22050 (256), 11025 (512)
//
//   fs = PLL_CLK / (NDAC x MDAC x DOSR); DAC_MOD_CLK = PLL_CLK/16 = DOSR x fs
//   (6,144 ou 5,6448 MHz — o máximo que a faixa 2,8..6,2 MHz permite, isto é, o
//   maior sobreamostragem possível em cada taxa). BCLK = DAC_MOD_CLK / (DOSR/32)
//   = 32 x fs: EXATAMENTE 32 bits por quadro, que é o que o programa PIO escravo
//   de 16 bits espera (um `pull` por quadro L+R). Restrições do datasheet
//   conferidas em tempo de compilação no static_assert abaixo.
//
// ============================================================================
// Sequência de registradores (P = página, R = registrador, decimal)
// ============================================================================
//
// begin() e a reconfiguração depois de um reset:
//   P0 R1    = 0x01   reset por software (o GPIO 22 é de board/net, NUNCA daqui)
//   P0 R4    = 0x03   PLL_CLKIN = MCLK, CODEC_CLKIN = PLL_CLK
//   P0 R27   = 0x0C   I2S, 16 bits, BCLK e WCLK como SAÍDA (DAC mestre)
//   P0 R29   = 0x01   BCLK sai do DAC_MOD_CLK; BCLK/WCLK param com o DAC desligado
//   P0 R65/66= 0x81   volume digital -63,5 dB
//   P0 R64   = 0x0C   DAC mudo
//   -- relógios (ver clocksOn) --
//   P0 R5..8          PLL: P = 1, R = 1, J, D
//   P0 R5    |= 0x80  liga o PLL; espera 10 ms pela trava
//   P0 R11   = 0x88   NDAC = 8 ligado
//   P0 R12   = 0x82   MDAC = 2 ligado
//   P0 R13/14= DOSR
//   P0 R30   = 0x80|N divisor do BCLK ligado, N = DOSR/32
//   -- analógico, página 1 --
//   P1 R33   = 0xBE   anti-estalo: DAC só desliga depois dos drivers; driver
//                     liga em 304 ms com degraus de 3,9 ms
//   P1 R35   = 0x44   DAC_L -> mixer esquerdo, DAC_R -> mixer direito
//   P1 R36/37= 0x8C   mixer -> HPL/HPR com -6 dB (teto = nível de linha na TV)
//   P1 R38   = 0x92   mixer esquerdo -> classe D com -9 dB
//   P1 R40/41= 0x04   HPL/HPR: ganho 0 dB, sem mudo
//   P1 R42   = 0x0C   classe D: ganho +12 dB, sem mudo
//   -- rota (applyRoute) --
//   P1 R31            drivers do fone ligados só em HEADPHONE, modo comum 1,65 V
//   P1 R32   bit 7    classe D ligado só em SPEAKER
//   P0 R63   = 0xE8   DACs ligados, dados TROCADOS, passo suave de 1 amostra
//            = 0xF8   idem, mas DAC esquerdo = (L+R)/2 (SPEAKER, que é mono)
//   -- volume (applyVolume) --
//   P0 R65/66         volume do usuário; P0 R64 = 0x00 tira o mudo
//
// Os ganhos fixos seguem o que a Adafruit usa no Fruit Jam (alto-falante de
// 8 ohm/1 W com classe D em +12 dB e analógico em -10 dB; teto "seguro" do
// volume digital em +1,5 dB): aqui o 100% dá +3 dB no alto-falante, abaixo do
// teto deles. O volume digital nunca passa de 0 dB — acima disso o filtro
// interno do DAC satura com áudio a plena escala.
//
// setRate: mudo (rampa) -> R63 DACs off -> R30/R11/R12 off -> PLL off ->
//          reinicia o I2S (fila descartada; sem BCLK, nenhum DMA em voo) ->
//          relógios on -> rota -> volume. É a ordem que o datasheet pede:
//          desliga do fim para o começo, liga do começo para o fim.
// setRoute: mudo (rampa) -> drivers/caminho da rota nova -> volume.
// flushSilence: mudo (rampa) -> R30 off (BCLK para) -> reinicia o I2S -> R30 on
//          -> volume.
//
// Por que os dados vão "trocados" (R63): o I2S do arduino-pico manda cada palavra
// de 32 bits do FIFO a partir do bit 31, e a metade de cima sai no canal com WS
// baixo (esquerdo). O par int16 {L, R} na memória little-endian, lido como
// uint32, é R<<16 | L — o R sairia no slot esquerdo. Em vez de gastar CPU
// trocando metades (ou mais um buffer na SRAM), o DAC pega o slot direito para o
// canal esquerdo e vice-versa. Custo zero.
//
// Sem detecção de fone: o GPIO1 do DAC (a saída de interrupção que a detecção
// usaria) está no GPIO 23, o MESMO fio do GPIO0 do ESP32-C6 (pino de boot do
// rádio). Ligar a interrupção do DAC ali mexeria no coprocessador de rede.
//
// Recuperação: se o ESP32-C6 for resetado de novo (o WiFiNINA pulsa o GPIO 22
// depois de um WiFi.end()), o DAC volta ao padrão de fábrica — escravo, sem
// relógio — e o DMA do I2S para. O write() percebe a fila parada por 250 ms,
// reconfigura o DAC do zero e segue; enquanto não consegue, descarta o PCM no
// ritmo da taxa, para a tarefa de áudio não travar nem disparar.
//
// NÃO TESTADO NO APARELHO. Os pontos a conferir primeiro com o Fruit Jam na mão:
// BCLK de 32 x fs saindo do DAC no GPIO 26 (osciloscópio), canais L/R na ordem
// certa, nível de linha na TV e o estalo ao trocar de rota.

#include "fj/AudioOut.h"

#include "fj/Platform.h"

#include <Adafruit_TLV320DAC3100.h>
#include <I2S.h>
#include <Wire.h>

#include <atomic>
#include <math.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"

namespace audioout {
namespace {

// ---- pinos -----------------------------------------------------------------
constexpr pin_size_t PIN_DOUT = PIN_I2S_DATAOUT; // 24
constexpr pin_size_t PIN_BCLK = PIN_I2S_BITCLK;  // 26; o WS é o seguinte (27)
constexpr pin_size_t PIN_MCLK = PIN_I2S_MCLK;    // 25 = saída do GPOUT3
static_assert(PIN_I2S_WORDSEL == PIN_I2S_BITCLK + 1, "o I2S do arduino-pico exige WS = BCLK + 1");
static_assert(PIN_I2S_MCLK == 25, "so o GPIO 25 e ligado ao gerador GPOUT3");
static_assert(XOSC_HZ == 12000000, "a tabela de PLL supoe cristal de 12 MHz");

// ---- buffers do I2S --------------------------------------------------------
// 10 buffers de 256 quadros (1 KB cada) + 1 de silêncio = 11 KB de SRAM. Dois
// ficam presos no DMA; os outros oito somam 2048 quadros de folga: 46 ms a
// 44,1 kHz, 93 ms a 22050 Hz — a mesma fila do original (8 x 256 no ESP32).
constexpr size_t BUF_COUNT = 10;
constexpr size_t BUF_FRAMES = 256; // um quadro estéreo de 16 bits = uma palavra

// ---- relógios --------------------------------------------------------------
constexpr uint8_t NDAC = 8, MDAC = 2;
struct RateCfg {
  uint32_t rate;
  uint8_t j;     // parte inteira do multiplicador do PLL
  uint16_t d;    // parte fracionária, em décimos de milésimo (J.D = J + D/10000)
  uint16_t dosr; // sobreamostragem do DAC
};
constexpr RateCfg RATES[] = {
    {8000, 8, 1920, 768},  {11025, 7, 5264, 512}, {16000, 8, 1920, 384}, {22050, 7, 5264, 256},
    {24000, 8, 1920, 256}, {32000, 8, 1920, 192}, {44100, 7, 5264, 128}, {48000, 8, 1920, 128},
};
constexpr size_t RATE_COUNT = sizeof(RATES) / sizeof(RATES[0]);

// Restrições do datasheet do TLV320DAC3100 (seção de relógios), com D != 0:
// 10 MHz <= MCLK/P <= 20 MHz, R = 1, 80 MHz <= PLL_CLK <= 110 MHz;
// 2,8 MHz < DOSR x fs < 6,2 MHz; DOSR múltiplo de 8 (filtro A do bloco PRB_P1);
// MDAC x DOSR >= 32 x RC (RC = 8 no PRB_P1); e fs exato. Recursivo, sem laço,
// para continuar valendo em C++11.
constexpr uint64_t pllHz(const RateCfg &c) { return ((uint64_t)c.j * 10000u + c.d) * (XOSC_HZ / 10000u); }
constexpr bool rateOk(const RateCfg &c) {
  return pllHz(c) == (uint64_t)c.rate * NDAC * MDAC * c.dosr && pllHz(c) >= 80000000u &&
         pllHz(c) <= 110000000u && (uint64_t)c.rate * c.dosr > 2800000u &&
         (uint64_t)c.rate * c.dosr < 6200000u && c.dosr % 32 == 0 && c.dosr / 32 <= 128 &&
         MDAC * c.dosr >= 32 * 8;
}
constexpr bool allRatesOk(size_t i = 0) { return i >= RATE_COUNT || (rateOk(RATES[i]) && allRatesOk(i + 1)); }
static_assert(allRatesOk(), "tabela de relogios fora das restricoes do TLV320DAC3100");

const RateCfg *findRate(uint32_t rate) {
  for (const RateCfg &c : RATES)
    if (c.rate == rate)
      return &c;
  return nullptr;
}

// ---- ganhos fixos do caminho analógico (ver topo) --------------------------
// Índices da tabela 6-24 do datasheet: no começo dela, ~0,5 dB por passo.
constexpr uint8_t HP_ANALOG_ATT = 12;  // -6 dB
constexpr uint8_t SPK_ANALOG_ATT = 18; // -9 dB

// ---- estado ----------------------------------------------------------------
Adafruit_TLV320DAC3100 codec;
I2S i2s(OUTPUT);
SemaphoreHandle_t mtx = nullptr; // criado no primeiro begin() e nunca apagado

std::atomic<bool> isReady{false}; // DAC configurado e I2S no ar
std::atomic<bool> begun{false};   // begin() já deu certo: vale tentar recuperar
std::atomic<uint32_t> curRate{22050};
std::atomic<Route> curRoute{Route::HEADPHONE};
std::atomic<uint8_t> curVolume{75}; // o padrão do settings.json

// Só com o mutex tomado:
bool i2sConfigured = false;
bool i2sRunning = false;
bool dacMuted = true;

// Só da tarefa de áudio (write/flushSilence):
uint64_t drainDueUs = 0;
uint32_t lastRecoveryMs = 0;

constexpr uint32_t STALL_US = 250000;      // fila parada assim = DAC sem relógio
constexpr uint32_t RECOVERY_GAP_MS = 2000; // entre tentativas de reconfigurar

struct Guard {
  Guard() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mtx); }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
};

// Tempo da rampa de mudo: com passo suave de uma amostra por 0,5 dB, de 0 dB
// até o mudo são 127 passos (16 ms a 8 kHz, 3 ms a 44,1 kHz).
uint32_t muteRampMs() {
  const uint32_t r = curRate.load();
  return (127u * 1000u + r - 1) / r + 1;
}

// 0..100 -> passos de 0,5 dB (0 = 0 dB). Curva de potência 2,5 em amplitude
// (50·log10): 75% = -6 dB, 50% = -15 dB, 25% = -30 dB, 10% = -50 dB. Fica entre
// a lei quadrática e a cúbica, as duas aproximações usuais de "volume que soa
// linear" para uma faixa de 40 a 60 dB.
int volumeToHalfDb(uint8_t percent) {
  if (percent >= 100)
    return 0;
  if (percent == 0) // log10(0) = -inf; quem chama já trata 0 como mudo
    return -127;
  const int steps = (int)lroundf(100.0f * log10f(percent / 100.0f)); // 2 x 50·log10
  return steps < -127 ? -127 : steps;
}

// ---- MCLK ------------------------------------------------------------------
void startMclk() {
  // Divisor 1: o cristal sai direto. Sem PLL do RP2350 no meio, sem jitter do
  // clk_sys, e continua certo quando o DVHSTX sobe o relógio para 240 MHz.
  clock_gpio_init(PIN_MCLK, CLOCKS_CLK_GPOUT3_CTRL_AUXSRC_VALUE_XOSC_CLKSRC, 1.0f);
}

void stopMclk() {
  hw_clear_bits(&clocks_hw->clk[clk_gpout3].ctrl, CLOCKS_CLK_GPOUT0_CTRL_ENABLE_BITS);
  gpio_set_function(PIN_MCLK, GPIO_FUNC_NULL);
}

// ---- operações no DAC (todas com o mutex tomado) ---------------------------
bool setDacMuteLocked(bool mute) {
  const bool ok = codec.setDACVolumeControl(mute, mute, TLV320_VOL_INDEPENDENT);
  if (ok)
    dacMuted = mute;
  return ok;
}

// Mudo com rampa antes de mexer em relógio ou rota: é o que evita o estalo.
bool muteForChangeLocked() {
  if (dacMuted)
    return true;
  const bool ok = setDacMuteLocked(true);
  delay(muteRampMs());
  return ok;
}

bool applyVolumeLocked() {
  const uint8_t v = curVolume.load();
  const bool mute = v == 0 || curRoute.load() == Route::MUTED;
  bool ok = true;
  if (!mute) {
    const float db = volumeToHalfDb(v) * 0.5f; // múltiplo de 0,5: exato em float
    ok = codec.setChannelVolume(false, db) && codec.setChannelVolume(true, db);
  }
  return setDacMuteLocked(mute) && ok;
}

// Liga os drivers da rota e (re)liga os DACs. Quem chama já pôs o DAC no mudo.
bool applyRouteLocked() {
  const Route r = curRoute.load();
  const bool hp = r == Route::HEADPHONE, spk = r == Route::SPEAKER;
  bool ok = codec.configureHeadphoneDriver(hp, hp, TLV320_HP_COMMON_1_65V, false);
  ok = codec.enableSpeaker(spk) && ok;
  // O classe D é mono e só ouve o mixer esquerdo: nele vai (L+R)/2.
  const tlv320_dac_path_t left = spk ? TLV320_DAC_PATH_MIXED : TLV320_DAC_PATH_SWAPPED;
  ok = codec.setDACDataPath(true, true, left, TLV320_DAC_PATH_SWAPPED, TLV320_VOLUME_STEP_1SAMPLE) && ok;
  return ok;
}

// Desliga do fim para o começo: DACs, divisores, PLL. Sem DAC ligado e sem
// divisor do BCLK, o BCLK para — e com ele o DMA do I2S.
bool clocksOffLocked() {
  bool ok = codec.setDACDataPath(false, false, TLV320_DAC_PATH_SWAPPED, TLV320_DAC_PATH_SWAPPED,
                                 TLV320_VOLUME_STEP_1SAMPLE);
  ok = codec.setBCLK_N(false, 4) && ok;
  ok = codec.setNDAC(false, NDAC) && ok;
  ok = codec.setMDAC(false, MDAC) && ok;
  ok = codec.powerPLL(false) && ok;
  return ok;
}

// Liga do começo para o fim: PLL (e espera a trava), NDAC, MDAC, DOSR, BCLK.
// Os DACs em si ligam em applyRouteLocked().
bool clocksOnLocked(const RateCfg &c) {
  bool ok = codec.setPLLValues(1, 1, c.j, c.d) && codec.powerPLL(true);
  delay(10);
  ok = ok && codec.setNDAC(true, NDAC) && codec.setMDAC(true, MDAC) && codec.setDOSR(c.dosr);
  ok = ok && codec.setBCLK_N(true, (uint8_t)(c.dosr / 32));
  return ok;
}

// Recria o I2S: é o único jeito de descartar a fila do AudioBufferManager.
// Chamar SÓ com o BCLK parado — sem relógio, nenhum DMA está em voo quando os
// buffers são liberados (a interrupção do DMA pode estar no outro núcleo).
bool restartI2sLocked() {
  if (!i2sConfigured) {
    // Escravo: o PIO só lê BCLK/WS (26/27) e desloca o DOUT (24).
    i2s.setBCLK(PIN_BCLK);
    i2s.setDATA(PIN_DOUT);
    i2s.setBitsPerSample(16);
    i2s.setSlave();
    i2s.setBuffers(BUF_COUNT, BUF_FRAMES, 0);
    i2sConfigured = true;
  }
  if (i2sRunning) {
    i2s.end();
    i2sRunning = false;
  }
  i2sRunning = i2s.begin();
  return i2sRunning;
}

// Configuração inteira a partir do reset. Usada no begin() e na recuperação.
bool configureAllLocked(const RateCfg &c) {
  if (!codec.reset())
    return false;
  dacMuted = true; // o reset deixa R64 = 0x0C
  // Logo depois do reset o DAC é escravo e não gera relógio: momento seguro para
  // (re)criar o I2S.
  bool ok = restartI2sLocked();
  ok = ok && codec.setPLLClockInput(TLV320DAC3100_PLL_CLKIN_MCLK) &&
       codec.setCodecClockInput(TLV320DAC3100_CODEC_CLKIN_PLL);
  ok = ok && codec.setCodecInterface(TLV320DAC3100_FORMAT_I2S, TLV320DAC3100_DATA_LEN_16, true, true);
  ok = ok && codec.setBCLKConfig(false, false, TLV320DAC3100_BCLK_SRC_DAC_MOD_CLK);
  ok = ok && codec.setChannelVolume(false, -63.5f) && codec.setChannelVolume(true, -63.5f);
  ok = ok && setDacMuteLocked(true);
  ok = ok && clocksOnLocked(c);
  ok = ok && codec.configureHeadphonePop(true, TLV320_HP_TIME_304MS, TLV320_RAMP_4MS);
  ok = ok && codec.configureAnalogInputs(TLV320_DAC_ROUTE_MIXER, TLV320_DAC_ROUTE_MIXER);
  ok = ok && codec.setHPLVolume(true, HP_ANALOG_ATT) && codec.setHPRVolume(true, HP_ANALOG_ATT);
  ok = ok && codec.setSPKVolume(true, SPK_ANALOG_ATT);
  ok = ok && codec.configureHPL_PGA(0, true) && codec.configureHPR_PGA(0, true);
  ok = ok && codec.configureSPK_PGA(TLV320_SPK_GAIN_12DB, true);
  ok = ok && applyRouteLocked(); // DACs ligam aqui: o BCLK nasce
  ok = ok && applyVolumeLocked();
  return ok;
}

// Desliga tudo o que begin() ligou. Com o mutex tomado.
void shutdownLocked() {
  if (isReady.load()) {
    muteForChangeLocked();
    codec.configureHeadphoneDriver(false, false, TLV320_HP_COMMON_1_65V, false);
    codec.enableSpeaker(false);
    clocksOffLocked();
  }
  if (i2sRunning) {
    i2s.end();
    i2sRunning = false;
  }
  stopMclk();
  isReady = false;
}

// Reconfigura o DAC do zero. Com o mutex tomado.
bool recoverLocked() {
  lastRecoveryMs = millis();
  const RateCfg *c = findRate(curRate.load());
  // Depois de um reset pelo GPIO 22, R27 volta a 0x00 (escravo). Só para o log:
  // a reconfiguração é completa de qualquer jeito.
  const uint8_t ifc = codec.readRegister(0, TLV320DAC3100_REG_CODEC_IF_CTRL1);
  Serial.printf("[AUDIO] DAC sem relogio: reconfigurando (R27=0x%02X, esperado 0x0C)\n", ifc);
  startMclk();
  const bool ok = c && configureAllLocked(*c);
  if (!ok)
    Serial.println("[AUDIO] TLV320 nao respondeu; PCM sera descartado no ritmo da taxa");
  isReady = ok;
  return ok;
}

// Sem DAC: "toca" os quadros no relógio, sem som, para a tarefa de áudio
// continuar ritmada (o player mede o tempo pelo PCM entregue).
void drainTimed(size_t frames) {
  const uint64_t now = time_us_64();
  if (drainDueUs + 100000u < now) // voltou de um período tocando de verdade
    drainDueUs = now;
  drainDueUs += (uint64_t)frames * 1000000u / curRate.load();
  if (drainDueUs > now) {
    const uint32_t ms = (uint32_t)((drainDueUs - now) / 1000u);
    if (ms)
      vTaskDelay(pdMS_TO_TICKS(ms));
  }
}

} // namespace

// ============================================================================
// API
// ============================================================================

bool begin(uint32_t sampleRate) {
  const RateCfg *c = findRate(sampleRate);
  if (!c)
    return false;
  if (isReady.load())
    return setRate(sampleRate);
  if (!mtx) {
    mtx = xSemaphoreCreateMutex();
    if (!mtx)
      return false;
  }
  Guard g;
  curRate = sampleRate;
  startMclk();
  // Wire0 nos pinos do DAC. Se alguém já abriu o barramento, setSDA/setSCL
  // recusam e begin() não faz nada — os pinos são os mesmos.
  Wire.setSDA(PIN_WIRE0_SDA);
  Wire.setSCL(PIN_WIRE0_SCL);
  Wire.setClock(400000);
  Wire.begin();
  if (!codec.begin(TLV320DAC3100_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("[AUDIO] TLV320 nao responde no I2C (0x18)");
    shutdownLocked();
    return false;
  }
  if (!configureAllLocked(*c)) {
    Serial.println("[AUDIO] falha configurando o TLV320");
    shutdownLocked();
    return false;
  }
  begun = true;
  isReady = true;
  return true;
}

void end() {
  if (!mtx)
    return;
  Guard g;
  if (begun.load() || isReady.load())
    shutdownLocked();
  begun = false;
}

bool ready() { return isReady.load(); }

bool setRate(uint32_t sampleRate) {
  const RateCfg *c = findRate(sampleRate);
  if (!c)
    return false;
  if (!isReady.load()) {
    // Guardada para o begin() ou para a próxima recuperação.
    curRate = sampleRate;
    return true;
  }
  Guard g;
  if (!isReady.load()) {
    curRate = sampleRate;
    return true;
  }
  if (sampleRate == curRate.load())
    return true;
  bool ok = muteForChangeLocked(); // rampa na taxa ANTIGA
  ok = clocksOffLocked() && ok;
  delay(1); // a última palavra que o DMA já empurrou assenta no FIFO
  ok = restartI2sLocked() && ok;
  curRate = sampleRate;
  ok = ok && clocksOnLocked(*c) && applyRouteLocked() && applyVolumeLocked();
  if (!ok)
    isReady = false; // o write() reconfigura do zero na taxa nova
  return ok;
}

uint32_t rate() { return curRate.load(); }

void setRoute(Route r) {
  curRoute = r;
  if (!isReady.load())
    return;
  Guard g;
  if (!isReady.load())
    return;
  bool ok = muteForChangeLocked();
  ok = applyRouteLocked() && ok;
  ok = applyVolumeLocked() && ok;
  if (!ok)
    isReady = false;
}

Route route() { return curRoute.load(); }

void setVolume(uint8_t percent) {
  curVolume = percent > 100 ? 100 : percent;
  if (!isReady.load())
    return;
  Guard g;
  // Sem espera: o DAC faz a rampa de volume sozinho (passo suave).
  if (isReady.load() && !applyVolumeLocked())
    isReady = false;
}

uint8_t volume() { return curVolume.load(); }

size_t write(const int16_t *interleaved, size_t frames) {
  if (!interleaved || !frames)
    return 0;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(interleaved);
  size_t left = frames;
  uint64_t lastProgressUs = time_us_64();
  bool recoveredHere = false;
  while (left) {
    if (!isReady.load()) {
      if (begun.load() && millis() - lastRecoveryMs >= RECOVERY_GAP_MS) {
        Guard g;
        recoverLocked();
      }
      if (!isReady.load()) {
        drainTimed(left);
        return frames;
      }
      recoveredHere = true;
      lastProgressUs = time_us_64();
    }

    size_t n = 0;
    {
      // O mutex só cobre a cópia (não bloqueante) para a fila; a espera por
      // espaço acontece fora dele, para o loop() poder mexer em rota/volume.
      Guard g;
      if (i2sRunning)
        n = i2s.write(p, left * 4) / 4;
    }
    const uint64_t now = time_us_64();
    if (n) {
      p += n * 4;
      left -= n;
      lastProgressUs = now;
      continue;
    }
    if (now - lastProgressUs >= STALL_US) {
      // Fila cheia e parada: o DAC deixou de gerar BCLK. Uma tentativa por
      // chamada; se nem assim andar, descarta o resto no ritmo da taxa.
      if (recoveredHere) {
        isReady = false;
        drainTimed(left);
        return frames;
      }
      {
        Guard g;
        recoverLocked();
      }
      recoveredHere = true;
      lastProgressUs = time_us_64();
      continue;
    }
    // Um buffer de 256 quadros leva 5,8 ms a 44,1 kHz: 1 ms de espera não deixa
    // a fila secar.
    vTaskDelay(1);
  }
  return frames;
}

size_t availableForWrite() {
  if (!isReady.load())
    return BUF_FRAMES;
  Guard g;
  return i2sRunning ? (size_t)i2s.availableForWrite() / 4 : 0;
}

void flushSilence() {
  if (!isReady.load()) {
    drainDueUs = 0;
    return;
  }
  Guard g;
  if (!isReady.load())
    return;
  const RateCfg *c = findRate(curRate.load());
  const uint8_t n = (uint8_t)(c->dosr / 32);
  bool ok = muteForChangeLocked();
  // Para o BCLK (o DAC continua ligado, só o divisor desliga) para recriar o
  // I2S sem DMA em voo.
  ok = codec.setBCLK_N(false, n) && ok;
  delay(1);
  ok = restartI2sLocked() && ok;
  ok = codec.setBCLK_N(true, n) && ok;
  ok = applyVolumeLocked() && ok;
  if (!ok)
    isReady = false;
}

} // namespace audioout
