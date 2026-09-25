#include "fj/Display.h"

#include <Adafruit_dvhstx.h>

#include "hardware/structs/hstx_ctrl.h"

LGFX_Sprite tv;

namespace {
// Buffer simples: dois quadros de 153.600 B levariam 60% da SRAM. O custo é um
// eventual rasgo horizontal no vídeo, que aparece menos que no CVBS porque o
// DVI varre a 60 Hz e o filme anda a no máximo 30.
// Usa o driver pimoroni::DVHSTX direto, e não o wrapper DVHSTX16 do Adafruit:
// o wrapper esconde o objeto do driver, e é ele quem sabe esperar o retraço.
pimoroni::DVHSTX dvi;
bool ok = false;

// Codificador TMDS para RGB565 com os bytes trocados.
//
// Cada pixel lógico chega ao HSTX como uma palavra de 32 bits com o mesmo pixel
// nas duas metades (o DVHSTX dobra a largura com `pixel * 0x10001`). Com os bytes
// trocados, a metade baixa fica:
//
//   bits 15..13 = G2 G1 G0   bits 12..8 = B4..B0
//   bits  7..3  = R4..R0     bits  2..0 = G5 G4 G3
//
// Cada pista TMDS pega os NBITS+1 bits do topo do byte baixo depois de uma
// rotação à direita de ROT bits (rotação de 32, não de 16).
//   vermelho: já está em 7..3                  -> ROT 0,  NBITS 4
//   azul:     12..8 para 7..3                  -> ROT 5,  NBITS 4
//   verde:    G5..G3 (2..0) para 7..5 e G2..G0 (bits 31..29 — a cópia na metade
//             alta, graças à duplicação) para 4..2 -> rotação à esquerda de 5,
//             isto é ROT 27, NBITS 5.
// O truque do verde depende da duplicação horizontal: vale para o modo 320x240
// (h_repeat_shift = 1), que é o único que usamos.
void configureSwappedRgb565() {
  hstx_ctrl_hw->expand_tmds = 4u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |   //
                              0u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |     //
                              5u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |   //
                              27u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |    //
                              4u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |   //
                              5u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
}
} // namespace

namespace display {

bool begin() {
  if (ok)
    return true;
  if (!dvi.init(W, H, pimoroni::DVHSTX::MODE_RGB565, false, DVHSTX_PINOUT_DEFAULT))
    return false;
  configureSwappedRgb565();
  tv.setColorDepth(lgfx::rgb565_2Byte);
  tv.setBuffer(dvi.get_back_buffer<uint16_t>(), W, H, lgfx::rgb565_2Byte);
  tv.fillScreen(TFT_BLACK);
  ok = true;
  return true;
}

bool ready() { return ok; }

void waitVsync() {
  if (ok)
    dvi.wait_for_vsync();
}

} // namespace display
