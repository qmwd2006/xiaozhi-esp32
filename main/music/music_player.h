#ifndef MUSIC_PLAYER_H_
#define MUSIC_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "audio_service.h"

// 通过 HTTP 拉取音频直链（MP3）并解码为 PCM 推入 AudioService 播放队列。
// 仅作为「播放外部 URL」的执行器，状态机与打断由 Application 负责。
class MusicPlayer {
public:
    using FinishedCallback = std::function<void(uint32_t playback_id, bool success)>;

    explicit MusicPlayer(AudioService& audio_service);
    ~MusicPlayer();

    // 开始播放。playback_id 用于区分多次播放，必须非 0。
    bool Start(std::string audio_url, uint32_t playback_id, FinishedCallback finished_callback);
    void Stop();

    bool IsBusy() const;
    bool IsActive(uint32_t playback_id) const;
    // 供 AudioService 的 on_playback_drained 回调转发：播放队列排空后判定是否结束。
    void OnPlaybackDrained();

private:
    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::string audio_url_;
    FinishedCallback finished_callback_;
    TaskHandle_t task_handle_ = nullptr;
    uint32_t playback_id_ = 0;
    bool active_ = false;
    bool worker_running_ = false;
    bool cancelled_ = false;
    bool http_finished_ = false;
    bool playback_drained_ = false;
    bool completion_reported_ = false;

    static void WorkerEntry(void* arg);
    void WorkerTask();
    bool IsCancelled(uint32_t playback_id) const;
    FinishedCallback CompleteLocked(uint32_t& playback_id);
};

#endif  // MUSIC_PLAYER_H_
