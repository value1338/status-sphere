#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "lvgl.h"
#include "printsphere/config_store.hpp"
#include "printsphere/layout_renderer.hpp"
#include "printsphere/msa2_status.hpp"

namespace printsphere {

enum class ScreenPowerMode : uint8_t {
  kAwake,
  kDimmed,
  kOff,
};

class Ui {
 public:
  static constexpr int kPageOverview = 0;
  static constexpr int kPageBattery = 1;
  static constexpr int kPageGrid = 2;
  static constexpr int kPageSensors = 3;
  static constexpr int kPageSystem = 4;
  static constexpr int kPageCount = 5;
  static constexpr int kPageLast = kPageCount - 1;

  void set_display_rotation(DisplayRotation rotation);
  esp_err_t initialize();
  void set_arc_color_scheme(const ArcColorScheme& colors);
  void set_display_settings(const DisplaySettings& settings);
  void set_battery_display_policy(const BatteryDisplayPolicy& policy);
  void apply_snapshot(const Msa2Snapshot& snapshot);
  void update_power_save(bool on_battery, bool keep_awake);
  bool is_low_power_mode_active() const;
  ScreenPowerMode screen_power_mode() const { return screen_power_mode_; }
  bool is_page_transition_active() const { return scrolling_; }
  void set_portal_access_state(bool lock_enabled, bool request_authorized, bool session_active,
                               bool pin_active, const std::string& pin_code,
                               uint32_t pin_remaining_s, uint32_t session_remaining_s);
  bool consume_portal_unlock_request();
  bool consume_display_settings_change(DisplaySettings& out);
  bool consume_wifi_reset_request();
  void request_wake_display();
  int active_page_count() const;

 private:
  esp_err_t build_dashboard();
  void apply_snapshot_locked(const Msa2Snapshot& snapshot, bool force_ring_refresh = false);
  void apply_ring_visual_locked(const Msa2Snapshot& snapshot);
  void apply_page_visibility();
  void note_activity(bool wake_display);
  void wake_display();
  void apply_brightness_policy();
  void set_pager_scroll_locked(bool locked);
  void set_active_page(int page);
  int clamp_page(int page) const;
  int nearest_page_for_scroll() const;
  lv_obj_t* page_object(int page) const;
  void set_brightness_percent(int brightness_percent);
  static void pager_event_cb(lv_event_t* event);
  static void screen_event_cb(lv_event_t* event);
  static void soc_arc_guard_cb(lv_event_t* event);
  void handle_pager_event(lv_event_t* event);
  void handle_screen_event(lv_event_t* event);

  bool initialized_ = false;
  lv_display_t* display_ = nullptr;
  lv_obj_t* screen_ = nullptr;
  lv_obj_t* pager_ = nullptr;
  lv_obj_t* pages_[kPageCount] = {};
  lv_obj_t* status_arc_ = nullptr;
  lv_obj_t* arc_value_label_ = nullptr;
  lv_obj_t* arc_title_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* battery_icon_label_ = nullptr;
  lv_obj_t* battery_pct_label_ = nullptr;
  lv_obj_t* brightness_overlay_ = nullptr;
  lv_obj_t* page_titles_[kPageCount] = {};
  lv_obj_t* metric_labels_[kPageCount][6] = {};
  lv_obj_t* metric_values_[kPageCount][6] = {};
  lv_obj_t* sensor_cards_[4] = {};
  lv_obj_t* sensor_titles_[4] = {};
  lv_obj_t* sensor_lines_[4][3] = {};

  ArcColorScheme arc_colors_{};
  DisplaySettings display_settings_{};
  BatteryDisplayPolicy battery_policy_{};
  bool display_invert_applied_ = false;
  DisplayRotation display_rotation_ = DisplayRotation::k0;
  Msa2Snapshot last_snapshot_{};
  int display_arc_value_ = 0;
  int active_page_ = kPageOverview;
  bool scrolling_ = false;
  bool pager_scroll_locked_ = false;
  int user_brightness_percent_ = -1;
  int applied_brightness_percent_ = -1;
  ScreenPowerMode screen_power_mode_ = ScreenPowerMode::kAwake;
  std::atomic<uint32_t> last_activity_tick_ms_{0};
  std::atomic<bool> portal_unlock_requested_{false};
  bool gesture_active_ = false;
  bool swipe_switched_ = false;
  bool overlay_visible_ = false;
  int gesture_start_x_ = 0;
  int gesture_start_y_ = 0;
  int gesture_start_brightness_ = 0;
  bool using_dynamic_layout_ = false;
  bool display_settings_dirty_ = false;
  LayoutRenderer layout_renderer_;
};

}  // namespace printsphere
