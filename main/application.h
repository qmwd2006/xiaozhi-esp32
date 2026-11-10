#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <cstdint>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "notify/notify_player.h"
#include "music/music_player.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)
#define MAIN_EVENT_STARTUP_HINT_TIMEOUT (1 << 14)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    // 播放一个音频直链（MP3），用于设备端音乐播放。仅当设备空闲时可用。
    // 若当前正在播放，则把 URL 追加到播放队列，当前曲目结束后自动续播。
    bool StartMusic(std::string audio_url);
    void StopMusic();
    // 跳过当前曲目，立即播放队列中的下一首。
    void NextMusic();
    // 返回上一首（依据播放历史）。若没有历史则忽略。
    void PrevMusic();
    // 暂停/恢复当前曲目（保留进度）。
    void PauseMusic();
    void ResumeMusic();
    // 跳转到指定秒数。
    bool SeekMusic(int seconds);
    // 立即播放（打断当前曲目并清空队列）。
    bool PlayMusicNow(std::string audio_url);
    // 把 URL 追加到播放队列（不立即打断当前曲目）。
    bool EnqueueMusic(std::string audio_url);
    // 快进/快退（秒，可为负）。
    bool SkipMusic(int delta_seconds);
    bool IsMusicPlaying() const;
    bool IsMusicPaused() const;
    // 当前播放位置/总时长（秒）。
    int MusicPositionSeconds() const;
    int MusicDurationSeconds() const;
    // 当前播放队列中剩余（含正在播放）的曲目数。
    int MusicQueueLength() const;

    // 循环模式，对齐小米音箱的「单曲循环 / 列表循环 / 顺序播放」。
    enum class MusicRepeatMode { kOff = 0, kList = 1, kOne = 2 };
    void SetMusicRepeatMode(MusicRepeatMode mode);
    MusicRepeatMode GetMusicRepeatMode() const { return music_repeat_mode_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t startup_wake_word_hint_timer_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    std::string startup_wake_word_hint_message_;
    AudioService audio_service_;
    NotifyPlayer notify_player_;
    uint32_t notification_playback_id_ = 0;
    MusicPlayer music_player_;
    uint32_t music_playback_id_ = 0;
    // 待播放队列：StartMusic 在播放中时追加，播完自动续播。
    std::deque<std::string> music_queue_;
    // 播放历史（最近播放的在队尾），用于「上一曲」。
    std::deque<std::string> music_history_;
    // 正在播放的曲目 URL（用于队列长度统计与续播去重）。
    std::string current_music_url_;
    // 播放历史上限，避免无限增长。
    static constexpr size_t kMusicHistoryMax = 20;
    // 主动停止时置位，避免 HandleMusicFinished 把它当成「自然播完」而自动续播。
    bool music_stop_requested_ = false;
    // 主动停止后待接续播放的 URL（用于 play_url 的立即切歌）。
    std::string music_replace_url_;
    // 因对话（聆听/TTS）打断而自动暂停；回到空闲时自动恢复。
    bool music_paused_by_dialog_ = false;
    // 用户显式暂停（self.music.pause）；此标记存在时不自动恢复。
    bool music_user_paused_ = false;
    // 循环模式：kOff 播完队列即停；kList 队列循环；kOne 单曲循环。
    MusicRepeatMode music_repeat_mode_ = MusicRepeatMode::kOff;
    // 最近一次自然播完的曲目 URL（单曲循环时重新入队）。
    std::string finished_music_url_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool startup_completed_ = false;
    bool pending_startup_wake_word_hint_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void ShowStartupWakeWordHint();
    void HideStartupWakeWordHint();
    void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles);
    void StopNotification();
    void HandleNotificationFinished(uint32_t playback_id, bool success);
    void HandleMusicFinished(uint32_t playback_id, bool success);
    // 尝试从 music_queue_ 取出下一首开始播放（主任务线程调用）。
    void PlayNextMusicFromQueue();
    // 实际启动指定 URL 的播放（主任务线程调用）。返回是否启动成功。
    bool StartMusicNow(const std::string& url);
    // 把当前曲目记入历史（供上一曲使用）。
    void PushCurrentToHistoryLocked();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
