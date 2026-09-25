#pragma once
// Ponte de compatibilidade só do simulador: probes escritos para o upstream
// (M5GFX + Panel_sdl) continuam compilando contra a LovyanGFX do fork sem
// edição. O M5GFX é um fork da LovyanGFX com a mesma API de desenho; o que ele
// acrescenta (autodetecção de painel das placas M5) não existe no simulador.
//
// Código novo NÃO usa isto: inclui "fj/Gfx.h", declara lgfx::LGFX_Device e
// chama sim::configure() (SimPanel.h) no painel.
#include "SimPanel.h"

class M5GFX : public lgfx::LGFX_Device {
public:
  // O Panel_sdl do M5GFX nascia 320x240; o da LovyanGFX nasce 240x320. Quem
  // chegou aqui pelo nome M5GFX espera o primeiro.
  void setPanel(lgfx::Panel_sdl *panel) {
    if (panel)
      sim::configure(*panel);
    lgfx::LGFX_Device::setPanel(panel);
  }
  void setPanel(lgfx::Panel_Device *panel) { lgfx::LGFX_Device::setPanel(panel); }
};
