#include "printsphere/layout_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_log.h"
#include "printsphere/board_config.hpp"

#ifdef HAS_EDITOR_LAYOUT
extern const char layout_json_start[] asm("_binary_layout_json_start");
extern const char layout_json_end[] asm("_binary_layout_json_end");
#endif

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
constexpr char kTag[] = "status.layout";

void enable_touch_bubble(lv_obj_t* obj) {
  if (obj == nullptr) return;
  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void make_input_pass_through(lv_obj_t* obj) {
  if (obj == nullptr) return;
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  enable_touch_bubble(obj);
}

void make_transparent(lv_obj_t* obj) {
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
}

const char* cjson_string(cJSON* obj, const char* key, const char* fallback = "") {
  cJSON* item = cJSON_GetObjectItem(obj, key);
  if (item != nullptr && cJSON_IsString(item)) return item->valuestring;
  return fallback;
}

double cjson_number(cJSON* obj, const char* key, double fallback = 0.0) {
  cJSON* item = cJSON_GetObjectItem(obj, key);
  if (item != nullptr && cJSON_IsNumber(item)) return item->valuedouble;
  return fallback;
}

void set_label_text_if_changed(lv_obj_t* label, const char* text) {
  if (label == nullptr || text == nullptr) return;
  const char* current = lv_label_get_text(label);
  if (current != nullptr && std::strcmp(current, text) == 0) return;
  lv_label_set_text(label, text);
}

}  // namespace

uint32_t LayoutRenderer::parse_css_color(const std::string& hex) {
  if (hex.empty() || hex[0] != '#') return 0xFFFFFF;
  unsigned long val = std::strtoul(hex.c_str() + 1, nullptr, 16);
  return static_cast<uint32_t>(val);
}

const lv_font_t* LayoutRenderer::pick_font(int size) {
  // Nur 20/32/40 px Bitmap-Fonts — Zwischengrößen würden sonst auf die nächsthöhere
  // Stufe springen (z. B. fontSize 27 → dosis_32) und im Editor enger wirken als am Display.
  if (size <= 27) return &dosis_20;
  if (size <= 39) return &dosis_32;
  return &dosis_40;
}

uint32_t LayoutRenderer::arc_color_for_value(const DynArc& arc, double pct) const {
  if (arc.color_mode == "threshold") {
    if (pct >= arc.threshold_high) return arc.color_high;
    if (pct >= arc.threshold_mid) return arc.color_mid;
    return arc.color_low;
  }
  if (arc.color_mode == "gradient") {
    double t = std::clamp(pct / 100.0, 0.0, 1.0);
    auto lerp = [](uint8_t a, uint8_t b, double t) -> uint8_t {
      return static_cast<uint8_t>(a + (b - a) * t);
    };
    uint8_t r = lerp((arc.gradient_start >> 16) & 0xFF, (arc.gradient_end >> 16) & 0xFF, t);
    uint8_t g = lerp((arc.gradient_start >> 8) & 0xFF, (arc.gradient_end >> 8) & 0xFF, t);
    uint8_t b = lerp(arc.gradient_start & 0xFF, arc.gradient_end & 0xFF, t);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
  }
  return arc.color_static;
}

bool LayoutRenderer::load_from_embedded() {
#ifndef HAS_EDITOR_LAYOUT
  return false;
#else
  const size_t len = layout_json_end - layout_json_start;
  if (len <= 2) {
    ESP_LOGI(kTag, "Embedded layout empty or minimal (%u bytes)", static_cast<unsigned>(len));
    return false;
  }

  cJSON* root = cJSON_ParseWithLength(layout_json_start, len);
  if (root == nullptr) {
    ESP_LOGE(kTag, "Failed to parse embedded layout JSON");
    return false;
  }

  cJSON* loc_item = cJSON_GetObjectItem(root, "locale");
  if (loc_item != nullptr && cJSON_IsString(loc_item)) {
    locale_ = loc_item->valuestring;
    if (locale_ != "en") locale_ = "de";
  }

  layout_width_ = board::kDisplayWidth;
  layout_height_ = board::kDisplayHeight;
  cJSON* display = cJSON_GetObjectItem(root, "display");
  if (display != nullptr && cJSON_IsObject(display)) {
    layout_width_ = static_cast<int>(cjson_number(display, "width", layout_width_));
    layout_height_ = static_cast<int>(cjson_number(display, "height", layout_height_));
    layout_width_ = std::clamp(layout_width_, 120, 800);
    layout_height_ = std::clamp(layout_height_, 120, 800);
    ESP_LOGI(kTag, "Layout display: %dx%d", layout_width_, layout_height_);
  }

  cJSON* pages_arr = cJSON_GetObjectItem(root, "pages");
  if (pages_arr == nullptr || !cJSON_IsArray(pages_arr)) {
    ESP_LOGI(kTag, "No 'pages' array in layout JSON");
    cJSON_Delete(root);
    return false;
  }

  int page_count = cJSON_GetArraySize(pages_arr);
  if (page_count == 0) {
    cJSON_Delete(root);
    return false;
  }

  pages_.clear();
  pages_.reserve(page_count);

  for (int pi = 0; pi < page_count; ++pi) {
    cJSON* page_json = cJSON_GetArrayItem(pages_arr, pi);
    DynPage dp;
    dp.name = cjson_string(page_json, "name", "Seite");

    cJSON* elements = cJSON_GetObjectItem(page_json, "elements");
    if (elements != nullptr && cJSON_IsArray(elements)) {
      int el_count = cJSON_GetArraySize(elements);
      for (int ei = 0; ei < el_count; ++ei) {
        cJSON* el = cJSON_GetArrayItem(elements, ei);
        const char* type = cjson_string(el, "type");

        if (std::strcmp(type, "label") == 0) {
          DynLabel dl;
          dl.field = cjson_string(el, "field");
          dl.suffix = cjson_string(el, "suffix");
          dl.format = cjson_string(el, "format");
          dl.static_text = cjson_string(el, "text", "--");
          dl.setting_key = cjson_string(el, "_settingKey");
          if (dl.setting_key.empty()) {
            dl.setting_key = cjson_string(el, "settingKey");
          }
          dp.labels.push_back(dl);
        } else if (std::strcmp(type, "arc") == 0) {
          DynArc da;
          da.field = cjson_string(el, "field");
          da.min_val = cjson_number(el, "min", 0);
          da.max_val = cjson_number(el, "max", 100);
          da.color_mode = cjson_string(el, "colorMode", "static");
          da.ring_style = cjson_string(el, "ringStyle", "standard");
          da.color_static = parse_css_color(cjson_string(el, "color", "#00ff00"));
          da.bg_color = parse_css_color(cjson_string(el, "bgColor", "#1a1a1a"));

          cJSON* thresholds = cJSON_GetObjectItem(el, "thresholds");
          if (thresholds != nullptr) {
            da.threshold_high = cjson_number(thresholds, "high", 60);
            da.threshold_mid = cjson_number(thresholds, "mid", 25);
            da.color_high = parse_css_color(cjson_string(thresholds, "colorHigh", "#00ff00"));
            da.color_mid = parse_css_color(cjson_string(thresholds, "colorMid", "#ffa500"));
            da.color_low = parse_css_color(cjson_string(thresholds, "colorLow", "#ff3333"));
          }

          cJSON* gradient = cJSON_GetObjectItem(el, "gradient");
          if (gradient != nullptr) {
            da.gradient_start = parse_css_color(cjson_string(gradient, "start", "#ff3333"));
            da.gradient_end = parse_css_color(cjson_string(gradient, "end", "#00ff00"));
          }

          dp.arcs.push_back(da);
        }
      }
    }
    pages_.push_back(std::move(dp));
  }

  cJSON_Delete(root);
  ESP_LOGI(kTag, "Loaded %d pages from embedded layout (json=%u bytes)",
           static_cast<int>(pages_.size()), static_cast<unsigned>(len));
  for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
    ESP_LOGI(kTag, "  Page %d '%s': %d labels, %d arcs",
             i, pages_[i].name.c_str(),
             static_cast<int>(pages_[i].labels.size()),
             static_cast<int>(pages_[i].arcs.size()));
  }
  return true;
#endif
}

void LayoutRenderer::build_pages(lv_obj_t* pager, DisplaySettings* live_settings) {
  live_settings_ = live_settings;
#ifdef HAS_EDITOR_LAYOUT
  const size_t len = layout_json_end - layout_json_start;
  cJSON* root = cJSON_ParseWithLength(layout_json_start, len);
  if (root == nullptr) return;

  cJSON* pages_arr = cJSON_GetObjectItem(root, "pages");
  if (pages_arr == nullptr) {
    cJSON_Delete(root);
    return;
  }

  for (int pi = 0; pi < static_cast<int>(pages_.size()); ++pi) {
    DynPage& dp = pages_[pi];
    cJSON* page_json = cJSON_GetArrayItem(pages_arr, pi);
    if (page_json == nullptr) {
      ESP_LOGW(kTag, "Page %d JSON missing, skipping", pi);
      continue;
    }

    dp.page_obj = lv_obj_create(pager);
    if (dp.page_obj == nullptr) {
      ESP_LOGE(kTag, "Failed to create LVGL page %d", pi);
      continue;
    }
    lv_obj_set_size(dp.page_obj, layout_width(), layout_height());
    make_transparent(dp.page_obj);
    lv_obj_clear_flag(dp.page_obj, LV_OBJ_FLAG_SCROLLABLE);
    enable_touch_bubble(dp.page_obj);

    dp.title_label = nullptr;

    cJSON* elements = cJSON_GetObjectItem(page_json, "elements");
    if (elements == nullptr) continue;
    int el_count = cJSON_GetArraySize(elements);

    int label_idx = 0;
    int arc_idx = 0;

    for (int ei = 0; ei < el_count; ++ei) {
      cJSON* el = cJSON_GetArrayItem(elements, ei);
      const char* type = cjson_string(el, "type");
      int x = static_cast<int>(cjson_number(el, "x", 0));
      int y = static_cast<int>(cjson_number(el, "y", 0));
      int w = static_cast<int>(cjson_number(el, "w", 0));
      int h = static_cast<int>(cjson_number(el, "h", 0));

      if (std::strcmp(type, "label") == 0) {
        if (label_idx >= static_cast<int>(dp.labels.size())) continue;
        DynLabel& dl = dp.labels[label_idx++];

        int font_size = static_cast<int>(cjson_number(el, "fontSize", 20));
        const char* anchor = cjson_string(el, "anchor", "start");
        const char* font_family = cjson_string(el, "fontFamily", "Dosis");
        uint32_t color = parse_css_color(cjson_string(el, "color", "#ffffff"));

        lv_obj_t* label = lv_label_create(dp.page_obj);
        if (std::strcmp(font_family, "MDI") == 0) {
          lv_obj_set_style_text_font(label, font_size >= 35 ? &mdi_40 : &mdi_30, 0);
          dl.is_icon = true;
        } else {
          lv_obj_set_style_text_font(label, pick_font(font_size), 0);
        }
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);

        if (std::strcmp(anchor, "middle") == 0) {
          lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
          lv_obj_align(label, LV_ALIGN_TOP_MID, x - (layout_width() / 2), y);
        } else if (std::strcmp(anchor, "end") == 0) {
          // x = rechte Kante (wie Editor: left + translateX(-100%)), nicht Label-Breite
          lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
          lv_obj_align(label, LV_ALIGN_TOP_RIGHT, x - layout_width(), y);
        } else {
          lv_obj_set_pos(label, x, y);
        }

        lv_label_set_text(label, dl.static_text.c_str());

        if (!dl.setting_key.empty() || dl.field == "_wifi") {
          lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_set_ext_click_area(label, 15);
          lv_obj_add_event_cb(label, &LayoutRenderer::setting_click_cb,
                              LV_EVENT_SHORT_CLICKED, this);
        } else {
          make_input_pass_through(label);
        }
        dl.obj = label;

      } else if (std::strcmp(type, "arc") == 0) {
        if (arc_idx >= static_cast<int>(dp.arcs.size())) continue;
        DynArc& da = dp.arcs[arc_idx++];

        int stroke = static_cast<int>(cjson_number(el, "strokeWidth", 22));

        lv_obj_t* arc = lv_arc_create(dp.page_obj);
        int arc_w = w;
        int arc_h = h;
        if (arc_w >= layout_width() - 10) arc_w = layout_width() - stroke - 4;
        if (arc_h >= layout_height() - 10) arc_h = layout_height() - stroke - 4;
        lv_obj_set_size(arc, arc_w, arc_h);
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(arc, 135);
        lv_arc_set_bg_angles(arc, 0, 270);
        lv_arc_set_range(arc, static_cast<int32_t>(da.min_val),
                         static_cast<int32_t>(da.max_val));
        lv_arc_set_value(arc, static_cast<int32_t>(da.min_val));
        lv_obj_set_style_arc_width(arc, stroke, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, stroke, LV_PART_INDICATOR);
        if (da.ring_style == "dynamic") {
          lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        } else {
          lv_obj_set_style_arc_color(arc, lv_color_hex(da.bg_color), LV_PART_MAIN);
        }
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(arc, 0);
        enable_touch_bubble(arc);

        lv_obj_set_style_arc_color(arc, lv_color_hex(da.color_static), LV_PART_INDICATOR);
        da.obj = arc;
      }
    }
  }

  cJSON_Delete(root);
  for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
    int lbl_ok = 0;
    int arc_ok = 0;
    for (const auto& l : pages_[i].labels) { if (l.obj) lbl_ok++; }
    for (const auto& a : pages_[i].arcs) { if (a.obj) arc_ok++; }
    ESP_LOGI(kTag, "Built page %d '%s': page_obj=%p  labels=%d  arcs=%d",
             i, pages_[i].name.c_str(), (void*)pages_[i].page_obj, lbl_ok, arc_ok);
  }
  ESP_LOGI(kTag, "Built %d dynamic pages total", static_cast<int>(pages_.size()));
#else
  (void)pager;
#endif
}

void LayoutRenderer::update_data(const Msa2Snapshot& snapshot) {
  for (auto& dp : pages_) {
    for (auto& dl : dp.labels) {
      if (dl.obj == nullptr) continue;

      if (!dl.field.empty()) {
        if (dl.field == "_wifi") {
          set_label_text_if_changed(dl.obj,
              snapshot.wifi_connected ? tr("verbunden", "connected")
                                      : tr("getrennt", "disconnected"));
        } else if (dl.field == "_fieldcount") {
          set_label_text_if_changed(dl.obj,
              std::to_string(snapshot.fields.size()).c_str());
        } else if (dl.field == "_time") {
          time_t now = 0;
          time(&now);
          struct tm ti = {};
          localtime_r(&now, &ti);
          char tbuf[16] = {};
          std::strftime(tbuf, sizeof(tbuf), "%H:%M", &ti);
          if (ti.tm_year > 70) {
            set_label_text_if_changed(dl.obj, tbuf);
          } else {
            set_label_text_if_changed(dl.obj, "--:--");
          }
        } else if (dl.field == "_date") {
          time_t now = 0;
          time(&now);
          struct tm ti = {};
          localtime_r(&now, &ti);
          char dbuf[16] = {};
          std::strftime(dbuf, sizeof(dbuf), "%d.%m.%Y", &ti);
          if (ti.tm_year > 70) {
            set_label_text_if_changed(dl.obj, dbuf);
          } else {
            set_label_text_if_changed(dl.obj, "--.--.----");
          }
        } else if (dl.field == "_battery_icon") {
          const char* icon;
          if (!snapshot.device_battery_present) {
            icon = "\xF3\xB0\x81\xB9";  // mdi-battery (full/placeholder)
          } else if (snapshot.device_charging) {
            icon = "\xF3\xB0\xA0\x87";  // mdi-battery-charging-100
          } else {
            int pct = static_cast<int>(snapshot.device_battery_percent);
            if (pct >= 90)      icon = "\xF3\xB0\x81\xB9";  // battery-100
            else if (pct >= 80) icon = "\xF3\xB0\x82\x81";  // battery-80
            else if (pct >= 70) icon = "\xF3\xB0\x82\x80";  // battery-70
            else if (pct >= 60) icon = "\xF3\xB0\x81\xBF";  // battery-60
            else if (pct >= 50) icon = "\xF3\xB0\x81\xBE";  // battery-50
            else if (pct >= 40) icon = "\xF3\xB0\x81\xBD";  // battery-40
            else if (pct >= 30) icon = "\xF3\xB0\x81\xBC";  // battery-30
            else if (pct >= 20) icon = "\xF3\xB0\x81\xBB";  // battery-20
            else if (pct >= 10) icon = "\xF3\xB0\x81\xBA";  // battery-10
            else                icon = "\xF3\xB0\x81\xBA";  // battery-10
          }
          set_label_text_if_changed(dl.obj, icon);
        } else if (dl.field == "_battery_pct") {
          if (snapshot.device_battery_present) {
            char buf[8] = {};
            std::snprintf(buf, sizeof(buf), "%u%%", snapshot.device_battery_percent);
            set_label_text_if_changed(dl.obj, buf);
          } else {
            set_label_text_if_changed(dl.obj, "--");
          }
        } else if (dl.field == "_ip") {
          set_label_text_if_changed(dl.obj,
              snapshot.wifi_ip.empty() ? "--" : snapshot.wifi_ip.c_str());
        } else if (dl.field == "_ap_ssid") {
          set_label_text_if_changed(dl.obj,
              snapshot.setup_ap_active ? snapshot.setup_ap_ssid.c_str() : tr("aus", "off"));
        } else if (dl.field == "_uptime") {
          set_label_text_if_changed(dl.obj,
              snapshot.updated_ms > 0 ? tr("aktiv", "active") : "--");
        } else {
          auto val = snapshot.number_field(dl.field.c_str());
          if (val.has_value()) {
            char buf[64] = {};
            if (dl.format == "int") {
              std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(*val), dl.suffix.c_str());
            } else {
              std::snprintf(buf, sizeof(buf), "%.1f%s", *val, dl.suffix.c_str());
            }
            set_label_text_if_changed(dl.obj, buf);
          } else {
            auto str_val = snapshot.string_field(dl.field.c_str());
            if (str_val.has_value()) {
              std::string text = *str_val + dl.suffix;
              set_label_text_if_changed(dl.obj, text.c_str());
            } else {
              std::string text = std::string("--") + dl.suffix;
              set_label_text_if_changed(dl.obj, text.c_str());
            }
          }
        }
      } else if (!dl.setting_key.empty()) {
        // handled in update_settings
      } else {
        set_label_text_if_changed(dl.obj, dl.static_text.c_str());
      }
    }

    for (auto& da : dp.arcs) {
      if (da.obj == nullptr || da.field.empty()) continue;

      auto val = snapshot.number_field(da.field.c_str());
      double raw = val.value_or(da.min_val);
      double clamped = std::clamp(raw, da.min_val, da.max_val);
      lv_arc_set_value(da.obj, static_cast<int32_t>(clamped));

      double range = da.max_val - da.min_val;
      double pct = (range > 0) ? ((clamped - da.min_val) / range * 100.0) : 0.0;
      uint32_t color = arc_color_for_value(da, pct);
      lv_obj_set_style_arc_color(da.obj, lv_color_hex(color), LV_PART_INDICATOR);
    }
  }
}

void LayoutRenderer::update_settings(const DisplaySettings& settings) {
  for (auto& dp : pages_) {
    for (auto& dl : dp.labels) {
      if (dl.obj == nullptr || dl.setting_key.empty()) continue;

      char buf[32] = {};
      if (dl.setting_key == "brightness") {
        std::snprintf(buf, sizeof(buf), "%d%%", settings.brightness_percent);
      } else if (dl.setting_key == "contrast") {
        std::snprintf(buf, sizeof(buf), "%d%%", settings.contrast_percent);
      } else if (dl.setting_key == "invert") {
        std::snprintf(buf, sizeof(buf), "%s",
                      settings.invert ? tr("invertiert", "inverted") : tr("normal", "normal"));
      } else if (dl.setting_key == "screenOff") {
        if (settings.screen_off_seconds == 0) {
          std::snprintf(buf, sizeof(buf), "%s", tr("nie", "never"));
        } else {
          std::snprintf(buf, sizeof(buf), "%lus", settings.screen_off_seconds);
        }
      }
      if (buf[0] != '\0') {
        set_label_text_if_changed(dl.obj, buf);
      }
    }
  }
}

lv_obj_t* LayoutRenderer::first_arc_obj() const {
  for (const auto& dp : pages_) {
    for (const auto& da : dp.arcs) {
      if (da.obj != nullptr) return da.obj;
    }
  }
  return nullptr;
}

bool LayoutRenderer::consume_settings_changed() {
  return settings_changed_.exchange(false);
}

void LayoutRenderer::mark_settings_changed() {
  settings_changed_.store(true);
}

bool LayoutRenderer::consume_wifi_reset() {
  return wifi_reset_requested_.exchange(false);
}

const char* LayoutRenderer::tr(const char* de, const char* en) const {
  return (locale_ == "en") ? en : de;
}

void LayoutRenderer::setting_click_cb(lv_event_t* event) {
  auto* self = static_cast<LayoutRenderer*>(lv_event_get_user_data(event));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
  if (self == nullptr || target == nullptr) return;

  for (const auto& dp : self->pages_) {
    for (const auto& dl : dp.labels) {
      if (dl.obj == target) {
        if (!dl.setting_key.empty()) {
          self->handle_setting_click(dl.setting_key);
        } else if (dl.field == "_wifi") {
          self->handle_setting_click("wifiReset");
        }
        return;
      }
    }
  }
}

void LayoutRenderer::handle_setting_click(const std::string& key) {
  if (live_settings_ == nullptr) return;

  if (key == "brightness") {
    int v = live_settings_->brightness_percent + 10;
    if (v > 100) v = 10;
    live_settings_->brightness_percent = v;
    settings_changed_.store(true);
  } else if (key == "contrast") {
    int v = live_settings_->contrast_percent + 10;
    if (v > 100) v = 10;
    live_settings_->contrast_percent = v;
    settings_changed_.store(true);
  } else if (key == "invert") {
    live_settings_->invert = !live_settings_->invert;
    settings_changed_.store(true);
  } else if (key == "screenOff") {
    static constexpr uint32_t kPresets[] = {10, 15, 30, 60, 120, 0};
    static constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
    int next = 0;
    for (int i = 0; i < kPresetCount; ++i) {
      if (kPresets[i] == live_settings_->screen_off_seconds) {
        next = (i + 1) % kPresetCount;
        break;
      }
    }
    live_settings_->screen_off_seconds = kPresets[next];
    settings_changed_.store(true);
  } else if (key == "wifiReset") {
    show_wifi_reset_confirm();
    return;
  }

  if (settings_changed_.load()) {
    update_settings(*live_settings_);
  }
}

void LayoutRenderer::show_wifi_reset_confirm() {
  lv_obj_t* mbox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(mbox, 320, 200);
  lv_obj_align(mbox, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1A1A2E), 0);
  lv_obj_set_style_bg_opa(mbox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(mbox, lv_color_hex(0x3A3A5E), 0);
  lv_obj_set_style_border_width(mbox, 2, 0);
  lv_obj_set_style_radius(mbox, 20, 0);
  lv_obj_set_style_pad_all(mbox, 20, 0);
  lv_obj_clear_flag(mbox, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(mbox);
  lv_label_set_text(title, tr("WiFi vergessen?", "Forget WiFi?"));
  lv_obj_set_style_text_font(title, pick_font(28), 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

  lv_obj_t* desc = lv_label_create(mbox);
  lv_label_set_text(desc, tr("WLAN-Daten werden\ngeloescht. Setup-AP\nwird aktiviert.",
                            "WiFi credentials will be\nerased. Setup AP\nwill be enabled."));
  lv_obj_set_style_text_font(desc, pick_font(18), 0);
  lv_obj_set_style_text_color(desc, lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(desc, LV_ALIGN_CENTER, 0, -5);

  auto make_btn = [&](const char* text, lv_align_t align, int x_ofs, uint32_t bg_color) {
    lv_obj_t* btn = lv_obj_create(mbox);
    lv_obj_set_size(btn, 120, 44);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, pick_font(22), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    return btn;
  };

  lv_obj_t* btn_no = make_btn(tr("Nein", "No"), LV_ALIGN_BOTTOM_LEFT, 10, 0x334155);
  lv_obj_t* btn_yes = make_btn(tr("Ja", "Yes"), LV_ALIGN_BOTTOM_RIGHT, -10, 0xDC2626);

  lv_obj_add_event_cb(btn_no, &LayoutRenderer::wifi_confirm_cb, LV_EVENT_SHORT_CLICKED, this);
  lv_obj_add_event_cb(btn_yes, &LayoutRenderer::wifi_confirm_cb, LV_EVENT_SHORT_CLICKED, this);
}

void LayoutRenderer::wifi_confirm_cb(lv_event_t* event) {
  auto* self = static_cast<LayoutRenderer*>(lv_event_get_user_data(event));
  lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(event));
  if (self == nullptr || btn == nullptr) return;

  lv_obj_t* mbox = lv_obj_get_parent(btn);
  if (mbox == nullptr) return;

  lv_obj_t* btn_label = lv_obj_get_child(btn, 0);
  bool confirmed = false;
  if (btn_label != nullptr) {
    const char* text = lv_label_get_text(btn_label);
    if (text != nullptr &&
        (std::strcmp(text, "Ja") == 0 || std::strcmp(text, "Yes") == 0)) {
      confirmed = true;
    }
  }

  lv_obj_delete(mbox);

  if (confirmed) {
    self->wifi_reset_requested_.store(true);
    for (auto& dp : self->pages_) {
      for (auto& dl : dp.labels) {
        if (dl.field == "_wifi" && dl.obj != nullptr) {
          set_label_text_if_changed(dl.obj, self->tr("Reset...", "Reset..."));
        }
      }
    }
  }
}

}  // namespace printsphere
