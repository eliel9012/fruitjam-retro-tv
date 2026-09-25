#pragma once
// ============================================================================
// Icones de condicao do tempo da tela "Local Forecast", no estilo do Weather
// Star 4000 (The Weather Channel dos anos 80): formas chapadas e grandes,
// paleta curta e saturada, e um contorno escuro em volta de cada forma.
//
// O contorno nao e enfeite: num tubo o croma sangra sobre o luma, e duas formas
// claras encostadas (sol atras de nuvem) viram uma mancha so. A borda escura e
// o que separava as duas, e e por isso que todo icone aqui e desenhado duas
// vezes — uma inflada, na cor do contorno, e outra por cima, na cor do miolo.
//
// Tudo sai de primitivas do M5GFX, sem bitmap embutido: o header fica pequeno,
// os icones escalam para qualquer tamanho e a mesma rotina serve ao painel CVBS
// do aparelho e ao painel SDL do simulador — dai o destino vir como LovyanGFX*,
// sem nada de Arduino, WiFi ou FreeRTOS no caminho.
//
// As cores sao so cantos do cubo RGB332 (8 niveis de vermelho e verde, 4 de
// azul). Tom sutil nao sobrevive a quantizacao da saida composta: o que e
// "azul-claro de chuva" no 565 pode cair no mesmo nivel do cinza da nuvem.
//
// Uso:
//   wx::drawWeatherIcon(&rca, cx, cy, 96, wx::iconFromWmo(code));
// ============================================================================

#include "fj/Gfx.h"

namespace wx {

// ---------------------------------------------------------------------------
//  Catalogo
// ---------------------------------------------------------------------------
// Um icone por familia de condicao, nao um por codigo WMO: a 48 px nao ha pixel
// suficiente para distinguir "garoa fraca" de "garoa forte", entao a intensidade
// so muda onde ela cabe (numero e comprimento dos traços de chuva).
enum WeatherIcon : uint8_t {
  WX_UNKNOWN = 0,
  WX_CLEAR,         // ceu limpo (dia)
  WX_FEW_CLOUDS,    // poucas nuvens
  WX_PARTLY_CLOUDY, // parcialmente nublado
  WX_CLOUDY,        // nublado
  WX_FOG,           // nevoeiro
  WX_DRIZZLE,       // garoa
  WX_RAIN_LIGHT,    // chuva fraca
  WX_RAIN,          // chuva
  WX_RAIN_HEAVY,    // chuva forte
  WX_SHOWERS,       // pancadas de chuva
  WX_THUNDER,       // trovoada
  WX_THUNDER_HAIL,  // trovoada com granizo
  WX_SNOW,          // neve
  WX_SNOW_GRAINS,   // graos de neve
  WX_ICON_COUNT
};

// Mesma lista de codigos de wmoConditionPt() em src/main.cpp, para que texto e
// icone nunca discordem na tela. Onde o texto distingue mais do que o desenho
// (congelante, por exemplo), o icone cai na familia mais proxima.
inline WeatherIcon iconFromWmo(int code) {
  switch (code) {
  case 0: return WX_CLEAR;
  case 1: return WX_FEW_CLOUDS;
  case 2: return WX_PARTLY_CLOUDY;
  case 3: return WX_CLOUDY;
  case 45:
  case 48: return WX_FOG;
  case 51:
  case 53:
  case 55: return WX_DRIZZLE;
  case 56:
  case 57: return WX_DRIZZLE; // garoa congelante: sem icone proprio
  case 61: return WX_RAIN_LIGHT;
  case 63: return WX_RAIN;
  case 65: return WX_RAIN_HEAVY;
  case 66:
  case 67: return WX_RAIN; // chuva congelante: sem icone proprio
  case 71:
  case 73:
  case 75: return WX_SNOW;
  case 77: return WX_SNOW_GRAINS;
  case 80:
  case 81:
  case 82: return WX_SHOWERS;
  case 85:
  case 86: return WX_SNOW; // pancadas de neve
  case 95: return WX_THUNDER;
  case 96:
  case 99: return WX_THUNDER_HAIL;
  default: return WX_UNKNOWN;
  }
}

// Rotulo curto, ASCII-only — serve a legenda de probe e a depuracao.
inline const char *iconName(WeatherIcon icon) {
  switch (icon) {
  case WX_CLEAR: return "CEU LIMPO";
  case WX_FEW_CLOUDS: return "POUCAS NUVENS";
  case WX_PARTLY_CLOUDY: return "PARCIAL NUBLADO";
  case WX_CLOUDY: return "NUBLADO";
  case WX_FOG: return "NEVOEIRO";
  case WX_DRIZZLE: return "GAROA";
  case WX_RAIN_LIGHT: return "CHUVA FRACA";
  case WX_RAIN: return "CHUVA";
  case WX_RAIN_HEAVY: return "CHUVA FORTE";
  case WX_SHOWERS: return "PANCADAS";
  case WX_THUNDER: return "TROVOADA";
  case WX_THUNDER_HAIL: return "TROVOADA GRANIZO";
  case WX_SNOW: return "NEVE";
  case WX_SNOW_GRAINS: return "GRAOS DE NEVE";
  default: return "INDISPONIVEL";
  }
}

// ---------------------------------------------------------------------------
//  Paleta
// ---------------------------------------------------------------------------
namespace detail {

// 565 montado a mao para nao depender de onde o M5GFX declara color565().
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t OUTLINE = rgb(0, 0, 0);          // contorno de tudo
constexpr uint16_t SUN = rgb(255, 216, 0);          // sol e raio      (332: 7,6,0)
constexpr uint16_t CLOUD = rgb(255, 255, 255);      // nuvem da frente (332: 7,7,3)
constexpr uint16_t CLOUD_BACK = rgb(160, 160, 160); // nuvem de tras   (332: 5,5,2)
constexpr uint16_t STORM = rgb(104, 104, 88);       // nuvem de trovoada (332: 3,3,1)
constexpr uint16_t RAIN = rgb(0, 168, 255);         // chuva           (332: 0,5,3)
constexpr uint16_t FOGBAR = rgb(180, 180, 176);     // faixa de nevoeiro (332: 5,5,2)
constexpr uint16_t MUTED = rgb(96, 96, 112);        // moldura do "sem dado"

// Percentual do lado do icone. Toda coordenada abaixo e escrita em centesimos
// do quadro, para o mesmo desenho fechar em 48 e em 96 px.
inline int pc(int s, int n) { return s * n / 100; }

// -------------------------------------------------------------------------
//  Formas base — cada uma aceita um "grow" que infla a silhueta, e e com ele
//  que o contorno sai com espessura uniforme sem precisar de stroke.
// -------------------------------------------------------------------------

// Nuvem: tres lobos e uma base reta. Os centros ficam a um raio da base, entao
// todos os lobos encostam na mesma linha e o fundo da nuvem sai chapado.
inline void cloudShape(LovyanGFX *d, int x, int yb, int w, uint16_t c, int g) {
  const int r1 = pc(w, 22), r2 = pc(w, 30), r3 = pc(w, 24);
  const int c1 = x + pc(w, 24), c2 = x + pc(w, 50), c3 = x + pc(w, 76);
  d->fillRect(c1, yb - r2 - g, c3 - c1, r2 + 2 * g, c);
  d->fillCircle(c1, yb - r1, r1 + g, c);
  d->fillCircle(c2, yb - r2, r2 + g, c);
  d->fillCircle(c3, yb - r3, r3 + g, c);
}

inline void cloud(LovyanGFX *d, int x, int yb, int w, uint16_t fill, int t) {
  cloudShape(d, x, yb, w, OUTLINE, t);
  cloudShape(d, x, yb, w, fill, 0);
}

// Sol: disco mais oito raios triangulares (o 4000 usava espicula, nao linha).
// Tabela de direcoes em 1/127 para nao arrastar <cmath> nem float pro header.
inline void sunShape(LovyanGFX *d, int cx, int cy, int r, int ray, uint16_t c, int g) {
  static const int8_t DIR[8][2] = {{127, 0},  {90, 90},  {0, 127},  {-90, 90},
                                   {-127, 0}, {-90, -90}, {0, -127}, {90, -90}};
  const int hw = (r * 34 / 100) + g;
  for (int i = 0; i < 8; ++i) {
    const int dx = DIR[i][0], dy = DIR[i][1];
    const int tip = r + ray + g, base = r * 7 / 10;
    const int ax = cx + dx * tip / 127, ay = cy + dy * tip / 127;
    const int bx = cx + dx * base / 127, by = cy + dy * base / 127;
    const int px = -dy * hw / 127, py = dx * hw / 127;
    d->fillTriangle(ax, ay, bx + px, by + py, bx - px, by - py, c);
  }
  d->fillCircle(cx, cy, r + g, c);
}

inline void sun(LovyanGFX *d, int cx, int cy, int r, int ray, int t) {
  sunShape(d, cx, cy, r, ray, OUTLINE, t);
  sunShape(d, cx, cy, r, ray, SUN, 0);
}

// Raio: ziguezague de seis vertices partido em quatro triangulos (o poligono e
// concavo, entao leque a partir de um vertice so nao serve). O contorno sai de
// afastar cada vertice do centro — aproximacao grosseira, mas a 48 px o olho
// so ve "tem borda escura".
inline void boltShape(LovyanGFX *d, int x, int y, int w, int h, uint16_t c, int g) {
  static const int8_t PX[6] = {62, 18, 45, 30, 85, 55};
  static const int8_t PY[6] = {0, 58, 58, 100, 42, 42};
  const int mx = x + w / 2, my = y + h / 2;
  int vx[6], vy[6];
  for (int i = 0; i < 6; ++i) {
    const int ax = x + w * PX[i] / 100, ay = y + h * PY[i] / 100;
    vx[i] = ax + (ax >= mx ? g : -g);
    vy[i] = ay + (ay >= my ? g : -g);
  }
  d->fillTriangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], c);
  d->fillTriangle(vx[0], vy[0], vx[2], vy[2], vx[5], vy[5], c);
  d->fillTriangle(vx[2], vy[2], vx[3], vy[3], vx[4], vy[4], c);
  d->fillTriangle(vx[2], vy[2], vx[4], vy[4], vx[5], vy[5], c);
}

inline void bolt(LovyanGFX *d, int x, int y, int w, int h, int t) {
  boltShape(d, x, y, w, h, OUTLINE, t);
  boltShape(d, x, y, w, h, SUN, 0);
}

// Gota: traco diagonal. Um halo escuro de 1 px de cada lado basta — contorno
// cheio numa forma de 3 px de largura apagaria o miolo.
inline void drop(LovyanGFX *d, int x, int y, int len, int th) {
  const int dx = len / 2;
  for (int i = -1; i <= th; ++i)
    d->drawLine(x + i, y - 1, x + i - dx, y + len - 1, OUTLINE);
  for (int i = 0; i < th; ++i)
    d->drawLine(x + i, y, x + i - dx, y + len, RAIN);
}

// Chuva: n gotas espalhadas sob a nuvem, em duas alturas alternadas — chuva
// enfileirada na mesma linha lê como pente, nao como precipitacao.
inline void rain(LovyanGFX *d, int x0, int y0, int s, int n, int lenPc, int th, int topPc) {
  const int len = pc(s, lenPc);
  const int span = pc(s, 60);
  for (int i = 0; i < n; ++i) {
    const int x = x0 + pc(s, 18) + (n > 1 ? span * i / (n - 1) : span / 2);
    const int y = y0 + pc(s, topPc) + ((i & 1) ? pc(s, 9) : 0);
    drop(d, x, y, len, th);
  }
}

// Floco: estrela de seis pontas em tres tracos, `hw` passes de cada lado da
// linha central. A espessura nao vem da espessura do contorno do resto do icone
// — a 48 px um braco de 1 px branco dentro de um contorno de 3 px vira uma
// mancha preta, entao o branco e que manda e o contorno e sempre ele mais um.
inline void flakeShape(LovyanGFX *d, int cx, int cy, int r, uint16_t c, int hw) {
  for (int k = -hw; k <= hw; ++k) {
    d->drawFastHLine(cx - r, cy + k, 2 * r + 1, c);
    d->drawLine(cx - r / 2 + k, cy - r, cx + r / 2 + k, cy + r, c);
    d->drawLine(cx + r / 2 + k, cy - r, cx - r / 2 + k, cy + r, c);
  }
}

inline void flake(LovyanGFX *d, int cx, int cy, int r) {
  const int hw = r / 4 > 1 ? r / 4 : 1;
  flakeShape(d, cx, cy, r, OUTLINE, hw + 1);
  flakeShape(d, cx, cy, r, CLOUD, hw);
}

// Disco com borda (pingo de garoa, grao de neve, pedra de granizo). O contorno
// do icone (3 px a 96) engoliria um disco de 4 px de raio, entao aqui ele e
// proporcional ao disco: a forma tem que continuar cheia, nao virar anel.
inline void disc(LovyanGFX *d, int cx, int cy, int r, int t, uint16_t c) {
  const int b = r / 3 > 1 ? (r / 3 < t ? r / 3 : t) : 1;
  d->fillCircle(cx, cy, r + b, OUTLINE);
  d->fillCircle(cx, cy, r, c);
}

} // namespace detail

// ---------------------------------------------------------------------------
//  Desenho
// ---------------------------------------------------------------------------
// (cx, cy) e o centro do icone; `size` e o lado do quadro que ele ocupa. As
// proporcoes foram fechadas olhando 48 e 96 px no simulador, que e onde os
// arredondamentos inteiros aparecem; qualquer tamanho a partir de ~32 px
// funciona, abaixo disso o contorno come o miolo.
inline void drawWeatherIcon(LovyanGFX *dst, int cx, int cy, int size, WeatherIcon icon) {
  using namespace detail;
  if (!dst || size < 8)
    return;
  const int s = size;
  const int x0 = cx - s / 2, y0 = cy - s / 2;
  const int t = s < 40 ? 1 : s / 26; // espessura do contorno: 1 / 1 / 3 em 32/48/96

  switch (icon) {
  case WX_CLEAR:
    sun(dst, cx, cy, pc(s, 25), pc(s, 15), t);
    break;

  case WX_FEW_CLOUDS:
    // Sol dominante, nuvem pequena mordendo o canto: a diferenca para o
    // "parcial nublado" e so a area relativa, entao ela precisa ser grande.
    sun(dst, x0 + pc(s, 38), y0 + pc(s, 32), pc(s, 23), pc(s, 13), t);
    cloud(dst, x0 + pc(s, 50), y0 + pc(s, 92), pc(s, 48), CLOUD, t);
    break;

  case WX_PARTLY_CLOUDY:
    sun(dst, x0 + pc(s, 28), y0 + pc(s, 24), pc(s, 18), pc(s, 10), t);
    cloud(dst, x0 + pc(s, 6), y0 + pc(s, 92), pc(s, 92), CLOUD, t);
    break;

  case WX_CLOUDY:
    // Duas nuvens, a de tras em cinza: sem o sol, e o empilhamento que diz
    // "encoberto" em vez de "uma nuvem passando".
    cloud(dst, x0 + pc(s, 30), y0 + pc(s, 54), pc(s, 68), CLOUD_BACK, t);
    cloud(dst, x0 + pc(s, 0), y0 + pc(s, 96), pc(s, 78), CLOUD, t);
    break;

  case WX_FOG: {
    // Faixas horizontais escalonadas. Nevoeiro como "nuvem baixa" ficaria igual
    // a nublado; a listra e o que o 4000 usava e continua sendo inconfundivel.
    static const int8_t BAR[4][2] = {{8, 78}, {20, 76}, {4, 74}, {22, 70}};
    const int h = pc(s, 13), gap = pc(s, 20);
    for (int i = 0; i < 4; ++i) {
      const int bx = x0 + pc(s, BAR[i][0]), bw = pc(s, BAR[i][1]);
      const int by = y0 + pc(s, 12) + i * gap;
      dst->fillRoundRect(bx - t, by - t, bw + 2 * t, h + 2 * t, (h + 2 * t) / 2, OUTLINE);
      dst->fillRoundRect(bx, by, bw, h, h / 2, FOGBAR);
    }
    break;
  }

  case WX_DRIZZLE:
    // Garoa e pingo redondo e esparso, nao traco: e o unico jeito de ela nao
    // virar "chuva fraca" a 48 px.
    cloud(dst, x0 + pc(s, 8), y0 + pc(s, 52), pc(s, 84), CLOUD, t);
    for (int i = 0; i < 5; ++i) {
      const int dx = x0 + pc(s, 20) + pc(s, 15) * i;
      const int dy = y0 + pc(s, 68) + ((i & 1) ? pc(s, 16) : 0);
      disc(dst, dx, dy, pc(s, 5), t, RAIN);
    }
    break;

  case WX_RAIN_LIGHT:
    cloud(dst, x0 + pc(s, 8), y0 + pc(s, 52), pc(s, 84), CLOUD, t);
    rain(dst, x0, y0, s, 3, 20, t + 1, 60);
    break;

  case WX_RAIN:
    cloud(dst, x0 + pc(s, 8), y0 + pc(s, 50), pc(s, 84), CLOUD, t);
    rain(dst, x0, y0, s, 4, 26, t + 1, 56);
    break;

  case WX_RAIN_HEAVY:
    // Chuva forte = nuvem mais alta, traco mais longo e mais grosso, ocupando
    // toda a faixa livre. A intensidade e area de azul, nao numero de gotas.
    cloud(dst, x0 + pc(s, 6), y0 + pc(s, 44), pc(s, 88), CLOUD, t);
    rain(dst, x0, y0, s, 6, 36, t + 2, 48);
    break;

  case WX_SHOWERS:
    // Pancada e intermitente: o sol espiando por tras da nuvem e o que separa
    // isso de "chuva".
    sun(dst, x0 + pc(s, 74), y0 + pc(s, 20), pc(s, 14), pc(s, 8), t);
    cloud(dst, x0 + pc(s, 2), y0 + pc(s, 56), pc(s, 76), CLOUD, t);
    rain(dst, x0, y0, s, 3, 24, t + 1, 62);
    break;

  case WX_THUNDER:
    cloud(dst, x0 + pc(s, 6), y0 + pc(s, 50), pc(s, 88), STORM, t);
    bolt(dst, x0 + pc(s, 30), y0 + pc(s, 46), pc(s, 40), pc(s, 50), t);
    break;

  case WX_THUNDER_HAIL:
    // Raio empurrado para a esquerda para abrir espaco as pelotas de granizo;
    // sem elas, este icone e o de trovoada sao o mesmo desenho.
    cloud(dst, x0 + pc(s, 6), y0 + pc(s, 48), pc(s, 88), STORM, t);
    bolt(dst, x0 + pc(s, 14), y0 + pc(s, 44), pc(s, 34), pc(s, 48), t);
    for (int i = 0; i < 3; ++i) {
      const int hx = x0 + pc(s, 62) + pc(s, 13) * (i % 2);
      const int hy = y0 + pc(s, 56) + pc(s, 15) * i;
      disc(dst, hx, hy, pc(s, 7), t, CLOUD);
    }
    break;

  case WX_SNOW:
    cloud(dst, x0 + pc(s, 8), y0 + pc(s, 52), pc(s, 84), CLOUD_BACK, t);
    for (int i = 0; i < 3; ++i) {
      const int fx = x0 + pc(s, 22) + pc(s, 28) * i;
      const int fy = y0 + pc(s, 70) + ((i == 1) ? pc(s, 18) : 0);
      flake(dst, fx, fy, pc(s, 10));
    }
    break;

  case WX_SNOW_GRAINS:
    // Grao e pelota, nao estrela: floco pequeno demais vira borrao no tubo.
    cloud(dst, x0 + pc(s, 8), y0 + pc(s, 52), pc(s, 84), CLOUD_BACK, t);
    for (int i = 0; i < 6; ++i) {
      const int gx = x0 + pc(s, 20) + pc(s, 12) * i;
      const int gy = y0 + pc(s, 70) + ((i & 1) ? pc(s, 16) : 0);
      disc(dst, gx, gy, pc(s, 5), t, CLOUD);
    }
    break;

  case WX_UNKNOWN:
  default:
    // Sem dado: moldura riscada. Qualquer forma de tempo aqui mentiria.
    dst->drawRoundRect(x0 + pc(s, 8), y0 + pc(s, 8), pc(s, 84), pc(s, 84), pc(s, 14), MUTED);
    dst->drawLine(x0 + pc(s, 24), y0 + pc(s, 24), x0 + pc(s, 76), y0 + pc(s, 76), MUTED);
    dst->drawLine(x0 + pc(s, 76), y0 + pc(s, 24), x0 + pc(s, 24), y0 + pc(s, 76), MUTED);
    break;
  }
}

} // namespace wx
