#pragma once
#include <stdint.h>
enum UiState {
  BOOT,
  HOME,
  VIDEO_LIBRARY,
  VIDEO_PLAYBACK,
  AIRCRAFT_RADAR,
  SETTINGS,
  SYSTEM_INFO,
  SETUP_PORTAL,
  WEATHER,
  MUSIC_BROWSER,
  MUSIC_NOW_PLAYING,
  ERROR_SCREEN
};
inline UiState homeTarget(int selection) {
  const UiState targets[] = {VIDEO_LIBRARY, MUSIC_BROWSER, AIRCRAFT_RADAR, SETTINGS, SYSTEM_INFO, WEATHER};
  return selection >= 0 && selection < 6 ? targets[selection] : HOME;
}
inline int touchButton(int x, int y) {
  if (x < 0 || x >= 320 || y < 184 || y >= 240)
    return -1;
  return x < 107 ? 0 : x < 214 ? 1 : 2;
}
inline bool timeReached(uint32_t now, uint32_t deadline) {
  return int32_t(now - deadline) >= 0;
}

inline bool isPlayerAudioButton(int x, int y) {
  return x >= 232 && x < 316 && y >= 160 && y < 184;
}

inline bool isBackButton(int x, int y) {
  return x >= 274 && x < 320 && y >= 0 && y < 40;
}
