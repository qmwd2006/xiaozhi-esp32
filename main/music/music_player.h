#ifndef MUSIC_PLAYER_H_
#define MUSIC_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "audio_service.h"

// 通过 HTTP 拉取音频直链（MP3）并解码为 PCM 推入 AudioService 播放队列。
// 仅作为「播放外部 URL」的执行器，状态机与打断由 Application 负责。
//
// 支持完整的播放器操作：播放、暂停、恢复、跳转（HTTP Range）、
// 停止，以及基于字节/码率的进度与总时长查询。
class MusicPlayer {
public:
    using FinishedCallback = std::function<void(uint32_t playback_id, bool success)>;

    explicit MusicPlayer(AudioService& audio_service);
    ~MusicPlayer();

    // 开始播放。playback_id 用于区分多次播放，必须非 0。
    // 若已有 worker 在运行，本调用会中止当前曲目并把本次请求排队，
    // 由旧 worker 退出后自动开始新曲目（避免「停止后立刻开始」的竞态）。
    // 只有 URL 非法或任务创建失败才返回 false。
    bool Start(std::string audio_url, uint32_t playback_id, FinishedCallback finished_callback);
    // 停止播放并丢弃任何已排队的待播放请求。
    void Stop();

    // 暂停/恢复：暂停时停止向播放队列推帧，但保留 HTTP 连接与解码状态。
    void Pause();
    void Resume();
    bool IsPaused() const;

    // 跳转到指定秒数（尽力而为：按码率估算字节偏移并重新发起 Range 请求）。
    // 仅在正在播放时有效。
    bool Seek(int seconds);

    bool IsBusy() const;
    bool IsActive(uint32_t playback_id = 0) const;

    // 当前播放位置与总时长（秒）。未知时返回 0。
    int PositionSeconds() const;
    int DurationSeconds() const;

    // 供 AudioService 的 on_playback_drained 回调转发：播放队列排空后判定是否结束。
    void OnPlaybackDrained();

private:
    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::condition_variable pause_cv_;

    std::string audio_url_;
    FinishedCallback finished_callback_;
    TaskHandle_t task_handle_ = nullptr;
    uint32_t playback_id_ = 0;
    bool active_ = false;
    bool worker_running_ = false;
    bool cancelled_ = false;
    bool paused_ = false;
    bool http_finished_ = false;
    bool playback_drained_ = false;
    bool completion_reported_ = false;

    // 等待旧 worker 退出后再播放的曲目（换歌时使用，避免 Start 失败）。
    std::string pending_url_;
    FinishedCallback pending_callback_;
    uint32_t pending_playback_id_ = 0;

    // 进度跟踪（字节与码率）。
    int64_t total_bytes_ = 0;   // 文件总字节（来自 Content-Range/Length），未知为 0
    int64_t current_byte_ = 0;  // 已消费的绝对字节偏移
    int bitrate_bps_ = 0;       // 解码器报告的码率，未知为 0
    // 待执行的跳转目标字节；<0 表示无。
    int64_t seek_target_byte_ = -1;
    bool seek_pending_ = false;
    // 启动早期（码率未知）就请求的跳转秒数；拿到码率后再换算。
    int pending_seek_seconds_ = -1;
    // 还需丢弃的采样数（跨解码块累计，直到越过跳转点）。
    int64_t skip_remaining_samples_ = 0;

    static void WorkerEntry(void* arg);
    void WorkerTask();
    // 启动一个新 worker；调用方需已持有 mutex_ 且已写入播放参数。
    bool StartWorkerLocked();
    // 若处于暂停状态则阻塞，直到恢复/取消/停止。
    void WaitIfPaused();
    bool IsCancelled(uint32_t playback_id) const;
    FinishedCallback CompleteLocked(uint32_t& playback_id);
};

#endif  // MUSIC_PLAYER_H_
