#include "printsphere/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "printsphere/board_config.hpp"

extern "C" {
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
extern const lv_font_t dosis_40;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t mdi_30;
extern const lv_font_t mdi_40;
}

namespace printsphere {

namespace {

constexpr char kTag[] = "status.ui";
constexpr int kDefaultBrightnessPercent = 80;
constexpr int kRingStrokeWidth = 22;
constexpr int kOverviewTitleY = 48;
constexpr int kOverviewArcSize = 440;
constexpr int kOverviewArcCenterY = 0;
constexpr int kOverviewSocValueY = -20;
constexpr int kOverviewSocLabelY = 16;
constexpr int kOverviewDetailY = 52;
constexpr int kOverviewMetricStartY = 290;
constexpr int kOverviewMetricRowStep = 34;
constexpr int kSwipeThresholdPx = 24;
constexpr int kGestureAxisLockMarginPx = 16;
constexpr int kBrightnessHorizontalTolerancePx = 18;
constexpr int kManualMinBrightnessPercent = 4;
constexpr uint32_t kRingBaseDark = 0x101010;
constexpr char kDegreeC[] = "\xC2\xB0""C";
constexpr char kMdiBattery100[] = "\xF3\xB0\x81\xB9";
constexpr char kMdiBatteryCharging100[] = "\xF3\xB0\xA0\x87";

uint32_t adjust_color_contrast(uint32_t rgb, int contrast_percent) {
  if (contrast_percent == 50) {
    return rgb;
  }
  const float factor = 1.0f + (static_cast<float>(contrast_percent) - 50.0f) / 50.0f;
  auto channel = [&](int value) {
    const int adjusted = 128 + static_cast<int>((value - 128) * factor);
    return std::clamp(adjusted, 0, 255);
  };
  const int r = channel(static_cast<int>((rgb >> 16) & 0xFF));
  const int g = channel(static_cast<int>((rgb >> 8) & 0xFF));
  const int b = channel(static_cast<int>(rgb & 0xFF));
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

class LvglLockGuard {
 public:
  explicit LvglLockGuard(uint32_t timeout_ms) : locked_(bsp_display_lock(timeout_ms) == ESP_OK) {}
  ~LvglLockGuard() {
    if (locked_) {
      bsp_display_unlock();
    }
  }
  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};

bsp_display_rotation_t bsp_rotation_for(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
      return BSP_DISPLAY_ROTATE_90;
    case DisplayRotation::k180:
      return BSP_DISPLAY_ROTATE_180;
    case DisplayRotation::k270:
      return BSP_DISPLAY_ROTATE_270;
    case DisplayRotation::k0:
    default:
      return BSP_DISPLAY_ROTATE_0;
  }
}

void apply_touch_rotation_flags(DisplayRotation rotation, bsp_display_cfg_t* cfg) {
  if (cfg == nullptr) {
    return;
  }
  switch (rotation) {
    case DisplayRotation::k90:
      cfg->touch_flags.swap_xy = 1;
      cfg->touch_flags.mirror_x = 0;
      cfg->touch_flags.mirror_y = 1;
      break;
    case DisplayRotation::k180:
      cfg->touch_flags.swap_xy = 0;
      cfg->touch_flags.mirror_x = 0;
      cfg->touch_flags.mirror_y = 0;
      break;
    case DisplayRotation::k270:
      cfg->touch_flags.swap_xy = 1;
      cfg->touch_flags.mirror_x = 1;
      cfg->touch_flags.mirror_y = 0;
      break;
    case DisplayRotation::k0:
    default:
      cfg->touch_flags.swap_xy = 0;
      cfg->touch_flags.mirror_x = 1;
      cfg->touch_flags.mirror_y = 1;
      break;
  }
}

void make_transparent(lv_obj_t* obj) {
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
}

void enable_touch_bubble(lv_obj_t* obj) {
  if (obj == nullptr) {
    return;
  }
  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void make_input_pass_through(lv_obj_t* obj) {
  if (obj == nullptr) {
    return;
  }
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  enable_touch_bubble(obj);
}

void set_label_text_if_changed(lv_obj_t* label, const char* text) {
  if (label == nullptr || text == nullptr) {
    return;
  }
  const char* current = lv_label_get_text(label);
  if (current != nullptr && std::strcmp(current, text) == 0) {
    return;
  }
  lv_label_set_text(label, text);
}

void set_label_text_if_changed(lv_obj_t* label, const std::string& text) {
  set_label_text_if_changed(label, text.c_str());
}

void create_metric_row(lv_obj_t* page, int row, int y, const char* label_text,
                       lv_obj_t** label_out, lv_obj_t** value_out) {
  const lv_font_t* info20 = &lv_font_montserrat_20;
  const lv_font_t* dosis32 = &dosis_32;

  lv_obj_t* label = lv_label_create(page);
  set_label_text_if_changed(label, label_text);
  lv_obj_set_style_text_font(label, info20, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, -90, y);
  make_input_pass_through(label);

  lv_obj_t* value = lv_label_create(page);
  set_label_text_if_changed(value, "--");
  lv_obj_set_style_text_font(value, dosis32, 0);
  lv_obj_set_style_text_color(value, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_width(value, 160);
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(value, LV_ALIGN_TOP_MID, 90, y - 2);
  make_input_pass_through(value);

  if (label_out != nullptr) {
    *label_out = label;
  }
  if (value_out != nullptr) {
    *value_out = value;
  }
}

uint32_t soc_color(const ArcColorScheme& colors, double soc, bool connected) {
  if (!connected) {
    return colors.offline;
  }
  if (soc >= 60.0) {
    return colors.soc_high;
  }
  if (soc >= 25.0) {
    return colors.soc_mid;
  }
  return colors.soc_low;
}

const char* battery_status_icon(const Msa2Snapshot& snapshot) {
  if (snapshot.device_charging) {
    return kMdiBatteryCharging100;
  }
  return kMdiBattery100;
}

}  // namespace

void Ui::set_display_rotation(DisplayRotation rotation) {
  display_rotation_ = rotation;
}

esp_err_t Ui::initialize() {
  if (initialized_) {
    return ESP_OK;
  }

  bsp_display_cfg_t display_cfg = {
      .lv_adapter_cfg = []() {
        esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
        cfg.task_core_id = 1;
        return cfg;
      }(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0,
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC,
      .touch_flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
  };
  apply_touch_rotation_flags(display_rotation_, &display_cfg);

  display_ = bsp_display_start_with_config(&display_cfg);
  if (display_ == nullptr) {
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(bsp_display_rotation_set(bsp_rotation_for(display_rotation_)), kTag,
                      "apply display rotation failed");

  user_brightness_percent_ = -1;
  applied_brightness_percent_ = -1;
  screen_power_mode_ = ScreenPowerMode::kAwake;
  last_activity_tick_ms_.store(lv_tick_get());
  set_brightness_percent(display_settings_.brightness_percent > 0
                           ? display_settings_.brightness_percent
                           : kDefaultBrightnessPercent);
  ESP_RETURN_ON_ERROR(build_dashboard(), kTag, "build_dashboard failed");
  display_invert_applied_ = display_settings_.invert;
  if (display_settings_.invert) {
    bsp_display_invert_color(true);
  }
  set_display_settings(display_settings_);

  lv_indev_t* indev = lv_indev_get_next(nullptr);
  while (indev != nullptr) {
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
      lv_indev_set_scroll_throw(indev, 90);
      break;
    }
    indev = lv_indev_get_next(indev);
  }

  initialized_ = true;
  ESP_LOGI(kTag, "UI ready with %d pages", active_page_count());
  return ESP_OK;
}

void Ui::set_arc_color_scheme(const ArcColorScheme& colors) {
  arc_colors_ = colors;
  if (initialized_) {
    LvglLockGuard lock(200);
    if (lock.locked()) {
      apply_ring_visual_locked(last_snapshot_);
    }
  }
}

void Ui::set_battery_display_policy(const BatteryDisplayPolicy& policy) {
  battery_policy_ = policy;
  if (display_settings_.screen_off_seconds > 0) {
    battery_policy_.screen_off_enabled = true;
    battery_policy_.off_timeout_idle_s = display_settings_.screen_off_seconds;
    battery_policy_.off_timeout_active_s = display_settings_.screen_off_seconds;
  }
}

void Ui::set_display_settings(const DisplaySettings& settings) {
  display_settings_ = settings;
  display_settings_.brightness_percent =
      std::clamp(display_settings_.brightness_percent, 0, 100);
  display_settings_.contrast_percent =
      std::clamp(display_settings_.contrast_percent, 0, 100);

  if (display_settings_.screen_off_seconds > 0) {
    battery_policy_.screen_off_enabled = true;
    battery_policy_.off_timeout_idle_s = display_settings_.screen_off_seconds;
    battery_policy_.off_timeout_active_s = display_settings_.screen_off_seconds;
  } else {
    battery_policy_.screen_off_enabled = false;
  }

  if (!initialized_) {
    return;
  }

  LvglLockGuard lock(200);
  if (!lock.locked()) {
    return;
  }

  set_brightness_percent(display_settings_.brightness_percent);
  if (display_invert_applied_ != display_settings_.invert) {
    bsp_display_invert_color(display_settings_.invert);
    display_invert_applied_ = display_settings_.invert;
  }

  const int contrast = display_settings_.contrast_percent;
  auto apply_text = [&](lv_obj_t* obj, uint32_t base_rgb) {
    if (obj != nullptr) {
      lv_obj_set_style_text_color(obj, lv_color_hex(adjust_color_contrast(base_rgb, contrast)),
                                  0);
    }
  };

  if (!using_dynamic_layout_) {
    for (int i = 0; i < kPageCount; ++i) {
      apply_text(page_titles_[i], 0xFFFFFF);
      for (int row = 0; row < 6; ++row) {
        apply_text(metric_labels_[i][row], 0x94A3B8);
        apply_text(metric_values_[i][row], 0xFFFFFF);
      }
    }
    apply_text(arc_value_label_, 0xFFFFFF);
    apply_text(arc_title_label_, 0x94A3B8);
    apply_text(detail_label_, 0x94A3B8);
    apply_text(battery_icon_label_, 0xFFFFFF);
    apply_text(battery_pct_label_, 0xFFFFFF);
    for (int s = 0; s < 4; ++s) {
      apply_text(sensor_titles_[s], 0xFFFFFF);
      for (int line = 0; line < 3; ++line) {
        apply_text(sensor_lines_[s][line], 0x94A3B8);
      }
    }
  }
  apply_ring_visual_locked(last_snapshot_);
}

bool Ui::consume_portal_unlock_request() {
  return portal_unlock_requested_.exchange(false);
}

bool Ui::consume_display_settings_change(DisplaySettings& out) {
  const bool from_tap =
      using_dynamic_layout_ && layout_renderer_.consume_settings_changed();
  if (!from_tap && !display_settings_dirty_) {
    return false;
  }
  display_settings_dirty_ = false;
  out = display_settings_;
  if (initialized_) {
    LvglLockGuard lock(200);
    if (lock.locked()) {
      set_display_settings(out);
    }
  }
  return true;
}

bool Ui::consume_wifi_reset_request() {
  if (!using_dynamic_layout_) return false;
  return layout_renderer_.consume_wifi_reset();
}

void Ui::set_portal_access_state(bool, bool, bool, bool, const std::string&, uint32_t, uint32_t) {}

void Ui::apply_snapshot(const Msa2Snapshot& snapshot) {
  if (!initialized_) {
    return;
  }

  LvglLockGuard lock(300);
  if (!lock.locked()) {
    return;
  }
  apply_snapshot_locked(snapshot, false);
}

int Ui::active_page_count() const {
  return using_dynamic_layout_ ? layout_renderer_.page_count() : kPageCount;
}

esp_err_t Ui::build_dashboard() {
  LvglLockGuard lock(3000);
  if (!lock.locked()) {
    return ESP_ERR_TIMEOUT;
  }

  const lv_font_t* dosis20 = &dosis_20;
  const lv_font_t* dosis32 = &dosis_32;
  const lv_font_t* dosis40 = &dosis_40;
  const lv_font_t* info20 = &lv_font_montserrat_20;
  const lv_font_t* mdi30 = &mdi_30;

  screen_ = lv_screen_active();
  lv_obj_set_style_bg_color(screen_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(screen_, &Ui::screen_event_cb, LV_EVENT_ALL, this);

  pager_ = lv_obj_create(screen_);
  lv_obj_set_size(pager_, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_center(pager_);
  make_transparent(pager_);
  lv_obj_set_flex_flow(pager_, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(pager_, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(pager_, LV_SCROLL_SNAP_NONE);
  lv_obj_set_scrollbar_mode(pager_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(pager_, &Ui::pager_event_cb, LV_EVENT_ALL, this);
  enable_touch_bubble(pager_);

#ifdef HAS_EDITOR_LAYOUT
  do {
    if (!layout_renderer_.load_from_embedded() || !layout_renderer_.has_layout()) {
      ESP_LOGW(kTag, "Editor layout not usable, falling back to hardcoded");
      break;
    }
    layout_renderer_.build_pages(pager_, &display_settings_);
    lv_obj_set_size(pager_, layout_renderer_.layout_width(), layout_renderer_.layout_height());
    lv_obj_center(pager_);

    bool any_page = false;
    for (int i = 0; i < layout_renderer_.page_count(); ++i) {
      if (layout_renderer_.page(i).page_obj != nullptr) { any_page = true; break; }
    }
    if (!any_page) {
      ESP_LOGW(kTag, "Dynamic pages have no LVGL objects, falling back to hardcoded");
      break;
    }

    using_dynamic_layout_ = true;
    status_arc_ = nullptr;

    brightness_overlay_ = lv_label_create(lv_layer_top());
    set_label_text_if_changed(brightness_overlay_, "80%");
    lv_obj_set_style_text_font(brightness_overlay_, dosis40, 0);
    lv_obj_set_style_text_color(brightness_overlay_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(brightness_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(brightness_overlay_);

    active_page_ = 0;
    apply_page_visibility();
    ESP_LOGI(kTag, "Dynamic layout loaded with %d pages", layout_renderer_.page_count());
    return ESP_OK;
  } while (false);
#endif

  auto create_page = [](lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, board::kDisplayWidth, board::kDisplayHeight);
    make_transparent(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    enable_touch_bubble(page);
    return page;
  };

  for (int i = 0; i < kPageCount; ++i) {
    pages_[i] = create_page(pager_);
    page_titles_[i] = lv_label_create(pages_[i]);
    lv_obj_set_style_text_font(page_titles_[i], dosis32, 0);
    lv_obj_set_style_text_color(page_titles_[i], lv_color_hex(0xFFFFFF), 0);
    const int title_y = (i == kPageOverview) ? kOverviewTitleY : 48;
    lv_obj_align(page_titles_[i], LV_ALIGN_TOP_MID, 0, title_y);
    make_input_pass_through(page_titles_[i]);
  }

  set_label_text_if_changed(page_titles_[kPageOverview], "Energie");
  set_label_text_if_changed(page_titles_[kPageBattery], "Batterie");
  set_label_text_if_changed(page_titles_[kPageGrid], "Netz");
  set_label_text_if_changed(page_titles_[kPageSensors], "Sensoren");
  set_label_text_if_changed(page_titles_[kPageSystem], "System");

  status_arc_ = lv_arc_create(pages_[kPageOverview]);
  lv_obj_set_size(status_arc_, kOverviewArcSize, kOverviewArcSize);
  lv_obj_align(status_arc_, LV_ALIGN_CENTER, 0, kOverviewArcCenterY);
  lv_arc_set_rotation(status_arc_, 135);
  lv_arc_set_bg_angles(status_arc_, 0, 270);
  lv_arc_set_range(status_arc_, 0, 100);
  lv_arc_set_value(status_arc_, 0);
  lv_obj_set_style_arc_width(status_arc_, kRingStrokeWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_width(status_arc_, kRingStrokeWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(status_arc_, lv_color_hex(kRingBaseDark), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(status_arc_, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(status_arc_, true, LV_PART_INDICATOR);
  lv_obj_set_style_opa(status_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(status_arc_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(status_arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(status_arc_, 0);
  enable_touch_bubble(status_arc_);
  lv_obj_add_event_cb(status_arc_, &Ui::soc_arc_guard_cb, LV_EVENT_ALL, this);

  arc_value_label_ = lv_label_create(pages_[kPageOverview]);
  set_label_text_if_changed(arc_value_label_, "--%");
  lv_obj_set_style_text_font(arc_value_label_, dosis40, 0);
  lv_obj_set_style_text_color(arc_value_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(arc_value_label_, LV_ALIGN_CENTER, 0, kOverviewSocValueY);
  make_input_pass_through(arc_value_label_);

  arc_title_label_ = lv_label_create(pages_[kPageOverview]);
  set_label_text_if_changed(arc_title_label_, "SoC");
  lv_obj_set_style_text_font(arc_title_label_, info20, 0);
  lv_obj_set_style_text_color(arc_title_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(arc_title_label_, LV_ALIGN_CENTER, 0, kOverviewSocLabelY);
  make_input_pass_through(arc_title_label_);

  battery_icon_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(battery_icon_label_, kMdiBattery100);
  lv_obj_set_style_text_font(battery_icon_label_, mdi30, 0);
  lv_obj_set_style_text_color(battery_icon_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(battery_icon_label_, LV_ALIGN_TOP_RIGHT, -56, 28);
  lv_obj_move_foreground(battery_icon_label_);
  make_input_pass_through(battery_icon_label_);

  battery_pct_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(battery_pct_label_, "--%");
  lv_obj_set_style_text_font(battery_pct_label_, dosis20, 0);
  lv_obj_set_style_text_color(battery_pct_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(battery_pct_label_, LV_ALIGN_TOP_RIGHT, -16, 30);
  lv_obj_move_foreground(battery_pct_label_);
  make_input_pass_through(battery_pct_label_);

  detail_label_ = lv_label_create(pages_[kPageOverview]);
  set_label_text_if_changed(detail_label_, "Waiting for data");
  lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_label_, 320);
  lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(detail_label_, info20, 0);
  lv_obj_set_style_text_color(detail_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(detail_label_, LV_ALIGN_CENTER, 0, kOverviewDetailY);
  make_input_pass_through(detail_label_);

  create_metric_row(pages_[kPageOverview], 0, kOverviewMetricStartY, "PV",
                    &metric_labels_[kPageOverview][0], &metric_values_[kPageOverview][0]);
  create_metric_row(pages_[kPageOverview], 1, kOverviewMetricStartY + kOverviewMetricRowStep,
                    "Verbrauch", &metric_labels_[kPageOverview][1],
                    &metric_values_[kPageOverview][1]);
  create_metric_row(pages_[kPageOverview], 2,
                    kOverviewMetricStartY + 2 * kOverviewMetricRowStep, "Netz",
                    &metric_labels_[kPageOverview][2], &metric_values_[kPageOverview][2]);
  create_metric_row(pages_[kPageOverview], 3,
                    kOverviewMetricStartY + 3 * kOverviewMetricRowStep, "Batterie",
                    &metric_labels_[kPageOverview][3], &metric_values_[kPageOverview][3]);

  const char* battery_labels[] = {"SoC", "Leistung", "Spannung", "Strom", "Temperatur", "Status"};
  for (int i = 0; i < 6; ++i) {
    create_metric_row(pages_[kPageBattery], i, 110 + i * 44, battery_labels[i],
                      &metric_labels_[kPageBattery][i], &metric_values_[kPageBattery][i]);
  }

  const char* grid_labels[] = {"Netzleistung", "Spannung", "Strom", "Frequenz", "RSSI", "Grid P"};
  for (int i = 0; i < 6; ++i) {
    create_metric_row(pages_[kPageGrid], i, 110 + i * 44, grid_labels[i],
                      &metric_labels_[kPageGrid][i], &metric_values_[kPageGrid][i]);
  }

  for (int s = 0; s < 4; ++s) {
    sensor_cards_[s] = lv_obj_create(pages_[kPageSensors]);
    lv_obj_set_size(sensor_cards_[s], 360, 78);
    lv_obj_align(sensor_cards_[s], LV_ALIGN_TOP_MID, 0, 96 + s * 86);
    lv_obj_set_style_radius(sensor_cards_[s], 18, 0);
    lv_obj_set_style_bg_color(sensor_cards_[s], lv_color_hex(0x141B24), 0);
    lv_obj_set_style_bg_opa(sensor_cards_[s], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sensor_cards_[s], lv_color_hex(0x243041), 0);
    lv_obj_set_style_border_width(sensor_cards_[s], 1, 0);
    lv_obj_set_style_pad_all(sensor_cards_[s], 10, 0);
    lv_obj_clear_flag(sensor_cards_[s], LV_OBJ_FLAG_SCROLLABLE);
    make_input_pass_through(sensor_cards_[s]);

    sensor_titles_[s] = lv_label_create(sensor_cards_[s]);
    lv_obj_set_style_text_font(sensor_titles_[s], dosis20, 0);
    lv_obj_set_style_text_color(sensor_titles_[s], lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(sensor_titles_[s], LV_ALIGN_TOP_LEFT, 4, 0);

    for (int line = 0; line < 3; ++line) {
      sensor_lines_[s][line] = lv_label_create(sensor_cards_[s]);
      lv_obj_set_style_text_font(sensor_lines_[s][line], info20, 0);
      lv_obj_set_style_text_color(sensor_lines_[s][line], lv_color_hex(0x94A3B8), 0);
      lv_obj_align(sensor_lines_[s][line], LV_ALIGN_TOP_LEFT, 4, 22 + line * 18);
    }
  }

  const char* system_labels[] = {"Helligkeit", "Kontrast", "Invert", "Aus nach",
                                 "Wi-Fi", "Felder"};
  for (int i = 0; i < 6; ++i) {
    create_metric_row(pages_[kPageSystem], i, 110 + i * 44, system_labels[i],
                      &metric_labels_[kPageSystem][i], &metric_values_[kPageSystem][i]);
  }

  brightness_overlay_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(brightness_overlay_, "80%");
  lv_obj_set_style_text_font(brightness_overlay_, dosis40, 0);
  lv_obj_set_style_text_color(brightness_overlay_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(brightness_overlay_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(brightness_overlay_);

  active_page_ = kPageOverview;
  apply_page_visibility();
  return ESP_OK;
}

void Ui::apply_snapshot_locked(const Msa2Snapshot& snapshot, bool force_ring_refresh) {
  if (using_dynamic_layout_) {
    last_snapshot_ = snapshot;
    layout_renderer_.update_data(snapshot);
    layout_renderer_.update_settings(display_settings_);
    return;
  }

  last_snapshot_ = snapshot;

  const double soc = snapshot.number_field("soc").value_or(0.0);
  char arc_buffer[16] = {};
  if (auto soc_opt = snapshot.number_field("soc")) {
    std::snprintf(arc_buffer, sizeof(arc_buffer), "%.0f%%", *soc_opt);
  } else {
    std::snprintf(arc_buffer, sizeof(arc_buffer), "--%%");
  }
  set_label_text_if_changed(arc_value_label_, arc_buffer);

  if (force_ring_refresh || true) {
    apply_ring_visual_locked(snapshot);
  }

  set_label_text_if_changed(detail_label_, snapshot.detail);

  if (snapshot.device_battery_present) {
    char dev_batt[16] = {};
    std::snprintf(dev_batt, sizeof(dev_batt), "%u%%", snapshot.device_battery_percent);
    set_label_text_if_changed(battery_pct_label_, dev_batt);
    set_label_text_if_changed(battery_icon_label_, battery_status_icon(snapshot));
    lv_obj_clear_flag(battery_icon_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(battery_pct_label_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(battery_icon_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_pct_label_, LV_OBJ_FLAG_HIDDEN);
  }

  set_label_text_if_changed(metric_values_[kPageOverview][0],
                            snapshot.format_field("sys_pv_p", " W"));
  set_label_text_if_changed(metric_values_[kPageOverview][1],
                            snapshot.format_field("sys_load_p", " W"));
  set_label_text_if_changed(metric_values_[kPageOverview][2],
                            snapshot.format_field("sys_grid_p", " W"));
  set_label_text_if_changed(metric_values_[kPageOverview][3], snapshot.format_field("bat_p", " W"));

  set_label_text_if_changed(metric_values_[kPageBattery][0], snapshot.format_field("soc", " %"));
  set_label_text_if_changed(metric_values_[kPageBattery][1], snapshot.format_field("bat_p", " W"));
  set_label_text_if_changed(metric_values_[kPageBattery][2], snapshot.format_field("bat_v", " V"));
  set_label_text_if_changed(metric_values_[kPageBattery][3], snapshot.format_field("bat_i", " A"));
  set_label_text_if_changed(metric_values_[kPageBattery][4],
                            snapshot.format_field("bat_temp", kDegreeC));
  set_label_text_if_changed(metric_values_[kPageBattery][5], snapshot.format_field("bat_sts"));

  set_label_text_if_changed(metric_values_[kPageGrid][0], snapshot.format_field("grid_p", " W"));
  set_label_text_if_changed(metric_values_[kPageGrid][1], snapshot.format_field("grid_v", " V"));
  set_label_text_if_changed(metric_values_[kPageGrid][2], snapshot.format_field("grid_i", " A"));
  set_label_text_if_changed(metric_values_[kPageGrid][3], snapshot.format_field("grid_f", " Hz"));
  set_label_text_if_changed(metric_values_[kPageGrid][4], snapshot.format_field("rssi", " dBm"));
  set_label_text_if_changed(metric_values_[kPageGrid][5],
                            snapshot.format_field("sys_grid_p", " W"));

  static const char* kSensorPrefixes[] = {"ht", "ht2", "ht3", "ht4"};
  static const char* kSensorTitles[] = {"Shelly H&T 1", "Shelly H&T 2", "Shelly H&T 3",
                                        "Shelly H&T 4"};
  for (int s = 0; s < 4; ++s) {
    const std::string prefix = kSensorPrefixes[s];
    const bool online = snapshot.bool_field((prefix + "_online").c_str()).value_or(false);
    set_label_text_if_changed(sensor_titles_[s],
                              std::string(kSensorTitles[s]) + (online ? "  online" : "  offline"));

    char line1[64] = {};
    char line2[64] = {};
    char line3[64] = {};
    std::snprintf(line1, sizeof(line1), "Temp: %s",
                  snapshot.format_field((prefix + "_temp").c_str(), kDegreeC).c_str());
    std::snprintf(line2, sizeof(line2), "Feuchte: %s",
                  snapshot.format_field((prefix + "_hum").c_str(), " %").c_str());
    std::snprintf(line3, sizeof(line3), "Batt/RSSI: %s / %s",
                  snapshot.format_field((prefix + "_batt").c_str(), " %").c_str(),
                  snapshot.format_field((prefix + "_rssi").c_str(), " dBm").c_str());
    set_label_text_if_changed(sensor_lines_[s][0], line1);
    set_label_text_if_changed(sensor_lines_[s][1], line2);
    set_label_text_if_changed(sensor_lines_[s][2], line3);
  }

  char brightness_buf[16] = {};
  std::snprintf(brightness_buf, sizeof(brightness_buf), "%d%%",
                display_settings_.brightness_percent);
  set_label_text_if_changed(metric_values_[kPageSystem][0], brightness_buf);

  char contrast_buf[16] = {};
  std::snprintf(contrast_buf, sizeof(contrast_buf), "%d%%", display_settings_.contrast_percent);
  set_label_text_if_changed(metric_values_[kPageSystem][1], contrast_buf);

  set_label_text_if_changed(metric_values_[kPageSystem][2],
                            display_settings_.invert ? "invertiert" : "normal");

  if (display_settings_.screen_off_seconds == 0) {
    set_label_text_if_changed(metric_values_[kPageSystem][3], "nie");
  } else {
    char off_buf[16] = {};
    std::snprintf(off_buf, sizeof(off_buf), "%lus", display_settings_.screen_off_seconds);
    set_label_text_if_changed(metric_values_[kPageSystem][3], off_buf);
  }

  set_label_text_if_changed(metric_values_[kPageSystem][4],
                            snapshot.wifi_connected ? "verbunden" : "getrennt");
  set_label_text_if_changed(metric_values_[kPageSystem][5], std::to_string(snapshot.fields.size()));

  (void)soc;
}

void Ui::apply_ring_visual_locked(const Msa2Snapshot& snapshot) {
  if (status_arc_ == nullptr) {
    return;
  }

  const double soc = snapshot.number_field("soc").value_or(0.0);
  const int arc_value = static_cast<int>(std::clamp(soc, 0.0, 100.0));
  display_arc_value_ = arc_value;
  lv_arc_set_value(status_arc_, arc_value);

  const uint32_t color = soc_color(arc_colors_, soc, snapshot.connected);
  lv_obj_set_style_arc_color(status_arc_, lv_color_hex(color), LV_PART_INDICATOR);

  const bool on_overview = !scrolling_ && active_page_ == kPageOverview;
  if (on_overview) {
    lv_obj_clear_flag(status_arc_, LV_OBJ_FLAG_HIDDEN);
    if (arc_value_label_ != nullptr) lv_obj_clear_flag(arc_value_label_, LV_OBJ_FLAG_HIDDEN);
    if (arc_title_label_ != nullptr) lv_obj_clear_flag(arc_title_label_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(status_arc_, LV_OBJ_FLAG_HIDDEN);
    if (arc_value_label_ != nullptr) lv_obj_add_flag(arc_value_label_, LV_OBJ_FLAG_HIDDEN);
    if (arc_title_label_ != nullptr) lv_obj_add_flag(arc_title_label_, LV_OBJ_FLAG_HIDDEN);
  }
}

void Ui::apply_page_visibility() {
  if (using_dynamic_layout_) {
    const int count = active_page_count();
    for (int i = 0; i < count; ++i) {
      lv_obj_t* p = layout_renderer_.page(i).page_obj;
      if (p != nullptr) lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  apply_ring_visual_locked(last_snapshot_);
}

lv_obj_t* Ui::page_object(int page) const {
  if (using_dynamic_layout_) {
    if (page < 0 || page >= layout_renderer_.page_count()) return nullptr;
    return layout_renderer_.page(page).page_obj;
  }
  if (page < 0 || page >= kPageCount) {
    return nullptr;
  }
  return pages_[page];
}

int Ui::clamp_page(int page) const {
  return std::clamp(page, 0, active_page_count() - 1);
}

int Ui::nearest_page_for_scroll() const {
  if (pager_ == nullptr) {
    return active_page_;
  }

  lv_obj_update_layout(pager_);
  int scroll_x = lv_obj_get_scroll_x(pager_);
  if (scroll_x < 0) {
    scroll_x = -scroll_x;
  }

  const int page_w =
      using_dynamic_layout_ ? layout_renderer_.layout_width() : board::kDisplayWidth;
  const int viewport_center = scroll_x + (page_w / 2);
  int best_page = active_page_;
  int best_distance = INT32_MAX;
  for (int page = 0; page < active_page_count(); ++page) {
    lv_obj_t* object = page_object(page);
    if (object == nullptr) {
      continue;
    }
    const int page_center = lv_obj_get_x(object) + (page_w / 2);
    const int distance = std::abs(page_center - viewport_center);
    if (distance < best_distance) {
      best_distance = distance;
      best_page = page;
    }
  }
  return best_page;
}

void Ui::set_active_page(int page) {
  const int clamped_page = clamp_page(page);
  lv_obj_update_layout(pager_);
  if (lv_obj_t* target_page = page_object(clamped_page); target_page != nullptr) {
    lv_obj_scroll_to_x(pager_, lv_obj_get_x(target_page), LV_ANIM_OFF);
  }
  active_page_ = clamped_page;
  scrolling_ = false;
  apply_page_visibility();
  apply_snapshot_locked(last_snapshot_, true);
}

void Ui::set_pager_scroll_locked(bool locked) {
  if (pager_ == nullptr || pager_scroll_locked_ == locked) {
    return;
  }
  pager_scroll_locked_ = locked;
  lv_obj_set_scroll_dir(pager_, locked ? LV_DIR_NONE : LV_DIR_HOR);
  if (!locked && pager_ != nullptr) {
    if (lv_obj_t* target_page = page_object(active_page_); target_page != nullptr) {
      lv_obj_scroll_to_x(pager_, lv_obj_get_x(target_page), LV_ANIM_OFF);
    }
  }
}

void Ui::soc_arc_guard_cb(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSING && code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED &&
      code != LV_EVENT_PRESS_LOST) {
    return;
  }

  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  lv_obj_t* arc = static_cast<lv_obj_t*>(lv_event_get_target(event));
  if (ui == nullptr || arc == nullptr) {
    return;
  }

  lv_arc_set_value(arc, ui->display_arc_value_);
  lv_event_stop_processing(event);
}

void Ui::pager_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_pager_event(event);
  }
}

void Ui::screen_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_screen_event(event);
  }
}

void Ui::handle_pager_event(lv_event_t* event) {
  if (pager_scroll_locked_) {
    return;
  }

  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_SCROLL_BEGIN) {
    scrolling_ = true;
    apply_page_visibility();
    return;
  }
  if (code != LV_EVENT_SCROLL_END) {
    return;
  }
  set_active_page(nearest_page_for_scroll());
}

void Ui::handle_screen_event(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
      code != LV_EVENT_PRESS_LOST) {
    return;
  }

  lv_indev_t* indev = lv_indev_get_act();
  if (indev == nullptr) {
    return;
  }

  lv_point_t point = {};
  lv_indev_get_point(indev, &point);

  if (code == LV_EVENT_PRESSED) {
    set_pager_scroll_locked(false);
    if (screen_power_mode_ == ScreenPowerMode::kOff) {
      note_activity(true);
      gesture_active_ = false;
      swipe_switched_ = false;
      overlay_visible_ = false;
      return;
    }
    note_activity(false);
    gesture_active_ = true;
    swipe_switched_ = false;
    overlay_visible_ = false;
    gesture_start_x_ = point.x;
    gesture_start_y_ = point.y;
    gesture_start_brightness_ = user_brightness_percent_;
    return;
  }

  if (code == LV_EVENT_PRESSING && gesture_active_) {
    note_activity(false);
    const int dx = static_cast<int>(point.x - gesture_start_x_);
    const int dy = static_cast<int>(gesture_start_y_ - point.y);
    const int abs_dx = std::abs(dx);
    const int abs_dy = std::abs(dy);

    if (swipe_switched_) {
      return;
    }

    if (!overlay_visible_) {
      const bool horizontal_swipe =
          abs_dx >= kSwipeThresholdPx && abs_dx >= (abs_dy - kGestureAxisLockMarginPx);
      const bool vertical_brightness =
          abs_dy >= kSwipeThresholdPx && abs_dx <= kBrightnessHorizontalTolerancePx &&
          abs_dy >= (abs_dx + kGestureAxisLockMarginPx);
      if (horizontal_swipe) {
        swipe_switched_ = true;
        return;
      }
      if (!vertical_brightness) {
        return;
      }
      set_pager_scroll_locked(true);
    }

    const float delta = static_cast<float>(dy) * (100.0f / 250.0f);
    const int new_brightness =
        std::clamp(gesture_start_brightness_ + static_cast<int>(std::lround(delta)),
                   kManualMinBrightnessPercent, 100);
    set_brightness_percent(new_brightness);
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%d%%", user_brightness_percent_);
    set_label_text_if_changed(brightness_overlay_, buffer);
    lv_obj_clear_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
    overlay_visible_ = true;
    return;
  }

  if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && gesture_active_) {
    note_activity(false);
    const int dx = static_cast<int>(point.x - gesture_start_x_);
    const int dy = static_cast<int>(gesture_start_y_ - point.y);
    const int abs_dx = std::abs(dx);
    const int abs_dy = std::abs(dy);
    const bool swipe_locked = swipe_switched_;

    gesture_active_ = false;
    swipe_switched_ = false;
    set_pager_scroll_locked(false);
    if (overlay_visible_) {
      lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
      overlay_visible_ = false;
      display_settings_.brightness_percent = user_brightness_percent_;
      display_settings_dirty_ = true;
      if (using_dynamic_layout_) {
        layout_renderer_.mark_settings_changed();
      }
      return;
    }

    if (swipe_locked && abs_dx >= kSwipeThresholdPx && abs_dx > abs_dy) {
      if (dx < 0) {
        set_active_page(active_page_ + 1);
      } else {
        set_active_page(active_page_ - 1);
      }
    }
  }
}

void Ui::set_brightness_percent(int brightness_percent) {
  const int clamped = std::clamp(brightness_percent, 0, 100);
  if (user_brightness_percent_ == clamped) {
    return;
  }
  user_brightness_percent_ = clamped;
  display_settings_.brightness_percent = clamped;

  if (initialized_) {
    LvglLockGuard lock(200);
    if (lock.locked()) {
      if (using_dynamic_layout_) {
        layout_renderer_.update_settings(display_settings_);
      } else if (metric_values_[kPageSystem][0] != nullptr) {
        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "%d%%", clamped);
        set_label_text_if_changed(metric_values_[kPageSystem][0], buf);
      }
    }
  }

  apply_brightness_policy();
}

void Ui::note_activity(bool wake_display_now) {
  last_activity_tick_ms_.store(lv_tick_get());
  if (wake_display_now) {
    wake_display();
  }
}

void Ui::wake_display() {
  if (screen_power_mode_ == ScreenPowerMode::kAwake) {
    return;
  }
  const bool was_off = screen_power_mode_ == ScreenPowerMode::kOff;
  screen_power_mode_ = ScreenPowerMode::kAwake;
  apply_brightness_policy();
  if (was_off) {
    esp_lv_adapter_resume();
  }
}

void Ui::request_wake_display() { note_activity(true); }

void Ui::apply_brightness_policy() {
  int target_brightness = user_brightness_percent_;
  if (screen_power_mode_ == ScreenPowerMode::kDimmed) {
    if (battery_policy_.dim_brightness_percent > 0) {
      target_brightness = std::clamp(battery_policy_.dim_brightness_percent, 1, 100);
    } else {
      target_brightness = std::max(8, std::min(18, std::max(1, user_brightness_percent_ / 3)));
    }
  } else if (screen_power_mode_ == ScreenPowerMode::kOff) {
    target_brightness = 0;
  }

  if (applied_brightness_percent_ == target_brightness) {
    return;
  }
  applied_brightness_percent_ = target_brightness;
  bsp_display_brightness_set(target_brightness);
}

bool Ui::is_low_power_mode_active() const {
  return screen_power_mode_ != ScreenPowerMode::kAwake;
}

void Ui::update_power_save(bool on_battery, bool keep_awake) {
  if (keep_awake) {
    note_activity(false);
  }

  if (screen_power_mode_ == ScreenPowerMode::kOff && gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
    note_activity(true);
    return;
  }

  const uint32_t now = lv_tick_get();
  const uint32_t idle_ms = now - last_activity_tick_ms_.load();
  const uint32_t dim_timeout =
      (keep_awake ? battery_policy_.dim_timeout_active_s : battery_policy_.dim_timeout_idle_s) *
      1000U;
  const uint32_t off_timeout =
      (keep_awake ? battery_policy_.off_timeout_active_s : battery_policy_.off_timeout_idle_s) *
      1000U;

  ScreenPowerMode target_mode = ScreenPowerMode::kAwake;
  const bool power_save_active = on_battery || battery_policy_.usb_power_save_enabled;
  const bool user_screen_off = display_settings_.screen_off_seconds > 0;

  if (power_save_active || user_screen_off) {
    if (battery_policy_.screen_off_enabled && idle_ms >= off_timeout) {
      target_mode = ScreenPowerMode::kOff;
    } else if (power_save_active && battery_policy_.dim_enabled && idle_ms >= dim_timeout) {
      target_mode = ScreenPowerMode::kDimmed;
    }
  }

  if (screen_power_mode_ != target_mode) {
    const bool was_off = screen_power_mode_ == ScreenPowerMode::kOff;
    const bool going_off = target_mode == ScreenPowerMode::kOff;
    screen_power_mode_ = target_mode;

    if (going_off && !was_off) {
      esp_err_t pause_ret = esp_lv_adapter_pause(1000);
      if (pause_ret != ESP_OK) {
        ESP_LOGW(kTag, "LVGL worker pause timeout - aborting screen-off (%s)",
                 esp_err_to_name(pause_ret));
        last_activity_tick_ms_.store(now);
        screen_power_mode_ = ScreenPowerMode::kAwake;
        return;
      }
    }

    apply_brightness_policy();

    if (was_off && !going_off) {
      esp_lv_adapter_resume();
    }
  }
}

}  // namespace printsphere
