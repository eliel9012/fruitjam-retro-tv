#pragma once
#include <stdint.h>
#include <stddef.h>
#include <M5GFX.h>

// ============================================================================
// VcrFont — fonte bitmap do OSD, no traço dos videocassetes Sony/Semp dos 90.
//
// Por que não usar as fontes do M5GFX: `fonts::Font0/2/4` são fontes de
// terminal (linhagem Adafruit), de traço fino e variável, desenhadas para LCD
// nítido. Na saída composta elas somem: o NTSC borra horizontalmente, e um
// traço de 1 px vira um cinza indeciso sobre o vídeo. O OSD de videocassete
// resolvia isso com o oposto — traço grosso e constante, geometria quadrada,
// contadores pequenos — que é o que esta tabela reproduz.
//
// Regras do desenho (valem para todos os glifos, e devem valer para qualquer
// glifo novo):
//   * Célula de 12x16. O desenho vive nas colunas 1..10 e nas linhas 2..13;
//     a coluna 0/11 e as linhas 0..1/14..15 são a folga fixa do monoespaçado,
//     e é ela que dá lugar ao contorno sem invadir o vizinho.
//   * Altura de caixa alta = 12 px (linhas 2..13). Só maiúsculas.
//   * Traço de 2 px em qualquer direção, sem afinar. Diagonais usam 3 px por
//     linha, porque 3 px na horizontal ≈ 2 px na perpendicular a 45°.
//   * Contadores (A O P R D Q 8) retangulares e pequenos; terminais retos.
//   * 1 bit por pixel, sem antialiasing — o borrão do NTSC já é o antialiasing.
//
// Orçamento de flash
//   Tabela: 45 glifos x 16 linhas x 2 bytes = 1440 bytes, e nada além disso —
//   conferido com `size -m` num .o que só usa este header: __const = 1440. A
//   arte ASCII dos glifos é constexpr e não sobra no binário, e não há tabela
//   de índice: o mapeamento char -> índice é aritmético (ver glyphIndex).
//
// Custo por caractere desenhado (ESP32 a 240 MHz, sobre vídeo em reprodução).
// Os números vêm do sim/probes/vcr_font.cpp, que os recalcula e imprime:
//   Sem contorno: 8,0 fillRect e 56 px por caractere em escala 1 (pior caso o
//   'K', com 21 fillRect). O rasterizador não emite um retângulo por pixel:
//   agrupa linhas idênticas consecutivas e, dentro do grupo, corridas
//   horizontais de bits — um 'E' sai em 5 retângulos.
//
//   Com contorno: +23 fillRect e +65 px, ou seja 2,15x os pixels de um glifo
//   simples. O contorno preto — a sombra que dava legibilidade sobre a imagem —
//   sai barato porque é dilatação, não repetição: a máscara de 16x12 bits é
//   dilatada 8-conexa com shifts (3 OR verticais + 2 OR horizontais por linha,
//   ~80 instruções por glifo), a sombra é `dilatada & ~original` e vai à tela
//   num passe só. A alternativa ingênua — redesenhar a string deslocada nas 8
//   direções — pintaria 9,00x os pixels e ~72 fillRect por caractere. Ou seja,
//   4,2x menos pixels e 2,3x menos chamadas, que é o que permite reimprimir o
//   OSD a cada quadro sem roubar tempo do decodificador.
//
// Escala: `scale` é um inteiro. 1 = 12x16 (tamanho de tela), 2 = 24x32 (o modo
// "grande" dos títulos). Não há escala fracionária de propósito — meio pixel
// num traço de 2 px destrói o peso uniforme que é a graça da fonte. O contorno
// acompanha a escala (1 px de bitmap = `scale` px na tela), que é o certo: um
// contorno de 1 px real sumiria embaixo de um traço de 8.
//
// Dependências: só M5GFX. Nada de Arduino/WiFi/SD/FreeRTOS, para que o mesmo
// header sirva ao painel CVBS do aparelho e ao painel SDL do simulador.
// ============================================================================

namespace vcrfont {

constexpr int CELL_W = 12;
constexpr int CELL_H = 16;
constexpr uint16_t CELL_MASK = 0x0FFF; // 12 bits válidos por linha

// O orçamento de flash em constantes, para não virar comentário desatualizado.
constexpr int GLYPH_COUNT = 45;
constexpr int FLASH_BYTES = GLYPH_COUNT * CELL_H * 2; // 1440

// Convenção de cor: `int32_t`, que é como o LovyanGFX interpreta as constantes
// TFT_* e o retorno de color565() — RGB565. Foi escolhido por ser o que o resto
// do repositório já passa para o M5GFX; um uint32_t seria lido como RGB888 e
// TFT_WHITE sairia ciano.
//
// Passe isto em `outline` para desenhar sem contorno.
constexpr int32_t NO_OUTLINE = -1;

namespace detail {

// Converte uma linha em arte ASCII para bitmap. O parâmetro é uma referência a
// array de tamanho exato: se alguém digitar 11 ou 13 colunas, o erro aparece na
// compilação, não num glifo torto meses depois.
//
// Escrita em recursão de uma expressão só porque o firmware compila em C++11
// (arduino-esp32 2.x, -std=gnu++11), onde constexpr não admite laço nem
// variável local. O simulador compila em C++17 e aceitaria o laço — foi assim
// que a primeira versão passou no probe e quebrou no firmware.
constexpr uint16_t rowBitsFrom(const char *s, int i) {
  return i >= CELL_W ? 0
                     : (uint16_t)(((s[i] != '.') ? (1u << (CELL_W - 1 - i)) : 0u) |
                                  rowBitsFrom(s, i + 1));
}
constexpr uint16_t rowBits(const char (&s)[CELL_W + 1]) { return rowBitsFrom(s, 0); }

// Ordem da tabela: espaço, 0-9, A-Z, e a pontuação. Ver glyphIndex().
constexpr int SPACE = 0, DIGIT0 = 1, LETTERA = 11, PUNCT = 37;

// Tudo aqui é constexpr: as strings existem só durante a compilação, o que
// chega ao binário são os 1440 bytes de uint16_t.
// A tabela é arte: uma linha do glifo por linha do arquivo, para que a letra
// apareça no próprio código e possa ser corrigida pixel a pixel. Daí o
// clang-format off — reflowado, o desenho vira uma parede de literais.
// clang-format off
constexpr uint16_t GLYPHS[GLYPH_COUNT][CELL_H] = {
    // [0] ' '
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    // [1] '0' — a barra interna é o que separa do 'O'; sem ela, em RGB332 sobre
    // vídeo, "O" e "0" viram a mesma mancha
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##.....###."),
        rowBits(".##....####."),
        rowBits(".##....####."),
        rowBits(".##...##.##."),
        rowBits(".##..##..##."),
        rowBits(".##.##...##."),
        rowBits(".####....##."),
        rowBits(".####....##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [2] '1'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...####....."),
        rowBits("..#####....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("..########.."),
        rowBits("..########.."),
        rowBits("............"),
        rowBits("............"),
    },
    // [3] '2'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits("......####.."),
        rowBits(".....####..."),
        rowBits("....####...."),
        rowBits("...####....."),
        rowBits(".####......."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [4] '3'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits("...########."),
        rowBits("...########."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [5] '4'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".......##..."),
        rowBits("......###..."),
        rowBits(".....####..."),
        rowBits("....#####..."),
        rowBits("...###.##..."),
        rowBits("..###..##..."),
        rowBits(".###...##..."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".......##..."),
        rowBits(".......##..."),
        rowBits(".......##..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [6] '5' — topo reto e sem esporão à direita; é o que separa do 'S'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".#########.."),
        rowBits(".##########."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [7] '6'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [8] '7'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("........###."),
        rowBits(".......###.."),
        rowBits("......###..."),
        rowBits(".....###...."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
    // [9] '8'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [10] '9' — espelho vertical do '6'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [11] 'A'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits("............"),
        rowBits("............"),
    },
    // [12] 'B' — bojo de cima 2 px mais estreito que o de baixo; sem isso vira '8'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".########..."),
        rowBits(".########..."),
        rowBits(".##....##..."),
        rowBits(".##....##..."),
        rowBits(".##....##..."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [13] 'C'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [14] 'D' — retângulo perfeito. É o contraponto do 'O', que é octogonal:
    // com os dois chanfrados os dois ficavam a um pixel de distância
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [15] 'E'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".########..."),
        rowBits(".########..."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [16] 'F'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".########..."),
        rowBits(".########..."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits("............"),
        rowBits("............"),
    },
    // [17] 'G'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##..######."),
        rowBits(".##..######."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [18] 'H'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits("............"),
        rowBits("............"),
    },
    // [19] 'I' — com barras em cima e embaixo: uma haste solta no meio de uma
    // célula de 12 px abriria um buraco na palavra
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [20] 'J'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".......##..."),
        rowBits(".......##..."),
        rowBits(".......##..."),
        rowBits(".......##..."),
        rowBits(".......##..."),
        rowBits(".##....##..."),
        rowBits(".##....##..."),
        rowBits(".##....##..."),
        rowBits(".########..."),
        rowBits(".########..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [21] 'K'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##.....###."),
        rowBits(".##....###.."),
        rowBits(".##...###..."),
        rowBits(".##..###...."),
        rowBits(".##.###....."),
        rowBits(".#####......"),
        rowBits(".#####......"),
        rowBits(".##.###....."),
        rowBits(".##..###...."),
        rowBits(".##...###..."),
        rowBits(".##....###.."),
        rowBits(".##.....###."),
        rowBits("............"),
        rowBits("............"),
    },
    // [22] 'L'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [23] 'M' — o V central para na meia-altura; descer até a base fecharia o
    // vão num traço de 2 px
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##......##."),
        rowBits(".###....###."),
        rowBits(".####..####."),
        rowBits(".##.####.##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits("............"),
        rowBits("............"),
    },
    // [24] 'N'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".####....##."),
        rowBits(".####....##."),
        rowBits(".#####...##."),
        rowBits(".#####...##."),
        rowBits(".##.###..##."),
        rowBits(".##.###..##."),
        rowBits(".##..###.##."),
        rowBits(".##..###.##."),
        rowBits(".##...#####."),
        rowBits(".##...#####."),
        rowBits(".##....####."),
        rowBits(".##.....###."),
        rowBits("............"),
        rowBits("............"),
    },
    // [25] 'O'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [26] 'P'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits("............"),
        rowBits("............"),
    },
    // [27] 'Q' — rabo por dentro do contador: para fora invadiria a folga e
    // colaria no caractere seguinte
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##..##..##."),
        rowBits(".##...##.##."),
        rowBits(".##....####."),
        rowBits(".##.....###."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [28] 'R'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".##.###....."),
        rowBits(".##..###...."),
        rowBits(".##...###..."),
        rowBits(".##....###.."),
        rowBits(".##.....###."),
        rowBits("............"),
        rowBits("............"),
    },
    // [29] 'S'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".##........."),
        rowBits(".##........."),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [30] 'T'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
    // [31] 'U'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##......##."),
        rowBits(".##########."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
    },
    // [32] 'V'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".###....###."),
        rowBits(".###....###."),
        rowBits(".###....###."),
        rowBits("..###..###.."),
        rowBits("..###..###.."),
        rowBits("..###..###.."),
        rowBits("...######..."),
        rowBits("...######..."),
        rowBits("...######..."),
        rowBits("....####...."),
        rowBits("....####...."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
    // [33] 'W' — três hastes até a meia-altura e só então o V duplo. Com o vértice
    // central curto (o espelho exato do 'M') a letra lia como 'U' com um entalhe
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##..##..##."),
        rowBits(".##.####.##."),
        rowBits(".####..####."),
        rowBits(".###....###."),
        rowBits(".##......##."),
        rowBits("............"),
        rowBits("............"),
    },
    // [34] 'X'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".###....###."),
        rowBits(".###....###."),
        rowBits("..###..###.."),
        rowBits("..###..###.."),
        rowBits("...######..."),
        rowBits("....####...."),
        rowBits("....####...."),
        rowBits("...######..."),
        rowBits("..###..###.."),
        rowBits("..###..###.."),
        rowBits(".###....###."),
        rowBits(".###....###."),
        rowBits("............"),
        rowBits("............"),
    },
    // [35] 'Y'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".###....###."),
        rowBits(".###....###."),
        rowBits("..###..###.."),
        rowBits("..###..###.."),
        rowBits("...######..."),
        rowBits("....####...."),
        rowBits("....####...."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
    // [36] 'Z'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("........###."),
        rowBits(".......###.."),
        rowBits("......###..."),
        rowBits(".....###...."),
        rowBits("....###....."),
        rowBits("...###......"),
        rowBits("..###......."),
        rowBits(".###........"),
        rowBits(".##########."),
        rowBits(".##########."),
        rowBits("............"),
        rowBits("............"),
    },
    // [37] ':' — os dois pontos do contador de tempo; ficam na faixa entre as
    // barras de E/F para não brigar com os dígitos vizinhos
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
    },
    // [38] '.'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
    // [39] '-'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits("...######..."),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
    },
    // [40] '/'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("........###."),
        rowBits(".......###.."),
        rowBits(".......###.."),
        rowBits("......###..."),
        rowBits(".....###...."),
        rowBits(".....###...."),
        rowBits("....###....."),
        rowBits("....###....."),
        rowBits("...###......"),
        rowBits("..###......."),
        rowBits("..###......."),
        rowBits(".###........"),
        rowBits("............"),
        rowBits("............"),
    },
    // [41] '%' — os "anéis" viram blocos 3x3: um anel de verdade precisaria de
    // 5x5 com traço de 2 px, e não cabe com a barra no meio
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".###....###."),
        rowBits(".###...###.."),
        rowBits(".###...###.."),
        rowBits("......###..."),
        rowBits(".....###...."),
        rowBits(".....###...."),
        rowBits("....###....."),
        rowBits("....###....."),
        rowBits("...###......"),
        rowBits("..###...###."),
        rowBits("..###...###."),
        rowBits(".###....###."),
        rowBits("............"),
        rowBits("............"),
    },
    // [42] '+'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("..########.."),
        rowBits("..########.."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
        rowBits("............"),
    },
    // [43] '?'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits("...######..."),
        rowBits(".##########."),
        rowBits(".##......##."),
        rowBits(".........##."),
        rowBits(".........##."),
        rowBits("......####.."),
        rowBits(".....####..."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
    // [44] '!'
    {
        rowBits("............"),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
        rowBits(".....##....."),
        rowBits(".....##....."),
        rowBits("............"),
        rowBits("............"),
    },
};
// clang-format on

// Índice do glifo. Sem tabela de busca: a ordem da tabela foi escolhida para
// que o mapeamento saia em aritmética pura. Minúsculas sobem para maiúsculas
// (a fonte é caixa alta) e o que não existe vira espaço — melhor um buraco do
// que um glifo aleatório sobre o vídeo.
constexpr int glyphIndex(char c) {
  return (c >= 'a' && c <= 'z')   ? LETTERA + (c - 'a')
         : (c >= 'A' && c <= 'Z') ? LETTERA + (c - 'A')
         : (c >= '0' && c <= '9') ? DIGIT0 + (c - '0')
         : (c == ':')             ? PUNCT + 0
         : (c == '.')             ? PUNCT + 1
         : (c == '-')             ? PUNCT + 2
         : (c == '/')             ? PUNCT + 3
         : (c == '%')             ? PUNCT + 4
         : (c == '+')             ? PUNCT + 5
         : (c == '?')             ? PUNCT + 6
         : (c == '!')             ? PUNCT + 7
                                  : SPACE;
}

// Dilatação 8-conexa da máscara do glifo, em `dst`. É a operação que troca o
// contorno de 8 redesenhos por um passe só: 5 ORs por linha contra 8 varreduras
// completas de rasterização.
inline void dilate(const uint16_t *src, uint16_t *dst) {
  for (int r = 0; r < CELL_H; ++r) {
    uint16_t v = src[r];
    if (r > 0)
      v = (uint16_t)(v | src[r - 1]);
    if (r + 1 < CELL_H)
      v = (uint16_t)(v | src[r + 1]);
    dst[r] = (uint16_t)((v | (v << 1) | (v >> 1)) & CELL_MASK);
  }
}

// Pinta uma máscara de 16x12 bits. Agrupa linhas idênticas consecutivas e, em
// cada grupo, corridas horizontais de bits — um 'E' sai em 5 retângulos em vez
// de 60 pixels soltos, que é o que torna viável reimprimir o OSD por quadro.
inline void blit(LovyanGFX *gfx, const uint16_t *rows, int x, int y, int scale, int32_t color) {
  gfx->setColor(color); // uma conversão de cor por glifo, não uma por retângulo
  for (int r = 0; r < CELL_H;) {
    const uint16_t m = rows[r];
    if (!m) {
      ++r;
      continue;
    }
    int h = 1;
    while (r + h < CELL_H && rows[r + h] == m)
      ++h;
    for (int c = 0; c < CELL_W;) {
      if (!(m & (1u << (CELL_W - 1 - c)))) {
        ++c;
        continue;
      }
      int w = 1;
      while (c + w < CELL_W && (m & (1u << (CELL_W - 1 - c - w))))
        ++w;
      gfx->fillRect(x + c * scale, y + r * scale, w * scale, h * scale);
      c += w;
    }
    r += h;
  }
}

} // namespace detail

// As 16 linhas de 12 bits de um caractere (bit 11 = coluna 0, à esquerda).
// Exposto para quem precisar compor o glifo de outro jeito — medir tinta, gerar
// uma máscara para um efeito, etc.
inline const uint16_t *glyph(char c) {
  return detail::GLYPHS[detail::glyphIndex(c)];
}

// Largura que uma string ocupa, em pixels. Monoespaçada, então é só contar —
// mas existe como função para que quem centraliza não precise saber disso.
inline int textWidth(const char *s, int scale = 1) {
  int n = 0;
  for (const char *p = s; p && *p; ++p)
    ++n;
  return n * CELL_W * scale;
}

constexpr int textHeight(int scale = 1) {
  return CELL_H * scale;
}

// Um caractere em (x, y) = canto superior esquerdo da célula.
inline void drawChar(LovyanGFX *gfx, char c, int x, int y, int32_t color, int scale = 1,
                     int32_t outline = NO_OUTLINE) {
  if (!gfx)
    return;
  const uint16_t *g = glyph(c);
  gfx->startWrite();
  if (outline != NO_OUTLINE) {
    uint16_t dil[CELL_H], ring[CELL_H];
    detail::dilate(g, dil);
    for (int r = 0; r < CELL_H; ++r)
      ring[r] = (uint16_t)(dil[r] & ~g[r]); // só o anel de fora: o miolo é tinta
    detail::blit(gfx, ring, x, y, scale, outline);
  }
  detail::blit(gfx, g, x, y, scale, color);
  gfx->endWrite();
}

// Desenha a string e devolve a largura consumida (útil para encadear).
//
// Dois passes quando há contorno: toda a sombra primeiro, depois toda a tinta.
// Os glifos vivem nas colunas 1..10 e a sombra chega às colunas 0 e 11, então
// hoje um passe só também funcionaria; o passe duplo é o que garante que isso
// continue valendo se algum glifo novo esbarrar na folga.
inline int drawText(LovyanGFX *gfx, const char *s, int x, int y, int32_t color, int scale = 1,
                    int32_t outline = NO_OUTLINE) {
  if (!gfx || !s)
    return 0;
  const int adv = CELL_W * scale;
  gfx->startWrite();
  if (outline != NO_OUTLINE) {
    int cx = x;
    for (const char *p = s; *p; ++p, cx += adv) {
      const uint16_t *g = glyph(*p);
      uint16_t dil[CELL_H], ring[CELL_H];
      detail::dilate(g, dil);
      for (int r = 0; r < CELL_H; ++r)
        ring[r] = (uint16_t)(dil[r] & ~g[r]);
      detail::blit(gfx, ring, cx, y, scale, outline);
    }
  }
  int cx = x;
  for (const char *p = s; *p; ++p, cx += adv)
    detail::blit(gfx, glyph(*p), cx, y, scale, color);
  gfx->endWrite();
  return cx - x;
}

// Centraliza a string na faixa [x, x + w). O arredondamento vai para a
// esquerda: meio pixel de sobra à direita some no overscan, à esquerda não.
inline int drawTextCentered(LovyanGFX *gfx, const char *s, int x, int y, int w, int32_t color, int scale = 1,
                            int32_t outline = NO_OUTLINE) {
  return drawText(gfx, s, x + (w - textWidth(s, scale)) / 2, y, color, scale, outline);
}

} // namespace vcrfont
