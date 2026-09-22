# ============================================================================
#  Simulador de telas da saída RCA — build nativo (macOS / Linux) com SDL2.
#
#  Reaproveita o M5GFX já baixado pelo PlatformIO, então rode `.venv/bin/pio run`
#  na raiz do repositório antes do primeiro `make` aqui.
#
#    make          compila  -> sim/build/m5sim
#    make run      compila e abre a janela
#    make clean
# ============================================================================

M5GFX ?= ../.pio/libdeps/m5stack-core2/M5GFX@0.2.29/src

CXX      ?= c++
CC       ?= cc
CFLAGS   := -O2 -g -w -I$(M5GFX)
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare
# O M5GFX inclui <SDL2/SDL.h>, então além do que o sdl2-config devolve
# (.../include/SDL2) é preciso o diretório pai, para resolver o prefixo SDL2/.
SDL_PREFIX := $(shell sdl2-config --prefix)
CXXFLAGS += -I$(M5GFX) -I../include -I$(SDL_PREFIX)/include $(shell sdl2-config --cflags)
LDFLAGS  := $(shell sdl2-config --libs)

BUILD := build
BIN   := $(BUILD)/m5sim

# Todo o M5GFX menos os backends de MCU (só o caminho SDL é compilado aqui).
# Os utilitários em C (qoi, qrcode, pngle, tjpgd, miniz) e as fontes CJK são
# referenciados pelo LGFXBase, então entram no link.
EXCLUDE := -not -path '*/platforms/esp32*' \
	-not -path '*/platforms/framebuffer/*' \
	-not -path '*/platforms/arduino*'
M5GFX_CPP := $(shell find $(M5GFX) -name '*.cpp' $(EXCLUDE) 2>/dev/null)
M5GFX_C   := $(shell find $(M5GFX) -name '*.c' $(EXCLUDE) 2>/dev/null)
OBJ := $(BUILD)/main.o \
	$(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(M5GFX_CPP))) \
	$(patsubst %.c,$(BUILD)/%.c.o,$(notdir $(M5GFX_C)))

VPATH := src:$(sort $(dir $(M5GFX_CPP)) $(dir $(M5GFX_C)))

.PHONY: all run clean check

all: check $(BIN)

check:
	@test -d "$(M5GFX)" || { \
	  echo "M5GFX nao encontrado em $(M5GFX)."; \
	  echo "Rode '.venv/bin/pio run' na raiz do repositorio primeiro,"; \
	  echo "ou passe o caminho: make M5GFX=/caminho/para/M5GFX/src"; \
	  exit 1; }
	@command -v sdl2-config >/dev/null || { echo "SDL2 ausente: brew install sdl2"; exit 1; }

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

run: all
	./$(BIN)

clean:
	rm -rf $(BUILD)
