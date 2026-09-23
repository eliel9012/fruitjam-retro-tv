// Testes nativos do modulo de envio por Wi-Fi (include/FileTransfer.h).
//
// `FILETRANSFER_NO_NETWORK` corta a metade do header que depende de Arduino,
// WiFi e SD. O que sobra e exatamente a logica que erra em silencio no
// aparelho — validacao de caminho, comparacao de senha, Basic, Range e a
// instalacao atomica — e e essa a parte que estes testes exercitam. Socket nao
// se testa aqui; e por isso que a logica foi separada da rede.
#define FILETRANSFER_NO_NETWORK
#include "FileTransfer.h"
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <string>

using namespace xfer;

// Cartao de mentira com a mesma superficie que este modulo usa do fs::SDFS:
// exists / remove / rename / mkdir, todos com const char*.
struct FakeCard {
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;
  std::string failRename, failRemove, failMkdir;

  bool exists(const char *p) const { return files.count(p) || dirs.count(p); }
  bool remove(const char *p) {
    if (failRemove == p)
      return false;
    return files.erase(p) > 0;
  }
  bool rename(const char *from, const char *to) {
    if (failRename == from || !files.count(from) || exists(to))
      return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool mkdir(const char *p) {
    if (failMkdir == p)
      return false;
    dirs.insert(p);
    return true;
  }
};

static PathError check(const char *raw, std::string *out = nullptr) {
  char buffer[PATH_CAP];
  memset(buffer, 0x7e, sizeof(buffer)); // lixo, para pegar quem nao termina a string
  const PathError result = sanitizePath(raw, buffer, sizeof(buffer));
  if (result == PathError::OK && out)
    *out = buffer;
  return result;
}

// --------------------------------------------------------------------------
// Validacao de caminho: a unica entrada nao confiavel deste modulo.
// --------------------------------------------------------------------------
void testPathValidation() {
  std::string path;

  // Caminhos legitimos, e so eles, viram caminho absoluto no cartao.
  assert(check("videos/meu-filme/video.mjpeg", &path) == PathError::OK);
  assert(path == "/M5RETRO/videos/meu-filme/video.mjpeg");
  assert(check("music/faixa.wav", &path) == PathError::OK);
  assert(path == "/M5RETRO/music/faixa.wav");
  assert(check("videos/a/b/c/d.mjpeg", &path) == PathError::OK);
  assert(path == "/M5RETRO/videos/a/b/c/d.mjpeg");

  // Percent-encoding e decodificado ANTES da validacao; senao "%2e%2e" passava.
  assert(check("videos/meu%20filme/video.mjpeg", &path) == PathError::OK);
  assert(path == "/M5RETRO/videos/meu filme/video.mjpeg");
  assert(check("videos/%2e%2e/segredo.json") == PathError::ESCAPA);
  assert(check("videos/%2E%2E%2Fconfig/secrets.json") == PathError::ESCAPA);
  assert(check("%76ideos/x.wav", &path) == PathError::OK); // "videos" escapado continua valendo
  assert(path == "/M5RETRO/videos/x.wav");

  // Subida de diretorio, em todas as posicoes.
  assert(check("videos/../config/secrets.json") == PathError::ESCAPA);
  assert(check("../config/secrets.json") == PathError::ESCAPA); // ".." reprova antes da raiz
  assert(check("videos/..") == PathError::ESCAPA);
  assert(check("videos/sub/../../config/x") == PathError::ESCAPA);
  assert(check("videos/./x.wav") == PathError::ESCAPA);
  assert(check("videos/...") == PathError::CARACTERE); // ponto no fim: o FAT o descarta

  // Caminho absoluto, em qualquer forma.
  assert(check("/M5RETRO/videos/x.mjpeg") == PathError::ABSOLUTO);
  assert(check("/etc/passwd") == PathError::ABSOLUTO);
  assert(check("%2fM5RETRO/videos/x.mjpeg") == PathError::ABSOLUTO);

  // Fora das duas pastas permitidas nao se grava nada.
  assert(check("config/secrets.json") == PathError::RAIZ);
  assert(check("cache/aircraft.json") == PathError::RAIZ);
  assert(check("Videos/x.mjpeg") == PathError::RAIZ); // comparacao e sensivel a caixa
  assert(check("videosx/x.mjpeg") == PathError::RAIZ);
  assert(check("videos") == PathError::INCOMPLETO);
  assert(check("music") == PathError::INCOMPLETO);
  assert(check("videos/") == PathError::VAZIO);
  assert(check("") == PathError::VAZIO);
  assert(check("videos//x.wav") == PathError::VAZIO);

  // Caracteres que o FAT nao aceita, ou que nao tem glifo nas fontes bitmap.
  assert(check("videos/x\\y.wav") == PathError::CARACTERE);
  assert(check("videos/c:x.wav") == PathError::CARACTERE);
  assert(check("videos/x*.wav") == PathError::CARACTERE);
  assert(check("videos/x|y.wav") == PathError::CARACTERE);
  assert(check("videos/\"x\".wav") == PathError::CARACTERE);
  assert(check("videos/musi%C3%A7a.wav") == PathError::CARACTERE); // acento em UTF-8
  assert(check("videos/x%0Ay.wav") == PathError::CARACTERE);       // quebra de linha
  assert(check("videos/x%00.wav") == PathError::CARACTERE);        // NUL truncaria depois
  assert(check("videos/x%2.wav") == PathError::CARACTERE);         // escape quebrado
  assert(check("videos/x%zz.wav") == PathError::CARACTERE);
  assert(check("videos/x.wav ") == PathError::CARACTERE); // espaco no fim: o FAT o descarta

  // A query nao faz parte do caminho.
  assert(check("videos/x.wav?cache=0", &path) == PathError::OK);
  assert(path == "/M5RETRO/videos/x.wav");

  // Limites de tamanho.
  assert(check(("videos/" + std::string(64, 'a') + ".wav").c_str()) == PathError::LONGO);
  assert(check(("videos/" + std::string(64, 'a')).c_str()) == PathError::OK);
  std::string deep = "videos";
  for (int i = 0; i < 13; ++i) // 149 bytes: passa do teto depois do prefixo e do ".part"
    deep += "/abcdefghij";
  assert(check(deep.c_str()) == PathError::LONGO);
  deep = "videos";
  for (int i = 0; i < 12; ++i)
    deep += "/abcdefghij";
  assert(check(deep.c_str()) == PathError::OK); // 138 bytes ainda cabem

  // Um buffer curto demais nunca e estourado: e recusado.
  char tiny[8];
  assert(sanitizePath("videos/x.wav", tiny, sizeof(tiny)) == PathError::LONGO);
  assert(sanitizePath(nullptr, tiny, sizeof(tiny)) == PathError::LONGO);

  // O caminho valido sempre deixa folga para o sufixo ".part".
  char full[PATH_CAP], part[PATH_CAP];
  assert(sanitizePath(("videos/" + std::string(64, 'a')).c_str(), full, sizeof(full)) == PathError::OK);
  assert(partPath(full, part, sizeof(part)));
  assert(std::string(part) == std::string(full) + ".part");

  char name[MAX_SEGMENT + 1];
  baseName("/M5RETRO/videos/meu-filme/video.mjpeg", name, sizeof(name));
  assert(std::string(name) == "video.mjpeg");
  baseName("sembarra", name, sizeof(name));
  assert(std::string(name) == "sembarra");
  char narrow[4];
  baseName("/M5RETRO/music/faixa.wav", narrow, sizeof(narrow));
  assert(std::string(narrow) == "fai"); // trunca, mas sempre termina
}

// --------------------------------------------------------------------------
// Senha: comparacao em tempo constante e sem saida antecipada.
// --------------------------------------------------------------------------
void testSecretCompare() {
  assert(secretEquals("K7MPX4TR", "K7MPX4TR"));
  assert(!secretEquals("K7MPX4TR", "K7MPX4TS"));  // ultimo caractere
  assert(!secretEquals("K7MPX4TR", "Z7MPX4TR"));  // primeiro caractere
  assert(!secretEquals("K7MPX4TR", "K7MPX4T"));   // prefixo nao passa
  assert(!secretEquals("K7MPX4TR", "K7MPX4TRX")); // sufixo extra nao passa
  assert(!secretEquals("K7MPX4TR", ""));
  assert(secretEquals("", ""));
  assert(!secretEquals(nullptr, "x") && !secretEquals("x", nullptr));

  // Senha vazia (sessao parada) nao e aceita por um cliente que manda vazio
  // junto com um usuario qualquer: a checagem de usuario e separada. Aqui so
  // se garante que o comparador nao trata "" como coringa.
  assert(!secretEquals("", "qualquer"));

  // Um segredo maior que o teto e comparado ate o teto, sem ler alem dele.
  const std::string longo(MAX_SECRET, 'x');
  assert(secretEquals(longo.c_str(), longo.c_str()));
  assert(!secretEquals(longo.c_str(), (longo.substr(0, MAX_SECRET - 1) + "y").c_str()));
}

// --------------------------------------------------------------------------
// HTTP Basic: base64 estrito e quebra em usuario/senha.
// --------------------------------------------------------------------------
void testBasicAuth() {
  uint8_t out[MAX_CRED];
  assert(base64Decode("bTVyZXRybzpLN01QWDRUUg==", 24, out, sizeof(out)) == 16);
  assert(memcmp(out, "m5retro:K7MPX4TR", 16) == 0);
  assert(base64Decode("", 0, out, sizeof(out)) == 0);
  assert(base64Decode("YQ==", 4, out, sizeof(out)) == 1 && out[0] == 'a');
  assert(base64Decode("YWI=", 4, out, sizeof(out)) == 2);
  assert(base64Decode("YWJj", 4, out, sizeof(out)) == 3);
  assert(base64Decode("YWJ", 3, out, sizeof(out)) == -1);    // comprimento invalido
  assert(base64Decode("YW!j", 4, out, sizeof(out)) == -1);   // fora do alfabeto
  assert(base64Decode("=WJj", 4, out, sizeof(out)) == -1);   // padding no inicio
  assert(base64Decode("Y=Jj", 4, out, sizeof(out)) == -1);   // padding no meio
  assert(base64Decode("YQ==YWJj", 8, out, sizeof(out)) == -1); // padding fora do ultimo grupo
  assert(base64Decode("YWJj", 4, out, 2) == -1);             // nao cabe

  char user[MAX_SECRET + 1], pass[MAX_SECRET + 1];
  assert(parseBasicAuth("Basic bTVyZXRybzpLN01QWDRUUg==", user, sizeof(user), pass, sizeof(pass)));
  assert(std::string(user) == "m5retro" && std::string(pass) == "K7MPX4TR");
  // Esquema sem sensibilidade a caixa, com espaco extra e \t, como um cliente
  // qualquer pode mandar.
  assert(parseBasicAuth("  bASIC \t bTVyZXRybzpLN01QWDRUUg==  ", user, sizeof(user), pass, sizeof(pass)));
  assert(std::string(user) == "m5retro" && std::string(pass) == "K7MPX4TR");
  // Senha com ':' dentro: so o primeiro dois-pontos separa.
  assert(parseBasicAuth("Basic YTpiOmM=", user, sizeof(user), pass, sizeof(pass)));
  assert(std::string(user) == "a" && std::string(pass) == "b:c");
  // Senha vazia e usuario vazio sao representaveis, e ainda assim reprovam no
  // secretEquals contra a senha real.
  assert(parseBasicAuth("Basic YTo=", user, sizeof(user), pass, sizeof(pass)));
  assert(std::string(user) == "a" && std::string(pass).empty());
  assert(!secretEquals("K7MPX4TR", pass));

  assert(!parseBasicAuth("Bearer abc", user, sizeof(user), pass, sizeof(pass)));
  assert(!parseBasicAuth("Basic", user, sizeof(user), pass, sizeof(pass)));
  assert(!parseBasicAuth("Basic ", user, sizeof(user), pass, sizeof(pass)));
  assert(!parseBasicAuth("Basic YWJj", user, sizeof(user), pass, sizeof(pass))); // sem ':'
  assert(!parseBasicAuth("Basic !!!!", user, sizeof(user), pass, sizeof(pass)));
  assert(!parseBasicAuth(nullptr, user, sizeof(user), pass, sizeof(pass)));
  // Byte de controle nas credenciais e recusado: seria injecao de cabecalho se
  // algum dia esse texto voltasse numa resposta.
  assert(!parseBasicAuth("Basic YQ0KOmI=", user, sizeof(user), pass, sizeof(pass)));
  // Nao cabe no buffer do chamador.
  char curto[4];
  assert(!parseBasicAuth("Basic bTVyZXRybzpLN01QWDRUUg==", curto, sizeof(curto), pass, sizeof(pass)));
  assert(!parseBasicAuth("Basic bTVyZXRybzpLN01QWDRUUg==", user, sizeof(user), curto, sizeof(curto)));
}

// --------------------------------------------------------------------------
// Range: so o inicio interessa a um envio.
// --------------------------------------------------------------------------
void testRange() {
  uint64_t start = 0xdead;
  assert(parseRange("bytes=0-", start) && start == 0);
  assert(parseRange("bytes=1048576-", start) && start == 1048576);
  assert(parseRange("bytes=1000-2000", start) && start == 1000);
  assert(parseRange("  BYTES = 42-  ", start) && start == 42); // caixa e espaco ao redor do '='
  assert(!parseRange("bytes=42 -", start)); // espaco antes do hifen nao e Range valido
  assert(parseRange("bytes=4294967295-", start) && start == 4294967295ull);

  start = 0xdead;
  assert(!parseRange("bytes=-500", start));         // sufixo nao serve para envio
  assert(!parseRange("bytes=0-10,20-30", start));   // varios intervalos
  assert(!parseRange("bytes=2000-1000", start));    // fim antes do inicio
  assert(!parseRange("bytes=abc-", start));
  assert(!parseRange("bytes=10", start));           // sem o hifen
  assert(!parseRange("bytes=", start));
  assert(!parseRange("items=0-", start));           // outra unidade
  assert(!parseRange("byte=0-", start));
  assert(!parseRange("by", start));                 // curto demais, sem ler alem do fim
  assert(!parseRange("", start));
  assert(!parseRange(nullptr, start));
  assert(!parseRange("bytes=4294967296-", start));  // acima do teto do FAT32
  assert(!parseRange("bytes=99999999999999999999-", start));
  assert(!parseRange("bytes=0-x", start));
  assert(!parseRange("bytes=+10-", start));
  assert(start == 0xdead); // nenhum caminho de falha escreve na saida
}

// --------------------------------------------------------------------------
// Espaco livre.
// --------------------------------------------------------------------------
void testFreeSpace() {
  const uint64_t mib = 1024ull * 1024ull;
  assert(fitsOnCard(50 * mib, 40 * mib, mib));
  assert(!fitsOnCard(50 * mib, 50 * mib, mib)); // a reserva nao pode ser comida
  assert(fitsOnCard(50 * mib, 49 * mib, mib));
  assert(!fitsOnCard(0, 1, 0) && fitsOnCard(1, 1, 0));
  assert(!fitsOnCard(0xFFFFFFFFFFFFFFFFull, MAX_FILE + 1, 0)); // maior que o FAT32 aguenta
  // Soma que daria a volta no uint64 nao pode virar "cabe".
  assert(!fitsOnCard(0xFFFFFFFFFFFFFFFFull, MAX_FILE, 0xFFFFFFFFFFFFFFFFull));
}

// --------------------------------------------------------------------------
// Instalacao atomica: o cartao nunca fica com um arquivo final pela metade.
// --------------------------------------------------------------------------
void testAtomicInstall() {
  const char *finalPath = "/M5RETRO/videos/meu-filme/video.mjpeg";
  const char *part = "/M5RETRO/videos/meu-filme/video.mjpeg.part";

  // Ciclo feliz: enquanto o envio corre so existe o `.part`; o nome final so
  // aparece depois do rename, e ja com o conteudo inteiro.
  FakeCard card;
  card.files[part] = "meta";
  assert(!card.exists(finalPath));
  card.files[part] = "metade do video";
  assert(!card.exists(finalPath)); // o player nunca ve um arquivo pela metade
  card.files[part] = "video inteiro";
  assert(installFile(card, part, finalPath));
  assert(card.files[finalPath] == "video inteiro" && !card.exists(part));

  // Sem `.part` nao ha o que instalar.
  assert(!installFile(card, "/M5RETRO/videos/x.part", "/M5RETRO/videos/x"));

  // Reenvio por cima de um arquivo que ja existe: o FAT nao deixa o rename
  // sobrescrever, entao o antigo sai primeiro.
  card.files[part] = "video novo";
  assert(installFile(card, part, finalPath));
  assert(card.files[finalPath] == "video novo" && !card.exists(part));

  // Se o antigo nao pode ser apagado, nada acontece: nem o novo entra, nem o
  // velho some, e o `.part` continua ali para uma nova tentativa.
  card.files[part] = "tentativa";
  card.failRemove = finalPath;
  assert(!installFile(card, part, finalPath));
  assert(card.files[finalPath] == "video novo" && card.files[part] == "tentativa");
  card.failRemove.clear();

  // Se o rename falhar, o `.part` FICA. E o que permite retomar em vez de
  // reenviar 45 MB.
  card.failRename = part;
  assert(!installFile(card, part, finalPath));
  assert(card.files[part] == "tentativa");
  assert(!card.exists(finalPath)); // o destino ja tinha sido removido
  card.failRename.clear();
  assert(installFile(card, part, finalPath));
  assert(card.files[finalPath] == "tentativa");

  // O nome do `.part` sai do nome final, sem alocar.
  char buffer[PATH_CAP];
  assert(partPath(finalPath, buffer, sizeof(buffer)));
  assert(std::string(buffer) == part);
  char curto[8];
  assert(!partPath(finalPath, curto, sizeof(curto)));
  assert(!partPath("", buffer, sizeof(buffer)));
}

// --------------------------------------------------------------------------
// Criacao das pastas intermediarias.
// --------------------------------------------------------------------------
void testEnsureParents() {
  FakeCard card;
  char scratch[PATH_CAP];
  const char *path = "/M5RETRO/videos/meu-filme/video.mjpeg.part";
  assert(ensureParents(card, path, scratch, sizeof(scratch)));
  assert(card.dirs.count("/M5RETRO") && card.dirs.count("/M5RETRO/videos"));
  assert(card.dirs.count("/M5RETRO/videos/meu-filme"));
  assert(!card.exists(path)); // o ultimo segmento e o arquivo, nao uma pasta
  // O buffer do chamador volta intacto: o '/' e reposto a cada passo.
  assert(std::string(scratch) == path);

  FakeCard blocked;
  blocked.failMkdir = "/M5RETRO/videos";
  assert(!ensureParents(blocked, path, scratch, sizeof(scratch)));

  // Caminho que nao cabe no scratch e recusado em vez de estourar.
  char tiny[8];
  assert(!ensureParents(card, path, tiny, sizeof(tiny)));
  assert(!ensureParents(card, "", scratch, sizeof(scratch)));
}

// Acao do controle remoto. O que chega aqui veio da URL, entao o que interessa
// e o que e RECUSADO: uma acao que passasse com '/' ou '%' viraria caminho, e
// uma truncada silenciosamente poderia virar outra acao valida.
void testControlAction() {
  char a[16];
  assert(sanitizeAction("ok", a, sizeof(a)) && !strcmp(a, "ok"));
  assert(sanitizeAction("acima", a, sizeof(a)) && !strcmp(a, "acima"));
  assert(sanitizeAction("inicio", a, sizeof(a)) && !strcmp(a, "inicio"));

  // Recusados, e em todos os casos `out` sai vazio.
  const char *ruins[] = {"",        "OK",      "ok/",     "ok ",    "ok\t",
                         "../ok",   "ok%2f",   "ok1",     "o k",    "ok.",
                         "ok-play", "ok\n"};
  for (const char *r : ruins) {
    assert(!sanitizeAction(r, a, sizeof(a)));
    assert(a[0] == '\0');
  }
  assert(!sanitizeAction(nullptr, a, sizeof(a)) && a[0] == '\0');

  // Longa demais recusa em vez de truncar: truncar "abaixomuito" para "abaixo"
  // executaria um comando que ninguem pediu.
  char curto[4];
  assert(sanitizeAction("ok", curto, sizeof(curto)) && !strcmp(curto, "ok"));
  assert(!sanitizeAction("abaixo", curto, sizeof(curto)) && curto[0] == '\0');
  assert(!sanitizeAction("ok", a, 0));
}

int main() {
  testPathValidation();
  testSecretCompare();
  testBasicAuth();
  testRange();
  testFreeSpace();
  testAtomicInstall();
  testEnsureParents();
  testControlAction();
  std::cout << "PASS: caminho, senha em tempo constante, Basic, Range, espaco, instalacao "
               "atomica, acao do controle\n";
}
