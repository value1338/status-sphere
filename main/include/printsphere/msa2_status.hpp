#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace printsphere {

struct StatusValue {
  bool is_null = true;
  bool is_bool = false;
  bool bool_value = false;
  bool has_number = false;
  double number_value = 0.0;
  bool has_string = false;
  std::string string_value;
  std::string desc;
};

struct Msa2Snapshot {
  bool connected = false;
  bool fetch_in_progress = false;
  std::string detail = "Waiting for data";
  std::string raw_json;
  uint64_t updated_ms = 0;

  bool wifi_connected = false;
  std::string wifi_ip;
  bool setup_ap_active = false;
  std::string setup_ap_ssid;
  std::string setup_ap_password;
  std::string setup_ap_ip;

  uint8_t device_battery_percent = 0;
  bool device_battery_present = false;
  bool device_charging = false;
  bool device_usb_present = false;

  std::map<std::string, StatusValue> fields;

  std::optional<double> number_field(const char* key) const;
  std::optional<std::string> string_field(const char* key) const;
  std::optional<bool> bool_field(const char* key) const;
  std::string format_field(const char* key, const char* suffix = "") const;
};

}  // namespace printsphere
