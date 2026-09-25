#pragma once
// Gravação "atômica" de arquivo pequeno (settings.json, secrets.json) no FAT.
//
// O rename do FAT não substitui um destino que já existe: no arduino-pico o
// SdFat recria a entrada com O_CREAT | O_EXCL e FALHA se o nome estiver
// tomado. Então o arquivo completo anterior fica guardado como .bak até o novo
// estar no lugar, e um rename interrompido (queda de energia no meio) é
// recuperado no próximo boot por recover().
//
// ARMADILHA do arduino-pico (PORTING.md 3.7): lá FILE_WRITE é
// O_RDWR | O_CREAT | O_APPEND — anexa ao arquivo velho em vez de truncar, e o
// JSON sai inválido. Por isso o temporário é aberto com o modo de string "w"
// (OM_CREATE | OM_TRUNCATE), que trunca no ESP32 e no arduino-pico. Nunca
// troque por FILE_WRITE. O tests/test_core.cpp confere o modo.
//
// `FS` é qualquer coisa com exists/remove/rename/open(path, "w") — o `SD` do
// arduino-pico (SDClass, que não herda de fs::FS), o do ESP32 ou um falso nos
// testes. `Path` precisa de operator+ com const char* (String, std::string).
namespace storage {
template <class FS, class Path> bool recover(FS &fs, const Path &path) {
  const Path backup = path + ".bak";
  if (!fs.exists(path) && fs.exists(backup))
    return fs.rename(backup, path);
  return true;
}
template <class FS, class Path, class Data> bool replace(FS &fs, const Path &path, const Data &data) {
  if (!recover(fs, path))
    return false;
  const Path tmp = path + ".tmp", backup = path + ".bak";
  if (fs.exists(tmp) && !fs.remove(tmp))
    return false;
  auto file = fs.open(tmp, "w");
  if (!file)
    return false;
  const bool complete = file.print(data) == data.length();
  file.flush();
  file.close();
  if (!complete) {
    fs.remove(tmp);
    return false;
  }
  if (fs.exists(backup) && !fs.remove(backup))
    return false;
  const bool existed = fs.exists(path);
  if (existed && !fs.rename(path, backup))
    return false;
  if (!fs.rename(tmp, path)) {
    if (existed)
      fs.rename(backup, path);
    return false;
  }
  if (existed)
    fs.remove(backup);
  return true;
}
} // namespace storage
