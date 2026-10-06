#include <cassert>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include "display_settings.h"

struct Storage {
  bool opens = true, writes = true, corrupt_readback = false;
  bool present = false, ended = false, read_only = true;
  uint8_t value = 0;
  unsigned write_count = 0;
  bool begin(const char* name, bool ro) {
    assert(std::strcmp(name, "toaster-ui") == 0);
    read_only = ro;
    return opens;
  }
  uint8_t getUChar(const char* key, uint8_t fallback) {
    assert(std::strcmp(key, "brightness") == 0);
    return present ? (corrupt_readback ? 0 : value) : fallback;
  }
  size_t putUChar(const char*, uint8_t next) {
    assert(!read_only);
    ++write_count;
    if (!writes) return 0;
    value = next; present = true;
    return 1;
  }
  void end() { ended = true; }
};

int main() {
  Storage storage;
  assert(loadDisplayBrightness(storage) == 100 && storage.ended);
  for (unsigned v = 0; v <= 255; ++v) {
    storage.value = static_cast<uint8_t>(v); storage.present = true;
    assert(loadDisplayBrightness(storage) == (v >= 1 && v <= 100 ? v : 100));
  }
  assert(saveDisplayBrightness(storage, 37));
  assert(loadDisplayBrightness(storage) == 37 && storage.write_count == 1);
  assert(!saveDisplayBrightness(storage, 0) && storage.write_count == 1);
  storage.opens = false;
  assert(loadDisplayBrightness(storage) == 100);
  assert(!saveDisplayBrightness(storage, 60));
  storage.opens = true; storage.writes = false;
  assert(!saveDisplayBrightness(storage, 60));
  storage.writes = true; storage.corrupt_readback = true;
  assert(!saveDisplayBrightness(storage, 60));
  std::puts("Display brightness defaults, bounds, persistence and write failures passed");
}
