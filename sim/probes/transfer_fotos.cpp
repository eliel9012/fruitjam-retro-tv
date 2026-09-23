// ============================================================================
// Bancada da categoria "fotos" do envio por Wi-Fi (include/FileTransfer.h).
//
// Nao abre janela: e um probe de LOGICA, nao de desenho. `FILETRANSFER_NO_NETWORK`
// corta a metade do header que depende de Arduino, WiFi e SD, e o que sobra —
// validacao de caminho, filtro de extensao, teto de tamanho e conferencia do
// cabecalho JPEG — e exatamente a parte que erra em silencio no aparelho.
//
// Roda tambem fora do Makefile do simulador, que e como ele foi conferido:
//
//   g++ -std=gnu++11 -I include sim/probes/transfer_fotos.cpp -o /tmp/probe && /tmp/probe
//
// O -std=gnu++11 nao e detalhe: o firmware compila nesse padrao e o simulador
// em C++17, entao um probe compilado so em C++17 esconderia justamente o tipo
// de erro que o AGENTS.md lista como armadilha 1.
// ============================================================================

#define FILETRANSFER_NO_NETWORK
#include "FileTransfer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xfer;

static int falhas = 0;
static int checagens = 0;

static void ok(bool condicao, const char *texto) {
  ++checagens;
  if (condicao) {
    printf("  ok    %s\n", texto);
  } else {
    printf("  FALHA %s\n", texto);
    ++falhas;
  }
}

// Mesmo auxiliar dos testes nativos: enche o buffer de lixo antes, para pegar
// quem devolve OK sem terminar a string.
static PathError caminho(const char *cru, std::string *saida = NULL) {
  char buffer[PATH_CAP];
  memset(buffer, 0x7e, sizeof(buffer));
  const PathError r = sanitizePath(cru, buffer, sizeof(buffer));
  if (r == PathError::OK && saida)
    *saida = buffer;
  return r;
}

// --------------------------------------------------------------------------
// 1. A categoria nova aceita JPEG e cai em /M5RETRO/fotos
// --------------------------------------------------------------------------
static void testaRoteamento() {
  printf("\n[1] fotos/ roteia para /M5RETRO/fotos\n");
  std::string p;

  ok(caminho("fotos/praia.jpg", &p) == PathError::OK && p == "/M5RETRO/fotos/praia.jpg",
     "fotos/praia.jpg -> /M5RETRO/fotos/praia.jpg");
  ok(caminho("fotos/praia.jpeg", &p) == PathError::OK && p == "/M5RETRO/fotos/praia.jpeg",
     "fotos/praia.jpeg aceito");
  ok(caminho("fotos/verao/dia1.jpg", &p) == PathError::OK && p == "/M5RETRO/fotos/verao/dia1.jpg",
     "subpasta dentro de fotos/ continua valendo");
  ok(caminho("fotos/foto%20do%20mar.jpg", &p) == PathError::OK &&
         p == "/M5RETRO/fotos/foto do mar.jpg",
     "percent-encoding decodificado antes de validar a extensao");

  // As categorias antigas nao podem ter mudado de comportamento.
  ok(caminho("videos/meu-filme/video.mjpeg", &p) == PathError::OK &&
         p == "/M5RETRO/videos/meu-filme/video.mjpeg",
     "videos/ intacto");
  ok(caminho("music/faixa.wav", &p) == PathError::OK && p == "/M5RETRO/music/faixa.wav",
     "music/ intacto");
  // O filtro de extensao e SO de fotos/: um .exe em videos/ continua passando
  // pela validacao de caminho como passava antes desta mudanca.
  ok(caminho("videos/x.exe") == PathError::OK, "videos/ nao ganhou filtro de extensao");

  ok(isPhotoPath("/M5RETRO/fotos/praia.jpg"), "isPhotoPath reconhece a pasta");
  ok(!isPhotoPath("/M5RETRO/videos/x.mjpeg"), "isPhotoPath nao pega videos/");
  ok(!isPhotoPath("/M5RETRO/fotosecreto/x.jpg"),
     "isPhotoPath exige a barra: 'fotosecreto' nao e 'fotos'");
  ok(!isPhotoPath(NULL), "isPhotoPath aceita ponteiro nulo");
}

// --------------------------------------------------------------------------
// 2. Extensao: so .jpg/.jpeg, sem sensibilidade a caixa
// --------------------------------------------------------------------------
static void testaExtensao() {
  printf("\n[2] extensao em fotos/\n");

  ok(caminho("fotos/virus.exe") == PathError::EXTENSAO, ".exe recusado");
  ok(caminho("fotos/musica.wav") == PathError::EXTENSAO, ".wav recusado");
  ok(caminho("fotos/filme.mjpeg") == PathError::EXTENSAO, ".mjpeg recusado");
  ok(caminho("fotos/config.json") == PathError::EXTENSAO, ".json recusado");
  ok(caminho("fotos/imagem.png") == PathError::EXTENSAO, ".png recusado");
  ok(caminho("fotos/semextensao") == PathError::EXTENSAO, "sem extensao recusado");
  ok(caminho("fotos/x.jpg.exe") == PathError::EXTENSAO,
     "extensao dupla: vale o ULTIMO sufixo, nao o do meio");
  ok(caminho("fotos/.jpg") == PathError::EXTENSAO, "'.jpg' sozinho nao tem nome");
  ok(caminho("fotos/.jpeg") == PathError::EXTENSAO, "'.jpeg' sozinho nao tem nome");

  // Caixa: camera e celular gravam ".JPG" tanto quanto ".jpg".
  std::string p;
  ok(caminho("fotos/DSC00123.JPG", &p) == PathError::OK && p == "/M5RETRO/fotos/DSC00123.JPG",
     ".JPG maiusculo aceito");
  ok(caminho("fotos/DSC00123.JPEG") == PathError::OK, ".JPEG maiusculo aceito");
  ok(caminho("fotos/x.JpG") == PathError::OK, ".JpG misturado aceito");
  ok(caminho("fotos/x.jPeG") == PathError::OK, ".jPeG misturado aceito");

  // A extensao e conferida no ARQUIVO, nao na pasta.
  ok(caminho("fotos/verao.jpg/x.png") == PathError::EXTENSAO,
     "pasta terminada em .jpg nao libera o arquivo");
  ok(caminho("fotos/verao/x.jpg") == PathError::OK, "pasta sem extensao, arquivo .jpg: aceito");

  // Mensagem existe e e ASCII.
  const char *texto = pathErrorText(PathError::EXTENSAO);
  bool asciiPuro = true;
  for (const char *q = texto; *q; ++q)
    if ((unsigned char)*q < 0x20 || (unsigned char)*q >= 0x7f)
      asciiPuro = false;
  ok(asciiPuro && strlen(texto) > 0, "pathErrorText(EXTENSAO) e ASCII e nao vazio");
  printf("        -> \"%s\"\n", texto);
  printf("        -> RAIZ agora diz: \"%s\"\n", pathErrorText(PathError::RAIZ));
}

// --------------------------------------------------------------------------
// 3. Travessia de diretorio NA CATEGORIA NOVA
//    A categoria nova nao pode virar um caminho para fora de /M5RETRO. Cada
//    caso aqui e recusado ANTES de a extensao sequer ser olhada.
// --------------------------------------------------------------------------
static void testaTravessia() {
  printf("\n[3] travessia recusada em fotos/\n");

  ok(caminho("fotos/../../etc/passwd") == PathError::ESCAPA, "fotos/../../etc/passwd");
  ok(caminho("fotos/../config/secrets.json") == PathError::ESCAPA, "sobe para config/");
  ok(caminho("fotos/../../../../../../etc/passwd.jpg") == PathError::ESCAPA,
     "mesmo terminando em .jpg, o '..' reprova antes");
  ok(caminho("fotos/..") == PathError::ESCAPA, "'..' como ultimo segmento");
  ok(caminho("fotos/./x.jpg") == PathError::ESCAPA, "'.' no meio");
  ok(caminho("fotos/a/../../b.jpg") == PathError::ESCAPA, "'..' aninhado");

  // Percent-encoding: decodificar ANTES de validar e o que pega o disfarce.
  ok(caminho("fotos/%2e%2e/segredo.jpg") == PathError::ESCAPA, "'..' escapado em %2e%2e");
  ok(caminho("fotos/%2E%2E%2Fconfig/secrets.json") == PathError::ESCAPA,
     "'../' escapado com %2F");
  ok(caminho("%66otos/../config/x.json") == PathError::ESCAPA,
     "'fotos' escapado nao muda a regra do '..'");

  // Caminho absoluto, em qualquer forma.
  ok(caminho("/M5RETRO/fotos/x.jpg") == PathError::ABSOLUTO, "absoluto recusado");
  ok(caminho("%2fM5RETRO/fotos/x.jpg") == PathError::ABSOLUTO, "absoluto escapado recusado");
  ok(caminho("/etc/passwd") == PathError::ABSOLUTO, "/etc/passwd");

  // Nomes parecidos com a categoria nao valem: a lista e fechada.
  ok(caminho("Fotos/x.jpg") == PathError::RAIZ, "'Fotos' com maiuscula nao e categoria");
  ok(caminho("fotosx/x.jpg") == PathError::RAIZ, "'fotosx' nao e categoria");
  ok(caminho("fot/x.jpg") == PathError::RAIZ, "'fot' nao e categoria");
  ok(caminho("config/secrets.json") == PathError::RAIZ, "config/ segue fora de alcance");
  ok(caminho("fotos") == PathError::INCOMPLETO, "so a pasta, sem arquivo");
  ok(caminho("fotos/") == PathError::VAZIO, "barra no fim");
  ok(caminho("fotos//x.jpg") == PathError::VAZIO, "barra dupla");

  // Caracteres proibidos no FAT / fora do ASCII continuam valendo em fotos/.
  ok(caminho("fotos/x\\y.jpg") == PathError::CARACTERE, "barra invertida");
  ok(caminho("fotos/c:x.jpg") == PathError::CARACTERE, "dois-pontos");
  ok(caminho("fotos/x%00.jpg") == PathError::CARACTERE, "NUL escapado");
  ok(caminho("fotos/f%C3%A9rias.jpg") == PathError::CARACTERE, "acento em UTF-8");
  ok(caminho("fotos/x.jpg ") == PathError::CARACTERE, "espaco no fim");

  // Todo caminho aceito comeca no prefixo do cartao, e sobra folga para o
  // ".part" — a atomicidade nao depende da categoria.
  char completo[PATH_CAP], parcial[PATH_CAP];
  const bool sane = sanitizePath("fotos/verao/dia1.jpg", completo, sizeof(completo)) == PathError::OK;
  ok(sane && strncmp(completo, CARD_ROOT, sizeof(CARD_ROOT) - 1) == 0,
     "caminho aceito comeca em /M5RETRO/");
  ok(sane && partPath(completo, parcial, sizeof(parcial)) &&
         std::string(parcial) == std::string(completo) + ".part",
     "o .part da foto e montado sem estourar o buffer");
}

// --------------------------------------------------------------------------
// 4. Teto de tamanho (espelha MAX_JPEG em src/main.cpp)
// --------------------------------------------------------------------------
static void testaTamanho() {
  printf("\n[4] teto de 128 KB\n");
  ok(MAX_PHOTO == 128u * 1024u, "MAX_PHOTO = 131072 (espelha MAX_JPEG)");
  ok(fitsPhotoBudget(1), "1 byte cabe");
  ok(fitsPhotoBudget(MAX_PHOTO - 1), "131071 cabe");
  ok(fitsPhotoBudget(MAX_PHOTO), "131072 cabe (limite inclusivo)");
  ok(!fitsPhotoBudget(MAX_PHOTO + 1), "131073 nao cabe");
  ok(!fitsPhotoBudget(2u * 1024u * 1024u), "foto tipica de celular (2 MB) recusada");
  ok(!fitsPhotoBudget(MAX_FILE), "4 GiB-1 recusado");
}

// --------------------------------------------------------------------------
// 5. Cabecalho JPEG: progressivo e 4:4:4 nao abrem no aparelho
// --------------------------------------------------------------------------

// Monta um cabecalho JPEG minimo: SOI, um APP0 de recheio opcional e o SOF
// pedido. `sofMarker` = 0xC0 baseline, 0xC2 progressivo.
static std::vector<uint8_t> cabecalho(uint8_t sofMarker, uint8_t amostragemY, int recheio = 0) {
  std::vector<uint8_t> j;
  j.push_back(0xFF);
  j.push_back(0xD8); // SOI
  if (recheio > 0) { // APP1 de recheio, como um EXIF grande
    const int len = recheio + 2;
    j.push_back(0xFF);
    j.push_back(0xE1);
    j.push_back(uint8_t(len >> 8));
    j.push_back(uint8_t(len & 0xFF));
    for (int i = 0; i < recheio; ++i)
      j.push_back(0x00);
  }
  j.push_back(0xFF);
  j.push_back(sofMarker);
  j.push_back(0x00);
  j.push_back(0x11); // 17 bytes de payload: 3 componentes
  j.push_back(0x08); // precisao
  j.push_back(0x00);
  j.push_back(0xF0); // altura 240
  j.push_back(0x01);
  j.push_back(0x40);        // largura 320
  j.push_back(0x03);        // 3 componentes
  j.push_back(0x01);        // id do Y
  j.push_back(amostragemY); // amostragem do Y
  j.push_back(0x00);
  j.push_back(0x02);
  j.push_back(0x11);
  j.push_back(0x01);
  j.push_back(0x03);
  j.push_back(0x11);
  j.push_back(0x01);
  j.push_back(0xFF);
  j.push_back(0xDA); // SOS
  j.push_back(0x00);
  j.push_back(0x08);
  for (int i = 0; i < 6; ++i)
    j.push_back(0x00);
  return j;
}

static JpegVerdict veredito(const std::vector<uint8_t> &j) {
  return inspectJpegHeader(j.empty() ? NULL : &j[0], j.size());
}

static void testaJpeg() {
  printf("\n[5] cabecalho JPEG\n");

  ok(veredito(cabecalho(0xC0, 0x22)) == JpegVerdict::OK, "baseline 4:2:0 (Y 2x2) aceito");
  ok(veredito(cabecalho(0xC0, 0x21)) == JpegVerdict::OK, "baseline 4:2:2 (Y 2x1) aceito");
  ok(veredito(cabecalho(0xC1, 0x22)) == JpegVerdict::OK, "SOF1 sequencial estendido aceito");
  ok(veredito(cabecalho(0xC0, 0x22, 4000)) == JpegVerdict::OK,
     "baseline atras de um APP1 de 4 KB ainda e achado");

  ok(veredito(cabecalho(0xC2, 0x22)) == JpegVerdict::PROGRESSIVO, "SOF2 progressivo recusado");
  ok(veredito(cabecalho(0xC6, 0x22)) == JpegVerdict::PROGRESSIVO, "SOF6 progressivo recusado");
  ok(veredito(cabecalho(0xCA, 0x22)) == JpegVerdict::PROGRESSIVO, "SOF10 progressivo recusado");
  ok(veredito(cabecalho(0xC2, 0x22, 4000)) == JpegVerdict::PROGRESSIVO,
     "progressivo atras de EXIF grande tambem e pego");

  ok(veredito(cabecalho(0xC0, 0x11)) == JpegVerdict::CHROMA_444, "baseline 4:4:4 (Y 1x1) recusado");

  std::vector<uint8_t> vazio;
  ok(veredito(vazio) == JpegVerdict::NAO_E_JPEG, "arquivo vazio recusado");
  std::vector<uint8_t> texto;
  const char *msg = "isto nao e uma foto, e um script";
  for (const char *q = msg; *q; ++q)
    texto.push_back((uint8_t)*q);
  ok(veredito(texto) == JpegVerdict::NAO_E_JPEG, "arquivo sem SOI recusado");

  std::vector<uint8_t> mz;
  mz.push_back('M');
  mz.push_back('Z'); // executavel renomeado para .jpg
  ok(veredito(mz) == JpegVerdict::NAO_E_JPEG, "executavel renomeado para .jpg recusado");

  // Sem prova, nao se acusa: um cabecalho cortado passa e o slideshow que se
  // defenda. Recusar aqui derrubaria foto boa.
  std::vector<uint8_t> cortado = cabecalho(0xC0, 0x22);
  cortado.resize(8);
  ok(veredito(cortado) == JpegVerdict::INDETERMINADO, "cabecalho cortado: INDETERMINADO");
  ok(jpegRejectText(JpegVerdict::INDETERMINADO) == NULL, "INDETERMINADO nao vira recusa");
  ok(jpegRejectText(JpegVerdict::OK) == NULL, "OK nao vira recusa");

  // As mensagens vao para o corpo HTTP E para a faixa de estado da TV: ASCII e
  // curtas (~44 caracteres antes de a fonte fina truncar).
  const JpegVerdict ruins[3] = {JpegVerdict::NAO_E_JPEG, JpegVerdict::PROGRESSIVO,
                                JpegVerdict::CHROMA_444};
  bool todasBoas = true;
  for (int i = 0; i < 3; ++i) {
    const char *t = jpegRejectText(ruins[i]);
    if (!t || strlen(t) == 0 || strlen(t) > 44) {
      todasBoas = false;
      continue;
    }
    for (const char *q = t; *q; ++q)
      if ((unsigned char)*q < 0x20 || (unsigned char)*q >= 0x7f)
        todasBoas = false;
    printf("        -> \"%s\" (%d caracteres)\n", t, (int)strlen(t));
  }
  ok(todasBoas, "motivos ASCII e de ate 44 caracteres");
}

int main() {
  printf("probe transfer_fotos — categoria fotos/ do envio por Wi-Fi\n");
  testaRoteamento();
  testaExtensao();
  testaTravessia();
  testaTamanho();
  testaJpeg();
  printf("\n%d checagens, %d falha(s)\n", checagens, falhas);
  return falhas ? 1 : 0;
}
