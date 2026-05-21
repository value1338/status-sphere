#include "printsphere/msa2_status.hpp"

#include <cstdio>

namespace printsphere {

namespace {

std::string format_number(double value, const char* suffix) {
  char buffer[48] = {};
  const double abs_value = value >= 0.0 ? value : -value;
  if (abs_value >= 1000.0) {
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", value, suffix != nullptr ? suffix : "");
  } else if (abs_value >= 100.0) {
    std::snprintf(buffer, sizeof(buffer), "%.0f%s", value, suffix != nullptr ? suffix : "");
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", value, suffix != nullptr ? suffix : "");
  }
  return buffer;
}

}  // namespace

std::optional<double> Msa2Snapshot::number_field(const char* key) const {
  if (key == nullptr) {
    return std::nullopt;
  }
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.is_null || !it->second.has_number) {
    return std::nullopt;
  }
  return it->second.number_value;
}

std::optional<std::string> Msa2Snapshot::string_field(const char* key) const {
  if (key == nullptr) {
    return std::nullopt;
  }
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.is_null || !it->second.has_string) {
    return std::nullopt;
  }
  return it->second.string_value;
}

std::optional<bool> Msa2Snapshot::bool_field(const char* key) const {
  if (key == nullptr) {
    return std::nullopt;
  }
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.is_null) {
    return std::nullopt;
  }
  if (it->second.is_bool) {
    return it->second.bool_value;
  }
  if (it->second.has_string) {
    const std::string& value = it->second.string_value;
    if (value == "true" || value == "1") {
      return true;
    }
    if (value == "false" || value == "0") {
      return false;
    }
  }
  return std::nullopt;
}

std::string Msa2Snapshot::format_field(const char* key, const char* suffix) const {
  if (key == nullptr) {
    return "--";
  }
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.is_null) {
    return "--";
  }
  if (it->second.has_string) {
    return it->second.string_value;
  }
  if (it->second.is_bool) {
    return it->second.bool_value ? "true" : "false";
  }
  if (it->second.has_number) {
    return format_number(it->second.number_value, suffix);
  }
  return "--";
}

}  // namespace printsphere
