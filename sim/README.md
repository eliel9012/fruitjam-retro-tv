# Simulador de telas da saída RCA

Roda as telas da saída composta numa janela do Mac/Linux, usando o backend **SDL
do M5GFX** (`lgfx::Panel_sdl`) — o mesmo rasterizador e as mesmas fontes bitmap
do firmware. Serve para conferir diagramação (sobretudo o que escapa da área
segura do tubo) sem ligar o Core2 na TV.

As constantes de geometria vêm de [`include/SafeArea.h`](../include/SafeArea.h),
o mesmo arquivo que o firmware usa, então simulador e aparelho não divergem.

## O que NÃO é simulado

O M5Unified 0.2.10 não tem backend SDL, e o restante do firmware depende de
periféricos do ESP32. Ficam de fora: Wi-Fi, cartão SD, I2S/áudio, decodificação
MJPEG, FreeRTOS, toque e botões. Os dados exibidos são de exemplo, fixos.

Isto também **não** simula o sinal analógico: codificação NTSC, fase da burst,
níveis de IRE e o recorte real de overscan do seu aparelho só se conferem com o
Core2 ligado na TV. A guia magenta desenhada na tela é uma marcação da área
segura teórica (~7% de cada borda), não uma medição do seu tubo.

## Dependências

```sh
brew install sdl2
```

O M5GFX é reaproveitado do que o PlatformIO já baixou, então rode uma vez na
raiz do repositório antes do primeiro build:

```sh
.venv/bin/pio run
```

## Compilar e rodar

```sh
make -C sim run
```

Para apontar para outra cópia do M5GFX:

```sh
make -C sim M5GFX=/caminho/para/M5GFX/src
```

## Exportar PNGs (headless)

```sh
./sim/build/m5sim --png docs/screens/rca
```

Desenha as oito telas e grava um PNG 320x240 de cada uma, sem abrir a janela.
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
