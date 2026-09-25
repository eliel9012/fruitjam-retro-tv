#pragma once
// Geometria do painel SDL do simulador.
//
// O Panel_sdl da LovyanGFX nasce 240x320 (retrato, o padrão de LCD SPI da
// biblioteca); o do M5GFX, que o simulador usava antes do port, nascia
// 320x240. Sem esta chamada as telas saem cortadas em 240 px de largura — e
// tudo "funciona", sem erro nenhum, só que conferindo a diagramação errada.
// Chamar ANTES de setPanel()/init().
#include "fj/Gfx.h"
#include "SafeArea.h"

namespace sim {

inline void configure(lgfx::Panel_sdl &panel) {
  auto cfg = panel.config();
  cfg.memory_width = cfg.panel_width = crt::W;   // 320
  cfg.memory_height = cfg.panel_height = crt::H; // 240
  panel.config(cfg);
}

} // namespace sim
