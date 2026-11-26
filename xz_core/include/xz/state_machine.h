// 设备状态机：固件 application.cc 的规则搬到这里，固件与 wasm 共用同一份实现。
// 纯逻辑，不碰 IO；所有外设动作以 action 形式返回，由端口（audio_service / <audio>）执行。
#pragma once
#include <string>

namespace xz {

enum class State { kIdle = 0, kConnecting, kListening, kSpeaking, kStandby };
enum class ListenMode { kAuto = 0, kManual, kRealtime };

const char* StateName(State s);
const char* ModeName(ListenMode m);

// 音乐侧的真实状态（由端口读出来喂进来）
struct MusicSnapshot {
    bool busy = false;         // 正在输出（有 url 且未暂停）
    bool paused = false;       // 有 url 但暂停
    bool user_paused = false;  // 用户显式暂停过
    bool user_stopped = false; // 用户显式停止过
    double pos = 0;            // 已播秒数
    std::string url;           // 当前曲目
    int index = -1;            // 队列索引
    int queue_len = 0;
};

struct DeviceEvent {
    std::string event;   // wake|ack_done|tts_start|tts_stop|user_start|user_stop|idle|standby|set_mode
    std::string mode;    // set_mode 用：auto|manual|realtime
    bool aec = true;     // 影响默认模式：AEC 开 → realtime
    MusicSnapshot music;
};

struct DeviceReply {
    State state = State::kIdle;
    ListenMode mode = ListenMode::kRealtime;
    bool uplink = false;   // 是否上传麦克风（播报中只有 realtime 才上传）
    bool ack = false;      // 是否需要先回一声「在」
    std::string music_action;  // "" | stop | resume | play_index
    double pos = 0;
    std::string url;
    int index = -1;
    std::string ToJson() const;
};

// 与固件一致的三处规则：
//  1) 离开 idle 时音乐真正 Stop（释放 HTTP）并记住位置，回 idle 续播；用户显式停止/暂停则不续
//  2) 播报中是否继续上传麦克风：realtime 上传，auto/manual 停（Speaking 分支）
//  3) 默认收音模式：AEC 开 → realtime，否则 auto
class DeviceStateMachine {
 public:
    DeviceReply OnEvent(const DeviceEvent& ev);
    State state() const { return state_; }
    ListenMode mode() const { return mode_; }
    bool music_paused_by_dialog() const { return paused_by_dialog_; }
    bool user_stopped() const { return user_stopped_; }
    static ListenMode DefaultMode(bool aec) {
        return aec ? ListenMode::kRealtime : ListenMode::kAuto;
    }

 private:
    DeviceReply Apply(State next, const MusicSnapshot& m);
    State state_ = State::kIdle;
    ListenMode mode_ = ListenMode::kRealtime;
    bool paused_by_dialog_ = false;
    bool user_paused_ = false;
    bool user_stopped_ = false;
    std::string resume_url_;
    double resume_pos_ = 0;
};

}  // namespace xz