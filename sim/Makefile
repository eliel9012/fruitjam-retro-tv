# ============================================================================
#  Simulador de telas do Fruit Jam Retro TV — build nativo (macOS / Linux) com
#  SDL2.
#
#  Usa a LovyanGFX, a mesma biblioteca gráfica do firmware (ver
#  include/fj/Gfx.h), com o backend SDL dela (lgfx/v1/platforms/sdl). Por
#  padrão reaproveita a cópia que o PlatformIO baixa para o ambiente fruitjam,
#  então rode `.venv/bin/pio run` na raiz do repositório antes do primeiro
#  `make` aqui — ou aponte para outro clone com LGFX=/caminho/LovyanGFX/src.
#
#    make          compila  -> sim/build/m5sim
#    make run      compila e abre a janela
#    make probes   compila as bancadas de sim/probes
#    make cxx11    confere os headers compartilhados em C++11 (não linka nada)
#    make clean
# ============================================================================

LGFX ?= ../.pio/libdeps/fruitjam/LovyanGFX/src

CXX      ?= c++
CC       ?= cc
# LGFX_SDL escolhe o backend SDL explicitamente, em vez de depender da sondagem
# por __has_include que a LovyanGFX faz sozinha.
CFLAGS   := -O2 -g -w -DLGFX_SDL -I$(LGFX)
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare -DLGFX_SDL
# A LovyanGFX inclui <SDL2/SDL.h>, então além do que o sdl2-config devolve
# (.../include/SDL2) é preciso o diretório pai, para resolver o prefixo SDL2/.
HAVE_SDL := $(shell command -v sdl2-config 2>/dev/null)
ifneq ($(HAVE_SDL),)
SDL_PREFIX := $(shell sdl2-config --prefix)
SDL_CFLAGS := -I$(SDL_PREFIX)/include $(shell sdl2-config --cflags)
LDFLAGS    := $(shell sdl2-config --libs) -lpthread
endif
CFLAGS   += $(SDL_CFLAGS)
CXXFLAGS += -I$(LGFX) -I../include -Icompat $(SDL_CFLAGS)

BUILD := build
BIN   := $(BUILD)/m5sim

# A LovyanGFX se compila como "unity build": lgfx_v1.cpp puxa os .inl do núcleo
# e do backend escolhido (LGFX_SDL). Os utilitários em C (qoi, qrcode, pngle,
# tjpgd, miniz) e as fontes CJK são referenciados pelo LGFXBase, então entram
# no link. Os backends de MCU ficam de fora por garantia.
EXCLUDE := -not -path '*/platforms/*'
LGFX_CPP := $(shell find $(LGFX) -name '*.cpp' $(EXCLUDE) 2>/dev/null)
LGFX_C   := $(shell find $(LGFX) -name '*.c' $(EXCLUDE) 2>/dev/null)
OBJ := $(BUILD)/main.o \
	$(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(LGFX_CPP))) \
	$(patsubst %.c,$(BUILD)/%.c.o,$(notdir $(LGFX_C)))

VPATH := src:$(sort $(dir $(LGFX_CPP)) $(dir $(LGFX_C)))

# Probes: cada arquivo em sim/probes/*.cpp vira um binário próprio em
# build/probe_<nome>, linkado contra a mesma LovyanGFX+SDL. Serve para
# desenvolver um componente isolado (ícones, efeitos, OSD) sem mexer no
# simulador principal nem no Makefile.
PROBE_SRC := $(wildcard probes/*.cpp)
PROBE_BIN := $(patsubst probes/%.cpp,$(BUILD)/probe_%,$(PROBE_SRC))
LGFX_OBJ := $(filter-out $(BUILD)/main.o,$(OBJ))

.PHONY: all run clean check check-sdl probes cxx11

all: check check-sdl $(BIN)

probes: check check-sdl $(PROBE_BIN)

SHARED_HDRS := $(wildcard ../include/*.h) $(wildcard compat/*.h)

# Ordem do link: objetos da LovyanGFX ANTES do código do simulador. O
# ~Panel_sdl() tira o painel de uma std::list global da biblioteca; com o
# GNU ld, quem vem primeiro no link é construído primeiro e destruído por
# último, e na ordem inversa essa lista já foi destruída quando o painel
# estático do simulador morre — segfault na saída, depois de tudo pronto (o
# --png gravava os arquivos e mesmo assim devolvia 139).
$(BUILD)/probe_%: probes/%.cpp $(LGFX_OBJ) $(SHARED_HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LGFX_OBJ) $< $(LDFLAGS) -o $@

# O arduino-pico compila o firmware em gnu++17, mas os headers compartilhados
# com o upstream (m5-retro-tv, que compila em -std=gnu++11) continuam em C++11
# para o fork poder puxar correções de lá (PORTING.md 3.10). Sem esta checagem,
# um constexpr com laço passa aqui e só quebra lá — foi exatamente o que
# aconteceu com a VcrFont. Todo header novo entra nesta lista.
#
# Só confere sintaxe, sem linkar: roda mesmo sem SDL instalado. Nesse caso a
# LovyanGFX é apontada para o backend de framebuffer do Linux, que só precisa
# dos headers do sistema.
CXX11_HDRS := VcrFont.h VcrOsd.h WeatherIcons.h ScreenFx.h SafeArea.h TransferScreen.h \
              ChannelMode.h TestPattern.h Subtitles.h PhotoShow.h RtcClock.h \
              RadioStream.h RadioScreen.h DiarioLogo.h AudioScope.h BurnIn.h VhsFx.h \
              Ascii.h PlaybackIO.h Id3.h SafeStorage.h BootSplash.h
ifneq ($(HAVE_SDL),)
CXX11_FLAGS := $(filter-out -std=c++17,$(CXXFLAGS))
else
CXX11_FLAGS := $(filter-out -std=c++17 -DLGFX_SDL,$(CXXFLAGS)) -DLGFX_LINUX_FB
endif

cxx11: check
	@for h in $(CXX11_HDRS); do \
	  printf '#include "%s"\nint main(){return 0;}\n' "$$h" \
	    | $(CXX) -std=c++11 -x c++ - $(CXX11_FLAGS) -fsyntax-only -o /dev/null \
	    || { echo "FALHOU em C++11: $$h"; exit 1; }; \
	  echo "ok C++11: $$h"; \
	done

check:
	@test -f "$(LGFX)/LovyanGFX.hpp" || { \
	  echo "LovyanGFX nao encontrada em $(LGFX)."; \
	  echo "Rode '.venv/bin/pio run' na raiz do repositorio primeiro,"; \
	  echo "ou passe o caminho: make LGFX=/caminho/para/LovyanGFX/src"; \
	  exit 1; }

check-sdl:
	@test -n "$(HAVE_SDL)" || { \
	  echo "SDL2 ausente: brew install sdl2 (macOS) ou apt-get install libsdl2-dev"; \
	  exit 1; }

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# O simulador também depende dos headers compartilhados: sem isto, mexer num
# header não recompilava o main.o e o binário velho seguia conferindo a
# diagramação antiga.
$(BUILD)/main.o: $(SHARED_HDRS)

$(BIN): $(OBJ)
	$(CXX) $(LGFX_OBJ) $(BUILD)/main.o $(LDFLAGS) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

run: all
	./$(BIN)

clean:
	rm -rf $(BUILD)
