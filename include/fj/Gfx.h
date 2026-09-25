#pragma once
// Uma única porta de entrada para a biblioteca gráfica.
//
// No aparelho: LovyanGFX (mesma API do M5GFX, que é um fork dela), desenhando
// num LGFX_Sprite cujo buffer é o próprio framebuffer do DVI (ver fj/Display.h).
// No simulador de PC: LovyanGFX com o backend SDL.
//
// Todo header compartilhado inclui ESTE arquivo, nunca <M5GFX.h> nem
// <LovyanGFX.hpp> direto, para que firmware e simulador vejam a mesma coisa.
#include <LovyanGFX.hpp>

// Tipo "qualquer superfície desenhável": o canvas da TV, um sprite, o painel do
// simulador. Era M5GFX* no firmware original.
using GfxTarget = lgfx::LovyanGFX;
