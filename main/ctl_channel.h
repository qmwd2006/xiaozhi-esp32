#pragma once

#include <memory>
#include <string>

#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 独立控制通道：一条常驻的明文 WebSocket（与语音协议、MQTT 都无关），
// 服务端随时下发 {"type":"call","id":N,"tool":"self.music.status","args":{}}
// 设备执行后回 {"type":"result","id":N,"ok":bool,"data":"<工具原始返回>"}。
// 只做工具执行，不改设备状态、不碰音频通道。
class CtlChannel {
public:
    CtlChannel();
    ~CtlChannel();

    // 从 OTA 下发的 ctl_ws 段读配置并启动（endpoint/token/enabled）
    bool StartFromSettings();
    void Stop();
    bool IsConnected() const { return connected_; }

private:
    static void EventHandler(void* ctx, esp_event_base_t base, int32_t id, void* data);
    void OnEvent(int32_t id, void* data);
    void OnText(const char* data, int len);
    void SendText(const std::string& text);
    void ScheduleReconnect();     // 断线后自己安排一次重连（不依赖客户端的自动重连）
    void DoReconnect();
    void StartWatchdog();         // 兜底：每 20 s 检查一次，没连上就重建

    std::string endpoint_;
    std::string token_;
    esp_websocket_client_handle_t client_ = nullptr;
    bool connected_ = false;
    bool started_ = false;
    int reconnect_attempts_ = 0;
    int disconnect_ticks_ = 0;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    esp_timer_handle_t watchdog_timer_ = nullptr;
};