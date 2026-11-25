#pragma once
#include <string>

namespace xz {

// 设备侧播放能力（固件由 MusicPlayer 实现；wasm 由浏览器 <audio> 实现）
class MusicPort {
 public:
    virtual ~MusicPort() = default;
    virtual void PlayUrl(const std::string& url, const std::string& title) = 0;
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void Seek(int seconds) = 0;
    virtual void Skip(int delta_seconds) = 0;
    virtual int Position() const = 0;
    virtual int Duration() const = 0;
    virtual bool IsPlaying() const = 0;
    virtual bool IsPaused() const = 0;
};

// 其它外设（音量/亮度/待机/NVS/摄像头）
class DevicePort {
 public:
    virtual ~DevicePort() = default;
    virtual void SetVolume(int volume) = 0;
    virtual void SetBrightness(int brightness) = 0;
    virtual void SetTheme(const std::string& theme) { (void)theme; }
    virtual void Standby() = 0;
    virtual void Reboot() { }
    virtual std::string Nvs(const std::string& action, const std::string& ns,
                            const std::string& key, const std::string& value) = 0;
    virtual std::string Status() = 0;          // get_device_status 的 JSON
    virtual std::string CameraPhoto() { return "{\"ok\":false,\"error\":\"no camera\"}"; }
};

}  // namespace xz