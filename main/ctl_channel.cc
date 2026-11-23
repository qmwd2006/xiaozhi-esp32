#include "ctl_channel.h"

#include <cstring>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_wifi.h>

#include "mcp_server.h"
#include "settings.h"

static const char* TAG = "CtlChannel";

CtlChannel::CtlChannel() = default;
CtlChannel::~CtlChannel() { Stop(); }

bool CtlChannel::StartFromSettings() {
    // OTA 的 ctl_ws 段会写进同一个 Settings 命名空间（mqttctl）
    Settings settings("mqttctl", false);
    if (!settings.GetBool("enabled", false)) {
        ESP_LOGI(TAG, "ctl_ws 未启用（enabled=false）");
        return false;
    }
    endpoint_ = settings.GetString("endpoint", "");
    token_ = settings.GetString("token", "");
    if (endpoint_.empty()) {
        ESP_LOGW(TAG, "ctl_ws 缺少 endpoint");
        return false;
    }

    // 拼 mac 与 token：ws://host:port/device-ctl?mac=xxxx&token=yyyy
    char mac_str[13] = {0};
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    std::string uri = endpoint_;
    uri += (uri.find('?') == std::string::npos ? "?" : "&");
    uri += "mac=";
    uri += mac_str;
    if (!token_.empty()) {
        uri += "&token=";
        uri += token_;
    }

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri.c_str();
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = 8000;
    cfg.disable_auto_reconnect = false;
    cfg.buffer_size = 2048;
    cfg.task_stack = 6144;

    client_ = esp_websocket_client_init(&cfg);
    if (client_ == nullptr) {
        ESP_LOGE(TAG, "init 失败");
        return false;
    }
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, EventHandler, this);
    if (esp_websocket_client_start(client_) != ESP_OK) {
        ESP_LOGE(TAG, "start 失败");
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        return false;
    }
    started_ = true;
    ESP_LOGI(TAG, "控制通道启动：%s（mac=%s）", endpoint_.c_str(), mac_str);
    return true;
}

void CtlChannel::Stop() {
    if (client_ != nullptr) {
        esp_websocket_client_stop(client_);
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
    connected_ = false;
    started_ = false;
}

void CtlChannel::EventHandler(void* ctx, esp_event_base_t base, int32_t id, void* data) {
    static_cast<CtlChannel*>(ctx)->OnEvent(id, data);
}

void CtlChannel::OnEvent(int32_t id, void* data) {
    auto* ev = static_cast<esp_websocket_event_data_t*>(data);
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            connected_ = true;
            ESP_LOGI(TAG, "已连接");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            connected_ = false;
            ESP_LOGW(TAG, "断开，等待自动重连");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x1 || ev->op_code == 0x0) {   // 文本帧（含分片）
                if (ev->data_len > 0) {
                    OnText(ev->data_ptr, ev->data_len);
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "错误");
            break;
        default:
            break;
    }
}

void CtlChannel::SendText(const std::string& text) {
    if (client_ == nullptr || !connected_) {
        return;
    }
    esp_websocket_client_send_text(client_, text.c_str(), static_cast<int>(text.size()),
                                   pdMS_TO_TICKS(3000));
}

void CtlChannel::OnText(const char* data, int len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        return;
    }
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "call") == 0) {
        cJSON* id_item = cJSON_GetObjectItem(root, "id");
        cJSON* tool = cJSON_GetObjectItem(root, "tool");
        cJSON* args = cJSON_GetObjectItem(root, "args");
        int id_num = cJSON_IsNumber(id_item) ? id_item->valueint : 0;
        if (cJSON_IsString(tool)) {
            std::string args_json = "{}";
            if (args != nullptr) {
                char* dumped = cJSON_PrintUnformatted(args);
                if (dumped != nullptr) {
                    args_json = dumped;
                    cJSON_free(dumped);
                }
            }
            ESP_LOGI(TAG, "<< call#%d %s", id_num, tool->valuestring);
            // 工具调用会被投递到主任务异步执行，回执稍后才产生，
            // 所以这里不能同步取返回值，必须把回执通过 sender 回传。
            McpServer::GetInstance().HandleExternalCallAsync(
                tool->valuestring, args_json, id_num,
                [this, id_num](const std::string& payload) {
                    cJSON* reply = cJSON_CreateObject();
                    cJSON_AddStringToObject(reply, "type", "result");
                    cJSON_AddNumberToObject(reply, "id", id_num);
                    cJSON_AddBoolToObject(reply, "ok", true);
                    cJSON_AddStringToObject(reply, "data", payload.c_str());
                    char* out = cJSON_PrintUnformatted(reply);
                    if (out != nullptr) {
                        SendText(out);
                        cJSON_free(out);
                    }
                    cJSON_Delete(reply);
                });
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "ping") == 0) {
        SendText("{\"type\":\"pong\"}");
    }
    cJSON_Delete(root);
}
