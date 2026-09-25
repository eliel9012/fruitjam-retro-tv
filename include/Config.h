#pragma once
#include <Arduino.h>
// Constantes gerais do Fruit Jam Retro TV.
//
// Pinos NAO moram aqui: estao em include/fj/Board.h, com os nomes do variant do
// arduino-pico. Duas constantes descrevendo o mesmo pino ja deram bug neste
// repositorio (AGENTS.md, armadilha 8), entao nao ha copia.
namespace cfg {
// Pastas continuam em /M5RETRO para o mesmo cartao servir no Core2 e no Fruit
// Jam (PORTING.md 3.9).
constexpr char ROOT[] = "/M5RETRO";
constexpr char VIDEOS[] = "/M5RETRO/videos";
constexpr char CONFIG[] = "/M5RETRO/config";
constexpr char SETTINGS[] = "/M5RETRO/config/settings.json";
constexpr char SECRETS[] = "/M5RETRO/config/secrets.json";
constexpr char CA[] = "/M5RETRO/config/ca.pem";
constexpr char CACHE[] = "/M5RETRO/cache/aircraft.json";
// Quadro logico da TV (o DVI dobra para 640x480) e o maior video aceito: no
// Fruit Jam o video pode ocupar a tela inteira.
constexpr uint16_t CRT_W = 320, CRT_H = 240, VIDEO_W = 320, VIDEO_H = 240;
constexpr size_t MAX_JPEG = 128 * 1024, AUDIO_CHUNK = 1024;
constexpr uint32_t AUDIO_RATE = 22050;
constexpr char VERSION[] = "FRUIT JAM RETRO TV v0.1";
constexpr char VERSION_SHORT[] = "v0.1";
// Nome do dono na abertura (include/BootSplash.h). ASCII; a abertura converte
// para maiúsculas e corta no que cabe.
constexpr char OWNER[] = "ELIEL";
} // namespace cfg
