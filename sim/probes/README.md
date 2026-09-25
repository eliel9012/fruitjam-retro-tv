# Probes

Bancada isolada para desenvolver um componente gráfico sem mexer no simulador
principal. Cada `.cpp` aqui vira um binário próprio:

```sh
make -C sim probes
./sim/build/probe_<nome>
```

Esqueleto mínimo (o `main` é seu; a janela é opcional — para conferir o
resultado prefira gravar um PNG com `createPng`, que funciona headless):

```cpp
#include "fj/Gfx.h"     // LovyanGFX + backend SDL, a mesma porta do firmware
#include "SimPanel.h"   // sim::configure(): painel em 320x240
#include "SafeArea.h"   // geometria compartilhada com o firmware

static lgfx::Panel_sdl panel;
static lgfx::LGFX_Device tv;

int main(int, char **) {
  panel.setScaling(3, 3);
  sim::configure(panel);       // a LovyanGFX nasce 240x320; sem isto a tela sai cortada
  tv.setPanel(&panel);
  if (!tv.init()) return 1;
  tv.setColorDepth(16);        // RGB565, igual ao canvas `tv` do Fruit Jam

  // ... desenhe aqui ...

  size_t len = 0;
  uint8_t *png = (uint8_t *)tv.createPng(&len, 0, 0, crt::W, crt::H);
  FILE *f = fopen("build/probe.png", "wb");
  fwrite(png, 1, len, f); fclose(f); free(png);
  return 0;
}
```

Regras: um probe não edita `sim/src/main.cpp`, `sim/Makefile` nem `src/main.cpp`.

Probe antigo, escrito para o M5GFX (`#include <M5GFX.h>`, `static M5GFX rca;`),
continua compilando: `compat/M5GFX.h` é uma ponte que já configura o painel em
320x240. Código novo usa o esqueleto acima.
