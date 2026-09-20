#pragma once
// FAT rename cannot replace a destination. Keep the previous complete file
// until its replacement is installed, and recover interrupted renames at boot.
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
