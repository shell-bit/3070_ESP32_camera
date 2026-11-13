#include "project_face_store.h"
#include "project_config.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include <cstdio>
#include <unistd.h>
namespace {
wl_handle_t handle = WL_INVALID_HANDLE;
std::string readSlot(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f)
    return {};
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  if (n < 0 || n > 256 * 1024) {
    fclose(f);
    return {};
  }
  std::string out(size_t(n), '\0');
  if (fread(out.data(), 1, out.size(), f) != out.size())
    out.clear();
  fclose(f);
  return out;
}
} // namespace
bool FaceStore::begin() {
  esp_vfs_fat_mount_config_t cfg{};
  cfg.format_if_mount_failed = PROJECT_FACE_ALLOW_FORMAT;
  cfg.max_files = 4;
  cfg.allocation_unit_size = 4096;
  if (esp_vfs_fat_spiflash_mount_rw_wl("/faces", "face_store", &cfg, &handle) != ESP_OK)
    return false;
  mounted = true;
  camera::Registry a, b;
  bool va = a.decode(readSlot("/faces/library0.bin")),
       vb = b.decode(readSlot("/faces/library1.bin"));
  if (va && (!vb || a.version > b.version))
    registry = a;
  else if (vb)
    registry = b;
  return true;
}
bool FaceStore::save(const camera::Registry &next) {
  if (!mounted)
    return false;
  std::string bytes = next.encode();
  const char *path = next.version % 2 ? "/faces/library1.bin" : "/faces/library0.bin";
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  camera::Registry verify;
  if (!ok || !verify.decode(readSlot(path)) || verify.version != next.version)
    return false;
  registry = std::move(verify);
  return true;
}
