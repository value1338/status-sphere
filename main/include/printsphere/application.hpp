#pragma once

#include "printsphere/config_store.hpp"
#include "printsphere/msa2_status_client.hpp"
#include "printsphere/pmu.hpp"
#include "printsphere/setup_portal.hpp"
#include "printsphere/ui.hpp"
#include "printsphere/wifi_manager.hpp"

namespace printsphere {

class Application {
 public:
  Application();
  void run();

 private:
  ConfigStore config_store_{};
  WifiManager wifi_manager_{};
  Msa2StatusClient status_client_{};
  Ui ui_{};
  SetupPortal setup_portal_;
  PmuManager pmu_manager_{};
};

}  // namespace printsphere
