#include "SecretsManager.h"
#include "SafeStorage.h"
#include <SD.h>
#include <ArduinoJson.h>
#include <math.h>

static const char *CFG_DIR = "/M5RETRO/config";
static const char *SECRETS = "/M5RETRO/config/secrets.json";
static const char *SETTINGS = "/M5RETRO/config/settings.json";

bool SecretsManager::begin() {
  return (SD.exists("/M5RETRO") || SD.mkdir("/M5RETRO")) && (SD.exists(CFG_DIR) || SD.mkdir(CFG_DIR)) &&
         storage::recover(SD, String(SECRETS)) && storage::recover(SD, String(SETTINGS));
}
bool SecretsManager::hasSecrets() const {
  return SD.exists(SECRETS);
}
bool SecretsManager::atomicWrite(const char *path, const String &data) {
  return begin() && storage::replace(SD, String(path), data);
}
void SecretsManager::createExampleIfMissing() {
  if (!begin())
    return;
  const char *example = "/M5RETRO/config/secrets.example.json";
  if (SD.exists(example))
    return;
  atomicWrite(example, "{\n  \"wifi_ssid\": \"NOME_DA_REDE\",\n  \"wifi_password\": \"SENHA_DA_REDE\",\n  "
                       "\"api_base_url\": \"https://seu-servidor.example\",\n  \"aircraft_endpoint\": "
                       "\"/aircraft\",\n  \"auth_mode\": \"bearer\",\n  \"api_token\": \"TOKEN\"\n}\n");
}
bool SecretsManager::load(SecretsConfig &s, RadarConfig &r) {
  if (!begin())
    return false;
  // Offline users still need volume, video and radar preferences.
  File f;
  if (SD.exists(SETTINGS))
    f = SD.open(SETTINGS, FILE_READ);
  if (f) {
    JsonDocument d;
    if (!deserializeJson(d, f)) {
      r.latitude = d["radar_center_lat"] | 0.0;
      r.longitude = d["radar_center_lon"] | 0.0;
      r.rangeKm = d["radar_radius_km"] | 250;
      r.refreshSeconds = d["refresh_seconds"] | 5;
      r.volume = constrain(d["volume"] | 75, 0, 100);
      r.experimental320 = String(d["video_quality"] | "240x160") == "320x240";
      r.vhsOsd = d["vhs_osd"] | true;
      r.audioOutput = String(d["audio_output"] | "rca");
      if (!isfinite(r.latitude) || r.latitude < -90 || r.latitude > 90)
        r.latitude = 0;
      if (!isfinite(r.longitude) || r.longitude < -180 || r.longitude > 180)
        r.longitude = 0;
      if (r.rangeKm != 50 && r.rangeKm != 100 && r.rangeKm != 250 && r.rangeKm != 500)
        r.rangeKm = 250;
      if (r.refreshSeconds != 5 && r.refreshSeconds != 10 && r.refreshSeconds != 30)
        r.refreshSeconds = 5;
      if (r.audioOutput != "rca" && r.audioOutput != "interno" && r.audioOutput != "mudo")
        r.audioOutput = "rca";
    }
    f.close();
  }
  if (SD.exists(SECRETS))
    f = SD.open(SECRETS, FILE_READ);
  if (!f) {
    createExampleIfMissing();
    return false;
  }
  JsonDocument d;
  auto err = deserializeJson(d, f);
  f.close();
  if (err)
    return false;
  s.ssid = d["wifi_ssid"] | "";
  s.password = d["wifi_password"] | "";
  s.baseUrl = d["api_base_url"] | "";
  s.endpoint = d["aircraft_endpoint"] | "";
  s.authMode = d["auth_mode"] | "bearer";
  s.authHeader = d["auth_header"] | "Authorization";
  s.authPrefix = d["auth_prefix"] | "Bearer ";
  s.token = d["api_token"] | "";
  s.allowInsecureTls = d["allow_insecure_tls"] | false;
  return s.valid();
}
bool SecretsManager::saveSettings(const RadarConfig &r) {
  JsonDocument d;
  d["radar_center_lat"] = r.latitude;
  d["radar_center_lon"] = r.longitude;
  d["radar_radius_km"] = r.rangeKm;
  d["refresh_seconds"] = r.refreshSeconds;
  d["volume"] = r.volume;
  d["video_quality"] = r.experimental320 ? "320x240" : "240x160";
  d["vhs_osd"] = r.vhsOsd;
  d["audio_output"] = r.audioOutput;
  String data;
  serializeJsonPretty(d, data);
  return atomicWrite(SETTINGS, data);
}
bool SecretsManager::save(const SecretsConfig &s, const RadarConfig &r) {
  if (!s.valid())
    return false;
  JsonDocument d;
  d["wifi_ssid"] = s.ssid;
  d["wifi_password"] = s.password;
  d["api_base_url"] = s.baseUrl;
  d["aircraft_endpoint"] = s.endpoint;
  d["auth_mode"] = s.authMode;
  d["auth_header"] = s.authHeader;
  d["auth_prefix"] = s.authPrefix;
  d["api_token"] = s.token;
  d["allow_insecure_tls"] = s.allowInsecureTls;
  String data;
  serializeJsonPretty(d, data);
  // Individual files are recoverable; the pair is not a filesystem transaction.
  return saveSettings(r) && atomicWrite(SECRETS, data);
}
bool SecretsManager::eraseNetworkConfiguration() {
  for (const char *suffix : {".bak", ".tmp", ""}) {
    String path = String(SECRETS) + suffix;
    if (SD.exists(path) && !SD.remove(path))
      return false;
  }
  return true;
}
