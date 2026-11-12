#include "mqtt_control.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdlib>
#include <cstring>

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "MqttControl"

static constexpr int kReconnectIntervalMs = 60000;

MqttControl::MqttControl() {
    esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                auto* self = static_cast<MqttControl*>(arg);
                Application::GetInstance().Schedule([self]() { self->ConnectAndSubscribe(); });
            },
        .arg = this,
    };
    esp_timer_create(&timer_args, &reconnect_timer_);
}

MqttControl::~MqttControl() {
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
    std::shared_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mqtt = std::move(mqtt_);
    }
    if (mqtt != nullptr) {
        mqtt->Disconnect();
    }
}

bool MqttControl::LoadConfig(Config& out) const {
    Settings settings("mqttctl", false);
    out.endpoint = settings.GetString("endpoint");
    out.client_id = settings.GetString("client_id");
    out.username = settings.GetString("username");
    out.password = settings.GetString("password");
    out.publish_topic = settings.GetString("publish_topic");
    out.subscribe_topic = settings.GetString("subscribe_topic");
    out.keepalive = settings.GetInt("keepalive", 240);
    return !out.endpoint.empty() && !out.subscribe_topic.empty();
}

bool MqttControl::Start() {
    if (!LoadConfig(config_)) {
        ESP_LOGW(TAG, "No mqtt_control config, skip");
        return false;
    }
    started_ = true;
    ESP_LOGI(TAG, "Starting: endpoint=%s client_id=%s sub=%s pub=%s",
             config_.endpoint.c_str(), config_.client_id.c_str(),
             config_.subscribe_topic.c_str(), config_.publish_topic.c_str());
    return ConnectAndSubscribe();
}

void MqttControl::Stop() {
    started_ = false;
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    // 先把指针移出临界区，再在锁外 Disconnect：esp-mqtt 的 Disconnect() 会等待
    // 其内部任务退出，而该任务的回调路径可能与我们争用 mutex_，持锁调用会死锁。
    std::shared_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mqtt = std::move(mqtt_);
        subscribed_ = false;
    }
    if (mqtt != nullptr) {
        mqtt->Disconnect();
    }
}

bool MqttControl::ConnectAndSubscribe() {
    if (!started_) {
        return false;
    }

    std::string host = config_.endpoint;
    int port = 1883;
    auto pos = host.find(':');
    if (pos != std::string::npos) {
        port = std::atoi(host.substr(pos + 1).c_str());
        host = host.substr(0, pos);
    }

    // 先在锁外断开旧连接（Disconnect 会等 esp-mqtt 任务退出，持锁易死锁）。
    std::shared_ptr<Mqtt> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(mqtt_);
    }
    if (old != nullptr) {
        old->Disconnect();
        old.reset();
    }

    auto network = Board::GetInstance().GetNetwork();
    auto mqtt = network->CreateMqtt(0);
    if (mqtt == nullptr) {
        ESP_LOGE(TAG, "CreateMqtt failed");
        return false;
    }
    mqtt->SetKeepAlive(config_.keepalive);
    // 注意：OnConnected 可能在 Connect() 返回前就被 esp-mqtt 任务回调，
    // 此时 MqttControl::mqtt_ 尚未赋值，因此订阅用局部 mqtt 指针。
    Mqtt* mqtt_raw = mqtt.get();
    mqtt->OnConnected([this, mqtt_raw]() {
        ESP_LOGI(TAG, "Connected, subscribing %s", config_.subscribe_topic.c_str());
        if (mqtt_raw->Subscribe(config_.subscribe_topic, 0)) {
            subscribed_ = true;
        } else {
            ESP_LOGE(TAG, "Subscribe failed");
        }
    });
    mqtt->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Disconnected, reconnect in %d s", kReconnectIntervalMs / 1000);
        subscribed_ = false;
        if (started_ && reconnect_timer_ != nullptr) {
            esp_timer_stop(reconnect_timer_);
            esp_timer_start_once(reconnect_timer_, (uint64_t)kReconnectIntervalMs * 1000);
        }
    });
    mqtt->OnMessage([this](const std::string& topic, const std::string& payload) {
        HandleMessage(topic, payload);
    });

    auto result = mqtt->Connect(host, port, config_.client_id, config_.username,
                                config_.password);
    if (!result) {
        ESP_LOGE(TAG, "Connect failed: %s", result.error().ToString().c_str());
        if (started_ && reconnect_timer_ != nullptr) {
            esp_timer_stop(reconnect_timer_);
            esp_timer_start_once(reconnect_timer_, (uint64_t)kReconnectIntervalMs * 1000);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        mqtt_ = std::move(mqtt);
    }
    // Connect 返回时通常已触发 OnConnected 并完成订阅；保险起见再补一次。
    if (!subscribed_ && mqtt_raw->Subscribe(config_.subscribe_topic, 0)) {
        subscribed_ = true;
    }
    ESP_LOGI(TAG, "MQTT control channel ready (subscribed=%d)", subscribed_);
    return true;
}

void MqttControl::HandleMessage(const std::string& topic, const std::string& payload) {
    ESP_LOGI(TAG, "RX [%s]: %s", topic.c_str(), payload.c_str());
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Invalid JSON");
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    cJSON* action = cJSON_GetObjectItem(root, "action");

    // 形式 1：MCP JSON-RPC（{"type":"mcp","payload":{...}}），复用设备工具。
    if (cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        cJSON* mcp_payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(mcp_payload)) {
            char* text = cJSON_PrintUnformatted(mcp_payload);
            if (text != nullptr) {
                std::string mcp_msg(text);
                cJSON_free(text);
                MqttControl* self = this;
                ESP_LOGI(TAG, "Dispatching MCP to main task: %s", mcp_msg.c_str());
                Application::GetInstance().Schedule([self, mcp_msg]() {
                    McpServer::GetInstance().ParseMessage(
                        mcp_msg, [self](const std::string& response) {
                            self->SendReceipt(response);
                        });
                });
            }
        }
        cJSON_Delete(root);
        return;
    }

    // 形式 2：便捷指令 {"action":"play_music","url":...} / {"action":"listen"} ...
    if (cJSON_IsString(action)) {
        std::string act = action->valuestring;
        cJSON* url_item = cJSON_GetObjectItem(root, "url");
        std::string url = cJSON_IsString(url_item) ? url_item->valuestring : "";
        auto& app = Application::GetInstance();
        if (act == "play_music") {
            if (!url.empty()) {
                app.Schedule([&app, url]() { app.PlayMusicNow(url); });
            }
        } else if (act == "listen") {
            app.Schedule([&app]() { app.StartListening(); });
        } else if (act == "stop") {
            app.Schedule([&app]() { app.StopListening(); });
        } else if (act == "reboot") {
            app.Schedule([&app]() { app.Reboot(); });
        } else {
            ESP_LOGW(TAG, "Unknown action: %s", act.c_str());
        }
    }
    cJSON_Delete(root);
}

void MqttControl::SendReceipt(const std::string& payload) {
    ESP_LOGI(TAG, "SendReceipt called, len=%u", static_cast<unsigned>(payload.size()));
    if (config_.publish_topic.empty()) {
        ESP_LOGW(TAG, "SendReceipt: publish_topic empty");
        return;
    }
    // 锁内只取引用，锁外 Publish：esp_mqtt_client_publish 在 outbox 满时可能阻塞
    // 等待其事件循环，若持锁等待，而事件循环回调又需要该锁，会死锁。
    std::shared_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mqtt = mqtt_;
    }
    if (mqtt != nullptr) {
        bool ok = mqtt->Publish(config_.publish_topic, payload, 0);
        ESP_LOGI(TAG, "SendReceipt publish %s (connected=%d)", ok ? "ok" : "FAIL",
                 mqtt->IsConnected());
    } else {
        ESP_LOGW(TAG, "SendReceipt: mqtt_ null");
    }
}
