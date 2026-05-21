#include "printsphere/msa2_status_client.hpp"

#include <cstring>
#include <vector>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "status.client";
constexpr size_t kMaxResponseBytes = 32U * 1024U;
constexpr TickType_t kPollInterval = pdMS_TO_TICKS(5000);
constexpr TickType_t kPollIntervalLowPower = pdMS_TO_TICKS(30000);

void parse_field_object(const char* key, const cJSON* item, StatusValue* out) {
  if (key == nullptr || item == nullptr || out == nullptr || !cJSON_IsObject(item)) {
    return;
  }

  const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(item, "value");
  const cJSON* desc_item = cJSON_GetObjectItemCaseSensitive(item, "desc");
  if (cJSON_IsString(desc_item) && desc_item->valuestring != nullptr) {
    out->desc = desc_item->valuestring;
  }

  if (cJSON_IsNull(value_item)) {
    out->is_null = true;
    return;
  }

  out->is_null = false;
  if (cJSON_IsBool(value_item)) {
    out->is_bool = true;
    out->bool_value = cJSON_IsTrue(value_item);
    return;
  }
  if (cJSON_IsNumber(value_item)) {
    out->has_number = true;
    out->number_value = value_item->valuedouble;
    return;
  }
  if (cJSON_IsString(value_item) && value_item->valuestring != nullptr) {
    out->has_string = true;
    out->string_value = value_item->valuestring;
  }
}

bool parse_status_json(const std::string& body, Msa2Snapshot* snapshot) {
  if (snapshot == nullptr) {
    return false;
  }

  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    return false;
  }

  snapshot->fields.clear();
  const cJSON* child = root->child;
  while (child != nullptr) {
    if (cJSON_IsObject(child) && child->string != nullptr) {
      StatusValue value;
      parse_field_object(child->string, child, &value);
      snapshot->fields.emplace(child->string, std::move(value));
    }
    child = child->next;
  }

  cJSON_Delete(root);
  snapshot->raw_json = body;
  return true;
}

}  // namespace

esp_err_t Msa2StatusClient::start() {
  if (task_running_) {
    return ESP_OK;
  }

  const BaseType_t created =
      xTaskCreate(&Msa2StatusClient::task_entry, "msa2_status", 8192, this, 4, nullptr);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  task_running_ = true;
  return ESP_OK;
}

void Msa2StatusClient::configure(const std::string& status_url) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_url_ = status_url.empty() ? "http://node.lan/msa2/status" : status_url;
}

void Msa2StatusClient::set_network_ready(bool ready) {
  std::lock_guard<std::mutex> lock(mutex_);
  network_ready_ = ready;
}

void Msa2StatusClient::set_low_power_mode(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  low_power_mode_ = enabled;
}

Msa2Snapshot Msa2StatusClient::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

Msa2Snapshot Msa2StatusClient::refreshed_snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void Msa2StatusClient::task_entry(void* arg) {
  static_cast<Msa2StatusClient*>(arg)->task_loop();
}

void Msa2StatusClient::task_loop() {
  while (true) {
    bool ready = false;
    bool low_power = false;
    std::string url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready = network_ready_;
      low_power = low_power_mode_;
      url = status_url_;
    }

    if (ready && !url.empty()) {
      Msa2Snapshot fetched;
      if (fetch_once(&fetched)) {
        fetched.connected = true;
        fetched.detail = "Connected";
        fetched.updated_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = std::move(fetched);
      } else {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.connected = false;
        snapshot_.detail = "Status fetch failed";
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot_.connected = false;
      snapshot_.detail = ready ? "No status URL configured" : "Waiting for Wi-Fi";
    }

    vTaskDelay(low_power ? kPollIntervalLowPower : kPollInterval);
  }
}

bool Msa2StatusClient::fetch_once(Msa2Snapshot* out) {
  if (out == nullptr) {
    return false;
  }

  std::string url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    url = status_url_;
  }

  if (url.empty()) {
    return false;
  }

  std::vector<char> buffer;
  buffer.reserve(4096);

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 8000;
  config.disable_auto_redirect = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    return false;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return false;
  }

  const int content_length = esp_http_client_fetch_headers(client);
  if (content_length > static_cast<int>(kMaxResponseBytes)) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  int total = 0;
  char chunk[512] = {};
  while (total < static_cast<int>(kMaxResponseBytes)) {
    const int read = esp_http_client_read(client, chunk, sizeof(chunk));
    if (read <= 0) {
      break;
    }
    buffer.insert(buffer.end(), chunk, chunk + read);
    total += read;
    if (content_length > 0 && total >= content_length) {
      break;
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (buffer.empty()) {
    return false;
  }

  buffer.push_back('\0');
  return parse_status_json(buffer.data(), out);
}

}  // namespace printsphere
