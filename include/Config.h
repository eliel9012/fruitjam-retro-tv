#pragma once
#include <Arduino.h>
namespace cfg {
constexpr char ROOT[] = "/M5RETRO";
constexpr char VIDEOS[] = "/M5RETRO/videos";
constexpr char CONFIG[] = "/M5RETRO/config";
constexpr char SETTINGS[] = "/M5RETRO/config/settings.json";
constexpr char SECRETS[] = "/M5RETRO/config/secrets.json";
constexpr char CA[] = "/M5RETRO/config/ca.pem";
constexpr char CACHE[] = "/M5RETRO/cache/aircraft.json";
constexpr uint8_t CVBS_PIN = 26, RCA_BCK = 19, RCA_DATA = 2, RCA_LRCK = 0;
constexpr uint16_t CRT_W = 320, CRT_H = 240, VIDEO_W = 240, VIDEO_H = 160;
constexpr size_t MAX_JPEG = 128 * 1024, AUDIO_CHUNK = 1024;
constexpr uint32_t AUDIO_RATE = 22050;
constexpr char VERSION[] = "M5 RETRO TV v0.2";
} // namespace cfg
