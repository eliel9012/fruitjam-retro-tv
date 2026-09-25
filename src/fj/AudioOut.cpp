// STUB — implementação pendente (ver PORTING.md, trabalho "hal-audio").
#include "fj/AudioOut.h"
namespace audioout {
bool begin(uint32_t) { return false; }
void end() {}
bool ready() { return false; }
bool setRate(uint32_t) { return false; }
uint32_t rate() { return 0; }
void setRoute(Route) {}
Route route() { return Route::MUTED; }
void setVolume(uint8_t) {}
size_t write(const int16_t *, size_t frames) { return frames; }
size_t availableForWrite() { return 0; }
void flushSilence() {}
} // namespace audioout
