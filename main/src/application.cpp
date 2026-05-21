#include "printsphere/application.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "printsphere/time_sync.hpp"

namespace printsphere {

namespace {

constexpr char kTag[] = "status.app";
constexpr TickType_t kScreenOffTouchWakePollSlice = pdMS_TO_TICKS(25);
constexpr TickType_t kUiCommandWakePollSlice = pdMS_TO_TICKS(50);

esp_err_t configure_power_management() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t pm_config = {};
  pm_config.max_freq_mhz = 240;
  pm_config.min_freq_mhz = 80;
  pm_config.light_sleep_enable = false;
  ESP_RETURN_ON_ERROR(esp_pm_configure(&pm_config), kTag, "esp_pm_configure failed");
#else
  ESP_LOGI(kTag, "Power management disabled in sdkconfig");
#endif
  return ESP_OK;
}

void wait_for_next_iteration(Ui& ui, TickType_t delay) {
  TickType_t remaining = delay;
  while (remaining > 0) {
    const bool touch_wake_poll_active = ui.screen_power_mode() == ScreenPowerMode::kOff;
    TickType_t slice = remaining;
    if (touch_wake_poll_active && slice > kScreenOffTouchWakePollSlice) {
      slice = kScreenOffTouchWakePollSlice;
    } else if (slice > kUiCommandWakePollSlice) {
      slice = kUiCommandWakePollSlice;
    }
    vTaskDelay(slice);
    remaining -= slice;

    if (touch_wake_poll_active && gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
      ui.request_wake_display();
      break;
    }
  }
}

}  // namespace

Application::Application()
    : setup_portal_(config_store_, wifi_manager_, status_client_, ui_, pmu_manager_) {}

void Application::run() {
  esp_log_level_set("mbedtls", ESP_LOG_WARN);
  ESP_LOGI(kTag, "Bootstrapping Status Sphere");

  ESP_ERROR_CHECK(config_store_.initialize());
  time_sync::set_timezone_iana(config_store_.load_timezone_iana());
  ESP_ERROR_CHECK(configure_power_management());
  ESP_ERROR_CHECK(wifi_manager_.initialize_network_stack());
  ESP_ERROR_CHECK(wifi_manager_.start_setup_access_point(config_store_.load_device_name()));

  const WifiCredentials wifi_credentials = config_store_.load_wifi_credentials();
  if (wifi_credentials.is_configured()) {
    const esp_err_t wifi_err = wifi_manager_.connect_station(wifi_credentials);
    if (wifi_err != ESP_OK) {
      ESP_LOGW(kTag, "Stored Wi-Fi connect failed: %s", esp_err_to_name(wifi_err));
    }
  }

  ESP_ERROR_CHECK(setup_portal_.start());
  ESP_ERROR_CHECK(pmu_manager_.initialize());
  ui_.set_arc_color_scheme(config_store_.load_arc_color_scheme());
  ui_.set_display_rotation(config_store_.load_display_rotation());
  ui_.set_battery_display_policy(config_store_.load_battery_display_policy());
  ui_.set_display_settings(config_store_.load_display_settings());
  ESP_ERROR_CHECK(ui_.initialize());

  status_client_.configure(config_store_.load_status_url());
  ESP_ERROR_CHECK(status_client_.start());

  ESP_LOGI(kTag, "Bootstrap complete");

  while (true) {
    if (ui_.consume_portal_unlock_request()) {
      setup_portal_.request_unlock_pin();
    }

    DisplaySettings changed_settings{};
    if (ui_.consume_display_settings_change(changed_settings)) {
      config_store_.save_display_settings(changed_settings);
      ESP_LOGI(kTag, "Display settings saved (bright=%d contrast=%d invert=%d off=%lu)",
               changed_settings.brightness_percent, changed_settings.contrast_percent,
               changed_settings.invert ? 1 : 0,
               static_cast<unsigned long>(changed_settings.screen_off_seconds));
    }

    if (ui_.consume_wifi_reset_request()) {
      ESP_LOGI(kTag, "WiFi reset requested from UI");
      wifi_manager_.disconnect_and_forget();
      config_store_.save_wifi_credentials({});
    }

    const PortalAccessSnapshot portal_access = setup_portal_.access_snapshot();
    const bool wifi_connected = wifi_manager_.is_station_connected();
    const bool page_transition_active = ui_.is_page_transition_active();

    status_client_.set_network_ready(wifi_connected);
    status_client_.set_low_power_mode(ui_.is_low_power_mode_active());

    Msa2Snapshot snapshot = status_client_.snapshot();
    snapshot.wifi_connected = wifi_connected;
    snapshot.wifi_ip = wifi_manager_.station_ip();
    snapshot.setup_ap_active = wifi_manager_.is_setup_access_point_active();
    snapshot.setup_ap_ssid = wifi_manager_.setup_access_point_ssid();
    snapshot.setup_ap_password = wifi_manager_.setup_access_point_password();
    snapshot.setup_ap_ip = wifi_manager_.setup_access_point_ip();

    const PowerSnapshot power = pmu_manager_.sample();
    if (power.available) {
      snapshot.device_battery_percent = power.battery_percent;
      snapshot.device_battery_present = power.battery_present;
      snapshot.device_charging = power.charging;
      snapshot.device_usb_present = power.usb_present;
    }

    ui_.set_portal_access_state(portal_access.lock_enabled, portal_access.request_authorized,
                                portal_access.session_active, portal_access.pin_active,
                                portal_access.pin_code, portal_access.pin_remaining_s,
                                portal_access.session_remaining_s);
    ui_.apply_snapshot(snapshot);

    const bool on_battery = power.available && power.battery_present && !power.usb_present;
    const bool keep_screen_awake = page_transition_active;
    ui_.update_power_save(on_battery, keep_screen_awake);

    const TickType_t loop_delay =
        page_transition_active || !ui_.is_low_power_mode_active() ? pdMS_TO_TICKS(500)
                                                                  : pdMS_TO_TICKS(1500);
    wait_for_next_iteration(ui_, loop_delay);
  }
}

}  // namespace printsphere
