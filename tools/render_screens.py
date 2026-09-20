#!/usr/bin/env python3
"""Renderiza mockups fiéis (320x240) das telas do firmware M5 RETRO TV e do
firmware Weather Channel, para documentação no README.

Gera PNGs em 640x480 (upscale NEAREST 2x do framebuffer 320x240) em docs/screens/.

Uso:  .venv/bin/python3 tools/render_screens.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
SCALE = 2

FONT_DIR = "/System/Library/Fonts/Supplemental"
MONO = os.path.join(FONT_DIR, "Courier New.ttf")
MONO_BOLD = os.path.join(FONT_DIR, "Courier New Bold.ttf")

# --- RGB565 -> RGB888 -------------------------------------------------------
def c565(v):
    return (((v >> 11) & 0x1F) * 255 // 31,
            ((v >> 5) & 0x3F) * 255 // 63,
            (v & 0x1F) * 255 // 31)

# Cores TFT_* do M5GFX
BLACK      = c565(0x0000)
NAVY       = c565(0x000F)
BLUE       = c565(0x001F)
CYAN       = c565(0x07FF)
WHITE      = c565(0xFFFF)
YELLOW     = c565(0xFFE0)
DARKCYAN   = c565(0x03EF)
GREEN      = c565(0x07E0)

def _font(bold, size):
    return ImageFont.truetype(MONO_BOLD if bold else MONO, size)

S = _font(False, 11)   # setTextSize(1)
M = _font(True, 19)    # setTextSize(2)
L = _font(True, 27)    # setTextSize(3)

class Screen:
    def __init__(self):
        self.img = Image.new("RGB", (W, H), NAVY)
        self.d = ImageDraw.Draw(self.img)

    # primitivas equivalentes ao M5GFX
    def fill_screen(self, color): self.d.rectangle([0, 0, W, H], fill=color)
    def rect(self, x, y, w, h, color): self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=color)
    def roundrect(self, x, y, w, h, r, color): self.d.rounded_rectangle([x, y, x + w, y + h], r, fill=color)
    def roundrect_outline(self, x, y, w, h, r, color):
        self.d.rounded_rectangle([x, y, x + w, y + h], r, outline=color, width=1)
    def hline(self, x, y, w, color): self.d.line([x, y, x + w - 1, y], fill=color)
    def vline(self, x, y, h, color): self.d.line([x, y, x, y + h - 1], fill=color)
    def circle(self, cx, cy, r, color):
        self.d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=color, width=1)
    def triangle(self, x, y, w, h, color):  # fillTriangle simples
        self.d.polygon([(x, y), (x - w, y + h), (x + w, y + h)], fill=color)

    def text(self, s, x, y, color, font=S, datum="tl", bg=None):
        f = font
        bb = self.d.textbbox((0, 0), s, font=f)
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        if datum == "mc":  # middle_center
            px, py = x - tw // 2, y - th // 2
        else:  # top_left
            px, py = x, y
        if bg is not None:
            self.d.rectangle([px, py, px + tw, py + th], fill=bg)
        self.d.text((px - bb[0], py - bb[1]), s, font=f, fill=color)

    def save(self, path):
        big = self.img.resize((W * SCALE, H * SCALE), Image.NEAREST)
        big.save(path)

def back_button(s):
    s.roundrect(280, 4, 36, 30, 4, BLUE)
    s.roundrect_outline(280, 4, 36, 30, 4, CYAN)
    s.d.polygon([(287, 19), (297, 10), (297, 28)], fill=WHITE)  # seta
    s.rect(296, 16, 12, 6, WHITE)

def controller_labels(s, left, center, right):
    s.rect(0, 184, W, 56, NAVY)
    for i, label in enumerate([left, center, right]):
        x = 6 if i == 0 else 110 if i == 1 else 214
        w = 100
        hot = (i == 1)  # destaque ilustrativo no botao central
        fill = CYAN if hot else BLUE
        ink = NAVY if hot else WHITE
        s.roundrect(x, 190, w, 43, 4, fill)
        s.roundrect_outline(x, 190, w, 43, 4, WHITE)
        s.text(label, x + w // 2, 211, ink, S, "mc")

def header(s, title):
    s.text(title, 12, 8, WHITE, M)
    s.hline(8, 36, 304, CYAN)

# ============================================================================
def home():
    s = Screen()
    items = ["VIDEOS", "TRAFEGO AEREO", "CONFIGURACOES", "SISTEMA"]
    s.text("M5 RETRO TV", 12, 8, WHITE, M)
    s.hline(8, 36, 304, CYAN)
    for i, it in enumerate(items):
        y = 48 + i * 32
        sel = (i == 0)
        if sel:
            s.roundrect(12, y - 3, 296, 28, 4, CYAN)
        s.text(("> " if sel else "  ") + it, 24, y, NAVY if sel else WHITE, M)
    controller_labels(s, "ACIMA", "OK", "ABAIXO")
    s.save("docs/screens/home.png")

def library():
    s = Screen()
    header(s, "VIDEOS")
    programs = ["Primeiro Teste", "Filme Retro", "Desenho Anos 80"]
    count = len(programs)
    for row in range(min(4, count)):
        idx = row
        y = 50 + row * 32
        sel = (idx == 0)
        if sel:
            s.roundrect(12, y - 2, 296, 28, 4, CYAN)
        s.text(("> " if sel else "  ") + programs[idx][:40], 20, y + 6, NAVY if sel else WHITE, S)
    s.text(f"1 / {count}", 216, 16, WHITE, S)
    controller_labels(s, "ANTERIOR", "PLAY", "PROXIMO")
    back_button(s)
    s.save("docs/screens/library.png")

def playback():
    s = Screen()
    # Poster estatico (fallback: fundo + titulo, como drawStaticPoster faria)
    s.fill_screen(NAVY)
    s.roundrect(30, 40, 260, 130, 6, BLUE)
    s.roundrect_outline(30, 40, 260, 130, 6, CYAN)
    s.text("M5 RETRO", 160, 88, YELLOW, M, "mc")
    s.text("PRIMEIRO TESTE", 160, 116, WHITE, M, "mc")
    s.text("CAPA", 160, 140, CYAN, S, "mc")
    # HUD 1Hz (faixa reservada y 204..219): relogio + barra de progresso
    s.rect(8, 204, 192, 16, BLUE)
    s.roundrect_outline(8, 204, 192, 16, 2, CYAN)
    s.text("00:01:24 / 03:27", 12, 207, WHITE, S)
    s.roundrect(12, 216, 184, 3, 1, CYAN)
    s.rect(12, 216, 55, 3, YELLOW)  # ~30% preenchido
    back_button(s)
    s.save("docs/screens/playback.png")

def radar():
    s = Screen()
    s.text("M5 RETRO TV", 10, 7, WHITE, S)
    s.text("TRAFEGO AEREO", 10, 22, WHITE, S)
    s.circle(105, 105, 62, CYAN)
    s.hline(43, 105, 124, DARKCYAN)
    s.vline(105, 43, 124, DARKCYAN)
    s.triangle(105, 70, 3, 6, CYAN)   # selecionado
    s.triangle(90, 120, 3, 6, YELLOW)
    s.triangle(130, 95, 3, 6, YELLOW)
    s.triangle(118, 140, 3, 6, YELLOW)
    s.text("PT-ABC", 190, 62, WHITE, S)
    s.text("FL 350", 190, 82, WHITE, S)
    s.text("412 KT", 190, 98, WHITE, S)
    s.text("22 GRAUS", 180, 145, WHITE, S)
    s.text("CONECTADA", 190, 122, CYAN, S)
    s.text("AERONAVES: 4", 190, 72, YELLOW, S)
    controller_labels(s, "AERONAVE", "DETALHES", "AERONAVE")
    back_button(s)
    s.save("docs/screens/radar.png")

def settings():
    s = Screen()
    header(s, "CONFIGURACOES")
    s.text("> VOLUME", 20, 68, CYAN, M)
    s.text("75%", 34, 110, WHITE, L)
    controller_labels(s, "ACIMA", "OK", "ABAIXO")
    back_button(s)
    s.save("docs/screens/settings.png")

def info():
    s = Screen()
    s.text("SISTEMA", 160, 94, WHITE, M, "mc")
    s.text("MEMORIA: 72880  PSRAM: 4012", 160, 130, CYAN, S, "mc")
    controller_labels(s, "ANTERIOR", "DETALHES", "PROXIMO")
    back_button(s)
    s.save("docs/screens/info.png")

def portal():
    s = Screen()
    s.text("CONFIGURACAO", 160, 94, WHITE, M, "mc")
    s.text("WI-FI: M5RETRO-TV", 160, 130, CYAN, S, "mc")
    s.text("SENHA: 12345678", 26, 162, WHITE, S)
    s.text("ABRA: 192.168.4.1", 26, 180, WHITE, S)
    controller_labels(s, "VOLTAR", "STATUS", "SAIR")
    back_button(s)
    s.save("docs/screens/portal.png")

def error():
    s = Screen()
    s.text("ERRO DO SISTEMA", 160, 94, WHITE, M, "mc")
    s.text("CARTAO SD NAO ENCONTRADO", 160, 130, CYAN, S, "mc")
    s.save("docs/screens/error.png")

def weather():
    s = Screen()
    s.fill_screen(NAVY)
    s.text("FRANCA - SP", 160, 6, YELLOW, M, "mc")
    s.hline(8, 42, 304, CYAN)
    s.text("SUNNY", 160, 54, WHITE, M, "mc")
    s.text("26 C", 160, 104, YELLOW, L, "mc")
    s.text("UMIDADE  62%", 160, 178, WHITE, S, "mc")
    s.text("VENTO  12 KM/H  NW", 160, 198, WHITE, S, "mc")
    s.hline(8, 222, 304, CYAN)
    s.rect(0, 224, W, 16, BLACK)
    s.text("SEG 26/15 SUNNY    TER 27/14 CLOUDY    QUA 25/12 RAIN ...", 160, 228, WHITE, S, "mc")
    s.save("docs/screens/weather.png")

def main():
    os.makedirs("docs/screens", exist_ok=True)
    for fn in (home, library, playback, radar, settings, info, portal, error, weather):
        fn()
        print("ok:", fn.__name__)

if __name__ == "__main__":
    main()
