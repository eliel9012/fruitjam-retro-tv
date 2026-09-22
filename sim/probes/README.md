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
#include <SDL2/SDL.h>   // antes do M5GFX: define SDL_h_
#include <M5GFX.h>
#include "SafeArea.h"   // geometria compartilhada com o firmware

static lgfx::Panel_sdl panel;
static M5GFX rca;

int main(int, char **) {
  panel.setScaling(3, 3);
  rca.setPanel(&panel);
  if (!rca.init()) return 1;
  rca.setColorDepth(8);        // RGB332, igual ao firmware na saída composta

  // ... desenhe aqui ...

  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, crt::W, crt::H);
  FILE *f = fopen("build/probe.png", "wb");
  fwrite(png, 1, len, f); fclose(f); free(png);
  return 0;
}
```

Regras: um probe não edita `sim/src/main.cpp`, `sim/Makefile` nem `src/main.cpp`.
