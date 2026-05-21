#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "lvgl.h"
#include "printsphere/board_config.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/msa2_status.hpp"

namespace printsphere {

struct DynLabel {
  lv_obj_t* obj = nullptr;
  std::string field;
  std::string suffix;
  std::string format;
  std::string static_text;
  std::string setting_key;
  bool is_icon = false;
};

struct DynArc {
  lv_obj_t* obj = nullptr;
  std::string field;
  double min_val = 0;
  double max_val = 100;
  std::string color_mode;
  std::string ring_style;  // "standard" or "dynamic"
  uint32_t color_static = 0x00FF00;
  uint32_t bg_color = 0x1A1A1A;
  double threshold_high = 60;
  double threshold_mid = 25;
  uint32_t color_high = 0x00FF00;
  uint32_t color_mid = 0xFFA500;
  uint32_t color_low = 0xFF3333;
  uint32_t gradient_start = 0xFF3333;
  uint32_t gradient_end = 0x00FF00;
};

struct DynPage {
  std::string name;
  lv_obj_t* page_obj = nullptr;
  lv_obj_t* title_label = nullptr;
  std::vector<DynLabel> labels;
  std::vector<DynArc> arcs;
};

class LayoutRenderer {
 public:
  bool load_from_embedded();
  bool has_layout() const { return !pages_.empty(); }
  int page_count() const { return static_cast<int>(pages_.size()); }
  int layout_width() const { return layout_width_ > 0 ? layout_width_ : board::kDisplayWidth; }
  int layout_height() const { return layout_height_ > 0 ? layout_height_ : board::kDisplayHeight; }

  void build_pages(lv_obj_t* pager, DisplaySettings* live_settings);
  void update_data(const Msa2Snapshot& snapshot);
  void update_settings(const DisplaySettings& settings);

  DynPage& page(int index) { return pages_[index]; }
  const DynPage& page(int index) const { return pages_[index]; }
  lv_obj_t* first_arc_obj() const;

  bool consume_settings_changed();
  void mark_settings_changed();
  bool consume_wifi_reset();

  void set_locale(const std::string& locale) { locale_ = locale; }

 private:
  const char* tr(const char* de, const char* en) const;
  const char* translate_static(const char* text) const;

  std::string locale_ = "de";
  static void setting_click_cb(lv_event_t* event);
  static void wifi_confirm_cb(lv_event_t* event);
  void handle_setting_click(const std::string& key);
  void show_wifi_reset_confirm();

  std::vector<DynPage> pages_;
  int layout_width_ = 0;
  int layout_height_ = 0;
  DisplaySettings* live_settings_ = nullptr;
  std::atomic<bool> settings_changed_{false};
  std::atomic<bool> wifi_reset_requested_{false};

  static const lv_font_t* pick_font(int size);
  static uint32_t parse_css_color(const std::string& hex);
  uint32_t arc_color_for_value(const DynArc& arc, double pct) const;
};

}  // namespace printsphere
