#pragma once
// Saída de áudio do Fruit Jam: TLV320DAC3100 (I2C 0x18) alimentado por I2S em
// PIO (BCLK 26, WS 27, DOUT 24, MCLK 25).
//
// Substitui o par "módulo RCA (I2S1) / alto-falante do Core2 (M5.Speaker)" do
// firmware original. O DAC tem as duas saídas no mesmo chip, então trocar de
// rota é mudar registrador, não desmontar driver:
//
//   Route::HEADPHONE  conector P2 (3,5 mm) — o que vai para a entrada de áudio
//                     da TV. Faz o papel do antigo AudioOutput::RCA.
//   Route::SPEAKER    alto-falante da placa (amplificador classe D do DAC).
//                     Faz o papel do antigo AudioOutput::INTERNAL.
//   Route::MUTED      as duas desligadas; o I2S continua correndo.
//
// O DVI do HSTX não leva áudio (é DVI, não HDMI com ilhas de dados): a TV fica
// muda pelo cabo de vídeo.
//
// Concorrência: write() é chamado SÓ pela tarefa de áudio (RCA_PCM). setRoute,
// setVolume e setRate podem vir do loop(); a implementação serializa.
#include <Arduino.h>

namespace audioout {

enum class Route : uint8_t { HEADPHONE, SPEAKER, MUTED };

// Configura o DAC por I2C e liga o I2S na taxa pedida. Precisa vir DEPOIS de
// net::beginRadio() (ver fj/Board.h, armadilha do GPIO 22). Idempotente.
bool begin(uint32_t sampleRate = 22050);
void end();
bool ready();

// Troca a taxa de amostragem (reconfigura I2S e PLL do DAC). Silencia por alguns
// ms. Devolve false se a taxa não é suportada.
bool setRate(uint32_t sampleRate);
uint32_t rate();

void setRoute(Route r);
Route route();

// 0..100, mapeado para o volume digital do DAC (em dB, não linear em amplitude).
void setVolume(uint8_t percent);

// Escreve quadros estéreo intercalados (L, R, L, R...) de 16 bits. Bloqueia até
// caber tudo no buffer do I2S. Devolve quantos QUADROS foram aceitos.
size_t write(const int16_t *interleaved, size_t frames);

// Quantos quadros cabem agora sem bloquear.
size_t availableForWrite();

// Enche o buffer de silêncio (equivale ao i2s_zero_dma_buffer do ESP-IDF).
void flushSilence();

} // namespace audioout
