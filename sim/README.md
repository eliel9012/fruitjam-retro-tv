# Simulador de telas

Roda as telas do Retro TV numa janela do Mac/Linux, usando o backend **SDL da
LovyanGFX** (`lgfx::Panel_sdl`) — o mesmo rasterizador e as mesmas fontes bitmap
do firmware (ver `include/fj/Gfx.h`). Serve para conferir diagramação (sobretudo
o que escapa da área segura) sem ligar o Fruit Jam na TV.

As constantes de geometria vêm de [`include/SafeArea.h`](../include/SafeArea.h),
o mesmo arquivo que o firmware usa, então simulador e aparelho não divergem.

## O que NÃO é simulado

O restante do firmware depende de periféricos do RP2350. Ficam de fora: Wi-Fi
(ESP32-C6), cartão SD, áudio (TLV320), decodificação MJPEG, FreeRTOS e os
botões. Os dados exibidos são de exemplo, fixos.

Isto também **não** simula a saída de vídeo: o DVI 640x480 (o quadro 320x240
dobrado) e o overscan que a sua TV aplicar pela HDMI só se conferem com o Fruit
Jam ligado nela. A guia magenta desenhada na tela é uma marcação da área segura
teórica (~7% de cada borda), não uma medição do seu aparelho.

O painel SDL da LovyanGFX nasce 240x320 (retrato). O simulador e os probes
chamam `sim::configure(panel)` (`compat/SimPanel.h`) antes do `setPanel()`
para ele virar 320x240 — esquecer isso não dá erro, só corta a tela em 240 px.

## Dependências

```sh
brew install sdl2            # macOS
sudo apt-get install libsdl2-dev   # Debian/Ubuntu
```

A LovyanGFX é reaproveitada do que o PlatformIO já baixou para o ambiente
`fruitjam`, então rode uma vez na raiz do repositório antes do primeiro build:

```sh
.venv/bin/pio run
```

`make -C sim cxx11` (checagem de C++11 dos headers compartilhados) só precisa
da LovyanGFX, não do SDL.

## Compilar e rodar

```sh
make -C sim run
```

Para apontar para outra cópia da LovyanGFX:

```sh
make -C sim LGFX=/caminho/para/LovyanGFX/src
```

## Exportar PNGs (headless)

```sh
./sim/build/m5sim --png docs/screens/rca
```

Desenha as telas e grava um PNG 320x240 de cada uma, sem abrir a janela.
Serve para conferir diagramação em CI — inclusive um teste automático de que
nada escapou da área segura.

## Teclas

| Tecla | Ação |
|---|---|
| ← / → | tela anterior / próxima |
| 1 a 8 | ir direto para uma tela |
| G | liga/desliga a guia de área segura |
| ESC | fechar |

Telas: início, biblioteca, player, radar, previsão do tempo, configurações,
sistema e música.
