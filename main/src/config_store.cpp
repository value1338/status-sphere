#include "printsphere/config_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "status.cfg";
constexpr char kNamespace[] = "statussphere";
constexpr char kDeviceName[] = "StatusSphere";
constexpr char kDefaultStatusUrl[] = "http://node.lan/msa2/status";

uint32_t parse_color_or_default(const std::string& value, uint32_t fallback) {
  if (value.empty()) {
    return fallback;
  }

  std::string normalized = value;
  if (!normalized.empty() && normalized.front() == '#') {
    normalized.erase(normalized.begin());
  } else if (normalized.size() > 2 && normalized[0] == '0' &&
             (normalized[1] == 'x' || normalized[1] == 'X')) {
    normalized.erase(0, 2);
  }

  if (normalized.size() != 6U) {
    return fallback;
  }

  char* end = nullptr;
  const unsigned long parsed = std::strtoul(normalized.c_str(), &end, 16);
  if (end == nullptr || *end != '\0' || parsed > 0xFFFFFFUL) {
    return fallback;
  }

  return static_cast<uint32_t>(parsed);
}

bool parse_bool_or_default(const std::string& value, bool fallback) {
  if (value.empty()) {
    return fallback;
  }

  std::string normalized = value;
  for (char& ch : normalized) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (normalized == "1" || normalized == "true" || normalized == "on" ||
      normalized == "enabled") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "off" ||
      normalized == "disabled") {
    return false;
  }
  return fallback;
}

}  // namespace

const char* to_string(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
      return "90";
    case DisplayRotation::k180:
      return "180";
    case DisplayRotation::k270:
      return "270";
    case DisplayRotation::k0:
    default:
      return "0";
  }
}

DisplayRotation parse_display_rotation(const std::string& value) {
  if (value == "90") {
    return DisplayRotation::k90;
  }
  if (value == "180") {
    return DisplayRotation::k180;
  }
  if (value == "270") {
    return DisplayRotation::k270;
  }
  return DisplayRotation::k0;
}

esp_err_t ConfigStore::initialize() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "nvs_flash_erase failed");
    err = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(err, kTag, "nvs_flash_init failed");
  return ESP_OK;
}

std::string ConfigStore::load_device_name() const { return kDeviceName; }

WifiCredentials ConfigStore::load_wifi_credentials() const {
  WifiCredentials credentials;
  credentials.ssid = load_string("wifi_ssid");
  credentials.password = load_string("wifi_pass");
  return credentials;
}

std::string ConfigStore::load_status_url() const {
  const std::string url = load_string("status_url");
  return url.empty() ? kDefaultStatusUrl : url;
}

DisplayRotation ConfigStore::load_display_rotation() const {
  return parse_display_rotation(load_string("display_rot"));
}

bool ConfigStore::load_portal_lock_enabled() const {
  return parse_bool_or_default(load_string("portal_lock"), true);
}

ArcColorScheme ConfigStore::load_arc_color_scheme() const {
  ArcColorScheme colors;
  colors.soc_high = parse_color_or_default(load_string("arc_soc_hi"), colors.soc_high);
  colors.soc_mid = parse_color_or_default(load_string("arc_soc_mid"), colors.soc_mid);
  colors.soc_low = parse_color_or_default(load_string("arc_soc_lo"), colors.soc_low);
  colors.charging = parse_color_or_default(load_string("arc_charge"), colors.charging);
  colors.discharging = parse_color_or_default(load_string("arc_dischg"), colors.discharging);
  colors.idle = parse_color_or_default(load_string("arc_idle"), colors.idle);
  colors.offline = parse_color_or_default(load_string("arc_offline"), colors.offline);
  return colors;
}

DisplaySettings ConfigStore::load_display_settings() const {
  DisplaySettings settings;
  const std::string brightness = load_string("disp_bright");
  if (!brightness.empty()) {
    settings.brightness_percent = std::atoi(brightness.c_str());
  }
  const std::string contrast = load_string("disp_contrast");
  if (!contrast.empty()) {
    settings.contrast_percent = std::atoi(contrast.c_str());
  }
  settings.invert = parse_bool_or_default(load_string("disp_invert"), settings.invert);

  const std::string off_sec = load_string("disp_off_sec");
  if (!off_sec.empty()) {
    settings.screen_off_seconds = static_cast<uint32_t>(std::atoi(off_sec.c_str()));
  }
  settings.brightness_percent = std::clamp(settings.brightness_percent, 0, 100);
  settings.contrast_percent = std::clamp(settings.contrast_percent, 0, 100);
  return settings;
}

BatteryDisplayPolicy ConfigStore::load_battery_display_policy() const {
  BatteryDisplayPolicy policy;
  policy.dim_enabled = parse_bool_or_default(load_string("bat_dim_en"), policy.dim_enabled);
  policy.screen_off_enabled =
      parse_bool_or_default(load_string("bat_off_en"), policy.screen_off_enabled);
  policy.usb_power_save_enabled =
      parse_bool_or_default(load_string("bat_usb_ps"), policy.usb_power_save_enabled);

  const std::string dim_pct = load_string("bat_dim_pct");
  if (!dim_pct.empty()) {
    policy.dim_brightness_percent = std::atoi(dim_pct.c_str());
  }
  return policy;
}

std::string ConfigStore::load_timezone_iana() const { return load_string("timezone"); }

esp_err_t ConfigStore::save_wifi_credentials(const WifiCredentials& credentials) const {
  ESP_RETURN_ON_ERROR(save_string("wifi_ssid", credentials.ssid), kTag, "save ssid failed");
  ESP_RETURN_ON_ERROR(save_string("wifi_pass", credentials.password), kTag, "save pass failed");
  return ESP_OK;
}

esp_err_t ConfigStore::save_status_url(const std::string& url) const {
  return save_string("status_url", url);
}

esp_err_t ConfigStore::save_display_rotation(DisplayRotation rotation) const {
  return save_string("display_rot", to_string(rotation));
}

esp_err_t ConfigStore::save_portal_lock_enabled(bool enabled) const {
  return save_string("portal_lock", enabled ? "1" : "0");
}

esp_err_t ConfigStore::save_timezone_iana(const std::string& iana_name) const {
  return save_string("timezone", iana_name);
}

esp_err_t ConfigStore::save_display_settings(const DisplaySettings& settings) const {
  ESP_RETURN_ON_ERROR(save_string("disp_bright", std::to_string(settings.brightness_percent)), kTag,
                      "save disp_bright failed");
  ESP_RETURN_ON_ERROR(save_string("disp_contrast", std::to_string(settings.contrast_percent)), kTag,
                      "save disp_contrast failed");
  ESP_RETURN_ON_ERROR(save_string("disp_invert", settings.invert ? "1" : "0"), kTag,
                      "save disp_invert failed");
  ESP_RETURN_ON_ERROR(save_string("disp_off_sec", std::to_string(settings.screen_off_seconds)),
                      kTag, "save disp_off_sec failed");
  return ESP_OK;
}

esp_err_t ConfigStore::save_arc_color_scheme(const ArcColorScheme& colors) const {
  char buffer[16] = {};
  auto save_color = [&](const char* key, uint32_t color) -> esp_err_t {
    std::snprintf(buffer, sizeof(buffer), "#%06X", static_cast<unsigned int>(color & 0xFFFFFFU));
    return save_string(key, buffer);
  };

  ESP_RETURN_ON_ERROR(save_color("arc_soc_hi", colors.soc_high), kTag, "save soc_hi failed");
  ESP_RETURN_ON_ERROR(save_color("arc_soc_mid", colors.soc_mid), kTag, "save soc_mid failed");
  ESP_RETURN_ON_ERROR(save_color("arc_soc_lo", colors.soc_low), kTag, "save soc_lo failed");
  ESP_RETURN_ON_ERROR(save_color("arc_charge", colors.charging), kTag, "save charge failed");
  ESP_RETURN_ON_ERROR(save_color("arc_dischg", colors.discharging), kTag, "save dischg failed");
  ESP_RETURN_ON_ERROR(save_color("arc_idle", colors.idle), kTag, "save idle failed");
  ESP_RETURN_ON_ERROR(save_color("arc_offline", colors.offline), kTag, "save offline failed");
  return ESP_OK;
}

esp_err_t ConfigStore::save_battery_display_policy(const BatteryDisplayPolicy& policy) const {
  ESP_RETURN_ON_ERROR(save_string("bat_dim_en", policy.dim_enabled ? "1" : "0"), kTag,
                      "save dim_en failed");
  ESP_RETURN_ON_ERROR(save_string("bat_off_en", policy.screen_off_enabled ? "1" : "0"), kTag,
                      "save off_en failed");
  ESP_RETURN_ON_ERROR(save_string("bat_usb_ps", policy.usb_power_save_enabled ? "1" : "0"), kTag,
                      "save usb_ps failed");
  ESP_RETURN_ON_ERROR(save_string("bat_dim_pct", std::to_string(policy.dim_brightness_percent)),
                      kTag, "save dim_pct failed");
  return ESP_OK;
}

esp_err_t ConfigStore::save_string(const char* key, const std::string& value) const {
  nvs_handle_t handle = 0;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag, "nvs_open failed");
  const esp_err_t err = nvs_set_str(handle, key, value.c_str());
  if (err == ESP_OK) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(handle));
  }
  nvs_close(handle);
  return err;
}

std::string ConfigStore::load_string(const char* key) const {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return {};
  }

  size_t required = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &required);
  if (err != ESP_OK || required == 0) {
    nvs_close(handle);
    return {};
  }

  std::vector<char> buffer(required, 0);
  err = nvs_get_str(handle, key, buffer.data(), &required);
  nvs_close(handle);
  if (err != ESP_OK) {
    return {};
  }
  return std::string(buffer.data());
}

}  // namespace printsphere
