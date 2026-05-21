#include "printsphere/setup_portal.hpp"

#include <algorithm>
#include <vector>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "printsphere/msa2_status.hpp"
#include "printsphere/time_sync.hpp"
#include "printsphere/ui.hpp"

namespace printsphere {

namespace {

constexpr char kTag[] = "status.portal";

std::string json_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char ch : input) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

void send_json(httpd_req_t* request, const std::string& body) {
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t receive_json_body(httpd_req_t* request, cJSON** root_out) {
  if (request->content_len <= 0 || request->content_len > 4096) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid body length");
  }

  std::vector<char> body(static_cast<size_t>(request->content_len) + 1U, '\0');
  int received = 0;
  while (received < request->content_len) {
    const int ret = httpd_req_recv(request, body.data() + received, request->content_len - received);
    if (ret <= 0) {
      return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "body read failed");
    }
    received += ret;
  }

  cJSON* root = cJSON_Parse(body.data());
  if (root == nullptr) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid json");
  }

  *root_out = root;
  return ESP_OK;
}

std::string read_string_field(const cJSON* object, const char* key) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return {};
  }
  return item->valuestring;
}

int read_int_field(const cJSON* object, const char* key, int fallback) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return item->valueint;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return std::atoi(item->valuestring);
  }
  return fallback;
}

bool read_bool_field(const cJSON* object, const char* key, bool fallback) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsBool(item)) {
    return cJSON_IsTrue(item);
  }
  if (cJSON_IsNumber(item)) {
    return item->valueint != 0;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    const std::string v = item->valuestring;
    if (v == "1" || v == "true" || v == "on") return true;
    if (v == "0" || v == "false" || v == "off") return false;
  }
  return fallback;
}

uint32_t parse_hex_color(const std::string& hex, uint32_t fallback) {
  if (hex.size() < 6) return fallback;
  const char* start = (hex[0] == '#') ? hex.c_str() + 1 : hex.c_str();
  char* end = nullptr;
  const unsigned long val = strtoul(start, &end, 16);
  if (end == start) return fallback;
  return static_cast<uint32_t>(val);
}

}  // namespace

esp_err_t SetupPortal::start() {
  if (server_ != nullptr) {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 12288;
  config.max_uri_handlers = 22;
  config.recv_wait_timeout = 30;

  ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), kTag, "httpd_start failed");

  auto reg = [&](const char* uri, httpd_method_t method,
                 esp_err_t (*handler)(httpd_req_t*)) -> esp_err_t {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = this;
    return httpd_register_uri_handler(server_, &u);
  };

  ESP_RETURN_ON_ERROR(reg("/", HTTP_GET, &SetupPortal::handle_root), kTag, "root failed");
  ESP_RETURN_ON_ERROR(reg("/generate_204", HTTP_GET, &SetupPortal::handle_captive_redirect), kTag, "captive failed");
  ESP_RETURN_ON_ERROR(reg("/hotspot-detect.html", HTTP_GET, &SetupPortal::handle_captive_redirect), kTag, "captive failed");
  ESP_RETURN_ON_ERROR(reg("/library/test/success.html", HTTP_GET, &SetupPortal::handle_captive_redirect), kTag, "captive failed");
  ESP_RETURN_ON_ERROR(reg("/connecttest.txt", HTTP_GET, &SetupPortal::handle_captive_redirect), kTag, "captive failed");
  ESP_RETURN_ON_ERROR(reg("/ncsi.txt", HTTP_GET, &SetupPortal::handle_captive_redirect), kTag, "captive failed");
  ESP_RETURN_ON_ERROR(reg("/api/health", HTTP_GET, &SetupPortal::handle_health), kTag, "health failed");
  ESP_RETURN_ON_ERROR(reg("/api/config", HTTP_GET, &SetupPortal::handle_config_get), kTag, "config get failed");
  ESP_RETURN_ON_ERROR(reg("/api/config", HTTP_POST, &SetupPortal::handle_config_post), kTag, "config post failed");
  ESP_RETURN_ON_ERROR(reg("/api/timezone", HTTP_POST, &SetupPortal::handle_timezone_post), kTag, "timezone failed");
  ESP_RETURN_ON_ERROR(reg("/api/wifi/scan", HTTP_GET, &SetupPortal::handle_wifi_scan), kTag, "wifi scan failed");
  ESP_RETURN_ON_ERROR(reg("/api/arc-colors", HTTP_POST, &SetupPortal::handle_arc_colors_post), kTag, "arc colors failed");
  ESP_RETURN_ON_ERROR(reg("/api/display-settings", HTTP_GET, &SetupPortal::handle_display_settings_get), kTag, "display get failed");
  ESP_RETURN_ON_ERROR(reg("/api/display-settings", HTTP_POST, &SetupPortal::handle_display_settings_post), kTag, "display post failed");

  ESP_LOGI(kTag, "Setup portal ready at http://%s/",
           wifi_manager_.setup_access_point_ip().c_str());
  return ESP_OK;
}

esp_err_t SetupPortal::handle_captive_redirect(httpd_req_t* request) {
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, nullptr, 0);
}

void SetupPortal::request_unlock_pin() {}

PortalAccessSnapshot SetupPortal::access_snapshot(bool request_authorized) {
  PortalAccessSnapshot snapshot;
  snapshot.lock_enabled = false;
  snapshot.request_authorized = request_authorized;
  snapshot.detail = "Portal open";
  return snapshot;
}

esp_err_t SetupPortal::handle_root(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const std::string status_url = portal->config_store_.load_status_url();
  const Msa2Snapshot status = portal->status_client_.snapshot();

  std::string html;
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
          "content=\"width=device-width,initial-scale=1\"><title>Status Sphere Config</title>";
  html += "<style>body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e5eef7;"
          "margin:0;padding:24px}h1{margin:0 0 8px}.card{background:#141b24;border:1px solid "
          "#243041;border-radius:16px;padding:20px;margin:16px 0}label{display:block;margin:"
          "12px 0 6px;color:#94a3b8}input{width:100%;padding:12px;border-radius:10px;border:1px "
          "solid #334155;background:#0f1720;color:#fff;box-sizing:border-box}button{margin-top:"
          "16px;padding:12px 18px;border:none;border-radius:10px;background:#22c55e;color:#041;"
          "font-weight:600;cursor:pointer}.status{color:#94a3b8;font-size:14px;margin-top:12px}"
          "</style></head><body>";
  html += "<h1>Status Sphere</h1><p class=\"status\">Round JSON status display for ESP32 AMOLED</p>";
  html += "<div class=\"card\"><h2>Status Endpoint</h2>";
  html += "<label for=\"status_url\">JSON URL</label>";
  html += "<input id=\"status_url\" value=\"" + json_escape(status_url) + "\">";
  html += "<button onclick=\"saveConfig()\">Save endpoint</button></div>";
  html += "<div class=\"card\"><h2>Wi-Fi</h2>";
  html += "<label for=\"wifi_ssid\">SSID</label>";
  html += "<input id=\"wifi_ssid\" value=\"" + json_escape(wifi.ssid) + "\">";
  html += "<label for=\"wifi_pass\">Password</label>";
  html += "<input id=\"wifi_pass\" type=\"password\" placeholder=\"Leave empty to keep saved\">";
  html += "<button onclick=\"saveWifi()\">Save Wi-Fi</button></div>";
  html += "<div class=\"card\"><h2>Live Status</h2>";
  html += "<div class=\"status\">Connected: " + std::string(status.connected ? "yes" : "no") +
          "<br>Detail: " + json_escape(status.detail) + "<br>Fields: " +
          std::to_string(status.fields.size()) + "</div>";
  html += "<button onclick=\"location.reload()\">Refresh</button></div>";
  html += "<script>";
  html += "async function saveConfig(){const status_url=document.getElementById('status_url').value;"
          "await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({status_url})});alert('Saved. Device will use new endpoint.');}";
  html += "async function saveWifi(){const wifi_ssid=document.getElementById('wifi_ssid').value;"
          "const wifi_password=document.getElementById('wifi_pass').value;"
          "await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({wifi_ssid,wifi_password})});alert('Wi-Fi saved. Reboot may be "
          "needed if connection fails.');}";
  html += "(async function(){try{const tz=Intl.DateTimeFormat().resolvedOptions().timeZone;"
          "if(tz){await fetch('/api/timezone',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({tz_iana:tz})});}}catch(e){}})();";
  html += "</script></body></html>";

  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, html.c_str(), html.size());
}

esp_err_t SetupPortal::handle_health(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  const Msa2Snapshot status = portal->status_client_.snapshot();
  std::string body = "{";
  body += "\"connected\":" + std::string(status.connected ? "true" : "false") + ",";
  body += "\"detail\":\"" + json_escape(status.detail) + "\",";
  body += "\"wifi_connected\":" +
          std::string(portal->wifi_manager_.is_station_connected() ? "true" : "false") + ",";
  body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\",";
  body += "\"field_count\":" + std::to_string(status.fields.size()) + ",";
  body += "\"updated_ms\":" + std::to_string(status.updated_ms);
  if (!status.raw_json.empty()) {
    body += ",\"raw_json\":" + status.raw_json;
  }
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_get(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const std::string status_url = portal->config_store_.load_status_url();
  std::string body = "{";
  body += "\"wifi_ssid\":\"" + json_escape(wifi.ssid) + "\",";
  body += "\"status_url\":\"" + json_escape(status_url) + "\"";
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  cJSON* root = nullptr;
  ESP_RETURN_ON_ERROR(receive_json_body(request, &root), kTag, "json body failed");

  const std::string status_url = read_string_field(root, "status_url");
  const std::string wifi_ssid = read_string_field(root, "wifi_ssid");
  const std::string wifi_password = read_string_field(root, "wifi_password");
  cJSON_Delete(root);

  if (!status_url.empty()) {
    portal->config_store_.save_status_url(status_url);
    portal->status_client_.configure(status_url);
  }

  if (!wifi_ssid.empty()) {
    WifiCredentials credentials = portal->config_store_.load_wifi_credentials();
    credentials.ssid = wifi_ssid;
    if (!wifi_password.empty()) {
      credentials.password = wifi_password;
    }
    portal->config_store_.save_wifi_credentials(credentials);
    portal->wifi_manager_.connect_station(credentials);
  }

  send_json(request, "{\"ok\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_timezone_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string tz_iana = read_string_field(root, "tz_iana");
  cJSON_Delete(root);

  if (!tz_iana.empty() && time_sync::iana_to_posix(tz_iana).empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "unsupported timezone");
  }

  ESP_LOGI(kTag, "Saving timezone: '%s'", tz_iana.c_str());
  ESP_RETURN_ON_ERROR(portal->config_store_.save_timezone_iana(tz_iana), kTag,
                      "save timezone failed");
  time_sync::set_timezone_iana(tz_iana.empty() ? "Europe/Berlin" : tz_iana);

  send_json(request, "{\"ok\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_wifi_scan(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  const std::vector<std::string> networks = portal->wifi_manager_.scan_visible_networks();
  std::string body = "{\"networks\":[";
  for (size_t i = 0; i < networks.size(); ++i) {
    if (i > 0) {
      body += ",";
    }
    body += "\"" + json_escape(networks[i]) + "\"";
  }
  body += "]}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_display_settings_get(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  const DisplaySettings settings = portal->config_store_.load_display_settings();
  std::string body = "{";
  body += "\"brightness_percent\":" + std::to_string(settings.brightness_percent) + ",";
  body += "\"contrast_percent\":" + std::to_string(settings.contrast_percent) + ",";
  body += "\"invert\":" + std::string(settings.invert ? "true" : "false") + ",";
  body += "\"screen_off_seconds\":" + std::to_string(settings.screen_off_seconds);
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_display_settings_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  cJSON* root = nullptr;
  ESP_RETURN_ON_ERROR(receive_json_body(request, &root), kTag, "display settings json failed");

  DisplaySettings settings = portal->config_store_.load_display_settings();
  settings.brightness_percent =
      read_int_field(root, "brightness_percent", settings.brightness_percent);
  settings.contrast_percent = read_int_field(root, "contrast_percent", settings.contrast_percent);
  settings.invert = read_bool_field(root, "invert", settings.invert);
  settings.screen_off_seconds = static_cast<uint32_t>(
      read_int_field(root, "screen_off_seconds", static_cast<int>(settings.screen_off_seconds)));
  cJSON_Delete(root);

  settings.brightness_percent = std::clamp(settings.brightness_percent, 0, 100);
  settings.contrast_percent = std::clamp(settings.contrast_percent, 0, 100);

  portal->config_store_.save_display_settings(settings);
  portal->ui_.set_display_settings(settings);

  ESP_LOGI(kTag, "Display settings updated: bright=%d contrast=%d invert=%d off=%lus",
           settings.brightness_percent, settings.contrast_percent, settings.invert ? 1 : 0,
           static_cast<unsigned long>(settings.screen_off_seconds));

  send_json(request, "{\"ok\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_arc_colors_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  cJSON* root = nullptr;
  ESP_RETURN_ON_ERROR(receive_json_body(request, &root), kTag, "arc colors json failed");

  ArcColorScheme scheme = portal->config_store_.load_arc_color_scheme();

  const std::string soc_high = read_string_field(root, "soc_high");
  const std::string soc_mid = read_string_field(root, "soc_mid");
  const std::string soc_low = read_string_field(root, "soc_low");
  const std::string charging = read_string_field(root, "charging");
  const std::string discharging = read_string_field(root, "discharging");
  const std::string idle = read_string_field(root, "idle");
  const std::string offline = read_string_field(root, "offline");
  cJSON_Delete(root);

  if (!soc_high.empty()) scheme.soc_high = parse_hex_color(soc_high, scheme.soc_high);
  if (!soc_mid.empty()) scheme.soc_mid = parse_hex_color(soc_mid, scheme.soc_mid);
  if (!soc_low.empty()) scheme.soc_low = parse_hex_color(soc_low, scheme.soc_low);
  if (!charging.empty()) scheme.charging = parse_hex_color(charging, scheme.charging);
  if (!discharging.empty()) scheme.discharging = parse_hex_color(discharging, scheme.discharging);
  if (!idle.empty()) scheme.idle = parse_hex_color(idle, scheme.idle);
  if (!offline.empty()) scheme.offline = parse_hex_color(offline, scheme.offline);

  portal->config_store_.save_arc_color_scheme(scheme);
  portal->ui_.set_arc_color_scheme(scheme);

  ESP_LOGI(kTag, "Arc colors updated: high=0x%06lX mid=0x%06lX low=0x%06lX",
           static_cast<unsigned long>(scheme.soc_high),
           static_cast<unsigned long>(scheme.soc_mid),
           static_cast<unsigned long>(scheme.soc_low));

  send_json(request, "{\"ok\":true}");
  return ESP_OK;
}

}  // namespace printsphere
