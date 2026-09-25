#pragma once
// Saída de vídeo do Fruit Jam: DVI pelo HSTX do RP2350, 640x480 @ 60 Hz,
// com o quadro lógico de 320x240 dobrado nos dois eixos.
//
// Por que 320x240 e não 640x480: todo o firmware (área segura, fontes bitmap,
// OSD, previsão do tempo) foi desenhado para 320x240, e um framebuffer RGB565
// de 640x480 teria 614 KB — mais que a SRAM inteira. 320x240 em RGB565 são
// 153.600 B, que cabem na SRAM interna com folga.
//
// O canvas `tv` é um LGFX_Sprite cujo buffer É o framebuffer do DVHSTX. Desenhar
// nele é desenhar na tela; não há cópia nem "flush".
//
// Ordem de bytes: o LGFX_Sprite de 16 bits guarda RGB565 com os bytes trocados
// (big-endian, herança do SPI), e o DVHSTX espera little-endian. Em vez de
// converter 76.800 pixels por quadro, reconfiguramos o codificador TMDS do HSTX
// para ler os campos já trocados (ver Display.cpp). Custo em tempo de execução:
// zero.
#include "fj/Gfx.h"

namespace display {

constexpr int W = 320, H = 240;

// Liga o DVI e aponta `tv` para o framebuffer. Chamar uma vez, no setup(),
// ANTES de criar qualquer tarefa (a interrupção de linha fica no núcleo que
// chamou). Devolve false se não houve memória para o framebuffer.
bool begin();

bool ready();

// Espera o retraço vertical (início do próximo quadro). Útil para trocar o
// conteúdo da tela sem rasgo visível; não chame no caminho do vídeo sem medir.
void waitVsync();

} // namespace display

// O canvas da TV: 320x240, RGB565. Era o `rca` (M5ModuleRCA) no firmware
// original. Válido só depois de display::begin().
extern LGFX_Sprite tv;
