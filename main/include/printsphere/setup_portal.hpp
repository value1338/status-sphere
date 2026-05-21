#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"
#include "printsphere/config_store.hpp"
#include "printsphere/msa2_status_client.hpp"
#include "printsphere/pmu.hpp"
#include "printsphere/wifi_manager.hpp"

namespace printsphere {

class Ui;

struct PortalAccessSnapshot {
  bool lock_enabled = false;
  bool request_authorized = true;
  bool session_active = false;
  bool pin_active = false;
  uint32_t session_remaining_s = 0;
  uint32_t pin_remaining_s = 0;
  std::string pin_code;
  std::string detail;
};

class SetupPortal {
 public:
  SetupPortal(ConfigStore& config_store, WifiManager& wifi_manager,
              Msa2StatusClient& status_client, Ui& ui, const PmuManager& pmu_manager)
      : config_store_(config_store),
        wifi_manager_(wifi_manager),
        status_client_(status_client),
        ui_(ui),
        pmu_manager_(pmu_manager) {}

  esp_err_t start();
  void request_unlock_pin();
  PortalAccessSnapshot access_snapshot(bool request_authorized = true);

 private:
  static esp_err_t handle_root(httpd_req_t* request);
  static esp_err_t handle_health(httpd_req_t* request);
  static esp_err_t handle_config_get(httpd_req_t* request);
  static esp_err_t handle_config_post(httpd_req_t* request);
  static esp_err_t handle_wifi_scan(httpd_req_t* request);
  static esp_err_t handle_arc_colors_post(httpd_req_t* request);
  static esp_err_t handle_display_settings_get(httpd_req_t* request);
  static esp_err_t handle_display_settings_post(httpd_req_t* request);

  static SetupPortal* instance();

  ConfigStore& config_store_;
  WifiManager& wifi_manager_;
  Msa2StatusClient& status_client_;
  Ui& ui_;
  const PmuManager& pmu_manager_;
  httpd_handle_t server_ = nullptr;
  inline static SetupPortal* instance_ = nullptr;
};

}  // namespace printsphere
