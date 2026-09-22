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

def airplane(s, x, y, color):
    # aviaozinho top-down apontando para "cima" (norte), como drawAirplane
    s.d.line([x, y - 3, x, y + 3], fill=color)               # fuselagem
    s.d.polygon([(x, y - 3), (x - 2, y + 1), (x + 2, y + 1)], fill=color)  # nariz
    s.d.line([x - 4, y, x + 4, y], fill=color)               # asas
    s.d.line([x - 2, y + 2, x + 2, y + 2], fill=color)       # cauda

# ============================================================================
def home():
    s = Screen()
    items = ["VIDEOS", "MUSICA", "TRAFEGO AEREO", "CONFIGURACOES", "SISTEMA", "TEMPO"]
    s.text("M5 RETRO TV", 12, 8, WHITE, M)
    s.hline(8, 36, 304, CYAN)
    for i, it in enumerate(items):
        y = 40 + i * 22
        sel = (i == 0)
        if sel:
            s.roundrect(12, y - 2, 296, 20, 4, CYAN)
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
    airplane(s, 105, 70, CYAN)   # selecionado
    airplane(s, 90, 120, YELLOW)
    airplane(s, 130, 95, YELLOW)
    airplane(s, 118, 140, YELLOW)
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
    header(s, "SISTEMA")
    y0 = 52
    s.text("MEMORIA LIVRE", 20, y0, CYAN, S)
    s.text("PSRAM LIVRE", 20, y0 + 26, CYAN, S)
    s.text("VERSAO", 20, y0 + 52, CYAN, S)
    s.text("72880 bytes", 140, y0, WHITE, S)
    s.text("4012512 bytes", 140, y0 + 26, WHITE, S)
    s.text("core2", 140, y0 + 52, WHITE, S)
    controller_labels(s, "ANTERIOR", "DETALHES", "PROXIMO")
    back_button(s)
    s.save("docs/screens/info.png")

def info_network():
    s = Screen()
    header(s, "SISTEMA")
    y0 = 52
    s.text("REDE", 20, y0, CYAN, S)
    s.text("SINAL", 20, y0 + 26, CYAN, S)
    s.text("ENDERECO IP", 20, y0 + 52, CYAN, S)
    s.text("CONECTADO", 168, y0, WHITE, S)
    s.text("-38 dBm", 168, y0 + 26, WHITE, S)
    s.text("192.168.15.23", 168, y0 + 52, WHITE, S)
    controller_labels(s, "ANTERIOR", "DETALHES", "PROXIMO")
    back_button(s)
    s.save("docs/screens/info_network.png")

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

def music():
    s = Screen()
    header(s, "MUSICA")
    s.text("/", 12, 42, DARKCYAN, S)
    entries = [("Artista Teste", True), ("Album", True), ("faixa-avulsa.mp3", False), ("outra.wav", False)]
    for row, (name, is_folder) in enumerate(entries[:4]):
        y = 56 + row * 32
        sel = (row == 0)
        if sel:
            s.roundrect(12, y - 2, 296, 28, 4, CYAN)
        label = ("> " if sel else "  ") + name + ("/" if is_folder else "")
        s.text(label[:40], 20, y + 6, NAVY if sel else (CYAN if is_folder else WHITE), S)
    s.text("1 / 4", 216, 16, WHITE, S)
    controller_labels(s, "ACIMA", "OK", "ABAIXO")
    back_button(s)
    s.save("docs/screens/music.png")

def music_playing():
    s = Screen()
    header(s, "MUSICA")
    # Capa do album (quadrado com borda ciano + nota musical desenhada)
    s.roundrect(16, 52, 88, 88, 2, CYAN)
    s.rect(18, 54, 84, 84, BLUE)
    s.d.polygon([(64, 74), (54, 92), (74, 92)], fill=CYAN)
    s.d.ellipse([56, 86, 70, 100], fill=CYAN)
    s.d.line([70, 74, 70, 96], fill=CYAN)
    s.d.line([70, 74, 78, 78], fill=CYAN)
    # Tags (truncadas como no firmware)
    s.text("Musica de Teste", 120, 52, YELLOW, S)
    s.text("ARTISTA COMICO", 120, 82, WHITE, S)
    s.text("ALBUM DE EXEMPLO", 120, 102, WHITE, S)
    s.text("2026", 120, 122, WHITE, S)
    # Nº da faixa + bitrate (canto sup. direito)
    s.text("2/4 160K", 200, 42, DARKCYAN, S)
    # Progresso (barra + relogio com total)
    s.rect(16, 170, 288, 6, CYAN)
    s.rect(17, 171, 80, 4, YELLOW)
    s.text("PLAY 00:03 / 00:08  SHUFFLE", 16, 184, CYAN, S)
    back_button(s)
    s.save("docs/screens/music_playing.png")

def weather():
    # Tela da saida RCA. Todo o conteudo fica na area segura do tubo
    # (SAFE_L=24, SAFE_T=18, SAFE_B=222), porque a TV CRT corta ~7% de
    # cada borda por overscan; so o fundo sangra ate o limite do raster.
    SAFE_L, SAFE_T, SAFE_W, SAFE_B = 24, 18, 272, 222
    TICKER_H = 16
    TICKER_Y = SAFE_B - TICKER_H  # 206
    s = Screen()
    s.fill_screen(NAVY)
    s.text("FRANCA - SP", 160, SAFE_T, YELLOW, M, "mc")
    s.hline(SAFE_L, SAFE_T + 30, SAFE_W, CYAN)
    s.text("PARCIAL NUBLADO", 160, SAFE_T + 40, WHITE, M, "mc")
    s.text("26 C", 160, SAFE_T + 76, YELLOW, L, "mc")
    s.text("UMIDADE  62%", 160, SAFE_T + 138, WHITE, S, "mc")
    s.text("VENTO  12 KM/H  SO", 160, SAFE_T + 158, WHITE, S, "mc")
    s.hline(SAFE_L, TICKER_Y - 4, SAFE_W, CYAN)
    s.rect(0, TICKER_Y, W, TICKER_H, BLACK)
    s.text("SEG 26/15C    TER 27/14C    QUA 25/12C", 160, TICKER_Y + 4, WHITE, S, "mc")
    s.save("docs/screens/weather.png")

def main():
    os.makedirs("docs/screens", exist_ok=True)
    for fn in (home, library, playback, radar, settings, info, info_network, portal, error, weather, music,
               music_playing):
        fn()
        print("ok:", fn.__name__)

if __name__ == "__main__":
    main()
