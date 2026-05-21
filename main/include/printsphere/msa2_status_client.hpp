#pragma once

#include <mutex>
#include <string>

#include "esp_err.h"
#include "printsphere/config_store.hpp"
#include "printsphere/msa2_status.hpp"

namespace printsphere {

class Msa2StatusClient {
 public:
  esp_err_t start();
  void configure(const std::string& status_url);
  void set_network_ready(bool ready);
  void set_low_power_mode(bool enabled);
  Msa2Snapshot snapshot() const;
  Msa2Snapshot refreshed_snapshot();

 private:
  static void task_entry(void* arg);
  void task_loop();
  bool fetch_once(Msa2Snapshot* out);

  mutable std::mutex mutex_{};
  Msa2Snapshot snapshot_{};
  std::string status_url_ = "http://node.lan/msa2/status";
  bool network_ready_ = false;
  bool low_power_mode_ = false;
  bool task_running_ = false;
};

}  // namespace printsphere
