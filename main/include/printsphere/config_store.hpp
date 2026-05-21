#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"

namespace printsphere {

enum class DisplayRotation : uint8_t {
  k0,
  k90,
  k180,
  k270,
};

const char* to_string(DisplayRotation rotation);
DisplayRotation parse_display_rotation(const std::string& value);

struct WifiCredentials {
  std::string ssid;
  std::string password;

  bool is_configured() const { return !ssid.empty(); }
};

struct BatteryDisplayPolicy {
  bool dim_enabled = true;
  int dim_brightness_percent = 0;
  bool screen_off_enabled = true;
  uint32_t dim_timeout_idle_s = 20;
  uint32_t dim_timeout_active_s = 30;
  uint32_t off_timeout_idle_s = 60;
  uint32_t off_timeout_active_s = 120;
  bool usb_power_save_enabled = false;
};

struct ArcColorScheme {
  uint32_t soc_high = 0x00FF00;
  uint32_t soc_mid = 0xFFA500;
  uint32_t soc_low = 0xFF3333;
  uint32_t charging = 0x3399FF;
  uint32_t discharging = 0xFFD54F;
  uint32_t idle = 0x666666;
  uint32_t offline = 0xFFFFFF;
};

struct DisplaySettings {
  int brightness_percent = 80;
  int contrast_percent = 50;
  bool invert = false;
  uint32_t screen_off_seconds = 60;
};

class ConfigStore {
 public:
  esp_err_t initialize();

  std::string load_device_name() const;
  WifiCredentials load_wifi_credentials() const;
  std::string load_status_url() const;
  DisplayRotation load_display_rotation() const;
  bool load_portal_lock_enabled() const;
  ArcColorScheme load_arc_color_scheme() const;
  DisplaySettings load_display_settings() const;
  BatteryDisplayPolicy load_battery_display_policy() const;
  std::string load_timezone_iana() const;

  esp_err_t save_wifi_credentials(const WifiCredentials& credentials) const;
  esp_err_t save_status_url(const std::string& url) const;
  esp_err_t save_display_rotation(DisplayRotation rotation) const;
  esp_err_t save_portal_lock_enabled(bool enabled) const;
  esp_err_t save_timezone_iana(const std::string& iana_name) const;
  esp_err_t save_arc_color_scheme(const ArcColorScheme& colors) const;
  esp_err_t save_display_settings(const DisplaySettings& settings) const;
  esp_err_t save_battery_display_policy(const BatteryDisplayPolicy& policy) const;

 private:
  esp_err_t save_string(const char* key, const std::string& value) const;
  std::string load_string(const char* key) const;
};

}  // namespace printsphere
