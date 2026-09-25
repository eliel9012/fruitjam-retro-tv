#pragma once
// Saída de áudio do Fruit Jam: TLV320DAC3100 (I2C 0x18) alimentado por I2S em
// PIO (BCLK 26, WS 27, DOUT 24, MCLK 25).
//
// Substitui o par "módulo RCA (I2S1) / alto-falante do Core2 (M5.Speaker)" do
// firmware original. O DAC tem as duas saídas no mesmo chip, então trocar de
// rota é mudar registrador, não desmontar driver:
//
//   Route::HEADPHONE  conector P2 (3,5 mm) — o que vai para a entrada de áudio
//                     da TV. Faz o papel do antigo AudioOutput::RCA. O teto do
//                     volume é nível de linha: alto demais para fone intra-
//                     auricular em 100%.
//   Route::SPEAKER    alto-falante da placa (amplificador classe D do DAC, mono:
//                     toca (L+R)/2). Faz o papel do antigo AudioOutput::INTERNAL.
//   Route::MUTED      as duas desligadas; o I2S continua correndo.
//
// O DVI do HSTX não leva áudio (é DVI, não HDMI com ilhas de dados): a TV fica
// muda pelo cabo de vídeo.
//
// Relógio: o DAC é o MESTRE do I2S (gera BCLK e WS a partir de um MCLK de 12 MHz
// tirado do cristal). Quem dita o ritmo do write() é o DAC, com a precisão do
// cristal — o "relógio = PCM entregue" do player continua valendo. O porquê e a
// sequência de registradores estão no topo de src/fj/AudioOut.cpp.
//
// Concorrência: write(), availableForWrite() e flushSilence() são da tarefa de
// áudio (RCA_PCM). setRoute, setVolume e setRate podem vir do loop(); a
// implementação serializa com um mutex que o write() NÃO segura enquanto espera
// espaço no buffer.
//
// setRoute/setVolume/setRate podem ser chamados antes de begin() (por exemplo
// ao aplicar o settings.json no boot): o valor fica guardado e o begin() o usa.
#include <Arduino.h>

namespace audioout {

enum class Route : uint8_t { HEADPHONE, SPEAKER, MUTED };

// Configura o DAC por I2C e liga o I2S na taxa pedida. Precisa vir DEPOIS de
// net::beginRadio() (ver fj/Board.h, armadilha do GPIO 22). Idempotente: com o
// DAC já no ar, equivale a setRate(). Devolve false se o DAC não responde no
// I2C ou a taxa não é suportada.
bool begin(uint32_t sampleRate = 22050);
void end();
// true com o DAC configurado e tocando. Cai para false se o DAC sumir (reset
// pelo GPIO 22 sem reconfiguração possível); o write() tenta reconfigurar sozinho.
bool ready();

// Taxas suportadas: 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000.
// Troca a taxa de amostragem (reprograma o PLL do DAC) com a saída silenciada
// por alguns ms e DESCARTA o que estava na fila (tocaria na taxa errada).
// Devolve false se a taxa não é suportada ou se o DAC não respondeu no I2C (aí
// ready() cai e o write() reconfigura na taxa nova). Antes do begin(), só guarda.
bool setRate(uint32_t sampleRate);
uint32_t rate();

void setRoute(Route r);
Route route();

// 0..100, mapeado para o volume digital do DAC em dB (curva perceptiva, 0 =
// mudo, 100 = 0 dB). Não mexe nas amostras: quem ainda escala o PCM em software
// (playback::scalePcm) atenua duas vezes.
void setVolume(uint8_t percent);
uint8_t volume();

// Escreve quadros estéreo intercalados (L, R, L, R...) de 16 bits. Bloqueia até
// caber tudo no buffer do I2S. Devolve quantos QUADROS foram aceitos — sempre
// `frames`: se o DAC parar de pedir dados (sumiu, ou ficou mais de ~250 ms sem
// relógio), o resto é descartado no ritmo da taxa atual, para a tarefa de áudio
// não disparar nem travar. ready() diz se o som está saindo de fato.
size_t write(const int16_t *interleaved, size_t frames);

// Quantos quadros cabem agora sem bloquear. Sem DAC, devolve um bloco inteiro
// (o write() descarta no ritmo certo).
size_t availableForWrite();

// Joga fora o que está na fila e deixa o DAC tocando silêncio (equivale ao
// i2s_zero_dma_buffer do ESP-IDF). Leva alguns ms (rampa de mudo do DAC).
void flushSilence();

} // namespace audioout
