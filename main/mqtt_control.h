#ifndef _MQTT_CONTROL_H_
#define _MQTT_CONTROL_H_

#include <memory>
#include <mutex>
#include <string>

#include <esp_timer.h>
#include <mqtt.h>

// 旁路 MQTT 控制客户端。
//
// 与 protocol_（音频/对话通道）完全独立：固件仍按 OTA 下发的 websocket/mqtt
// 配置走原有语音协议，这里额外常驻一个 MQTT 连接，仅用于接收远程控制指令
// （远程播放音乐 / 唤醒聆听 / 停止聆听等），从而让 idle 状态（不连语音服务端）
// 的设备也能被服务端秒级触达。
//
// 仅在 OTA 响应携带可选 "mqtt_control" 段时启动；官方云端不返回该段，因此
// 不影响官方服务下的行为。
class MqttControl {
public:
    MqttControl();
    ~MqttControl();

    // 从 Settings("mqttctl") 读取配置并连接/订阅；配置缺失则直接返回 false。
    bool Start();

    void Stop();

private:
    struct Config {
        std::string endpoint;
        std::string client_id;
        std::string username;
        std::string password;
        std::string publish_topic;
        std::string subscribe_topic;
        int keepalive = 240;
    };

    bool LoadConfig(Config& out) const;
    bool ConnectAndSubscribe();
    void HandleMessage(const std::string& topic, const std::string& payload);
    void SendReceipt(const std::string& payload);

    Config config_;
    std::unique_ptr<Mqtt> mqtt_;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    std::mutex mutex_;
    bool started_ = false;
    bool subscribed_ = false;
};

#endif  // _MQTT_CONTROL_H_
