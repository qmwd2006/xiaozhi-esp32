#include "xz/state_machine.h"

#include "xz/json_util.h"

namespace xz {

const char* StateName(State s) {
    switch (s) {
        case State::kIdle: return "idle";
        case State::kConnecting: return "connecting";
        case State::kListening: return "listening";
        case State::kSpeaking: return "speaking";
        case State::kStandby: return "standby";
    }
    return "idle";
}

const char* ModeName(ListenMode m) {
    switch (m) {
        case ListenMode::kAuto: return "auto";
        case ListenMode::kManual: return "manual";
        case ListenMode::kRealtime: return "realtime";
    }
    return "auto";
}

static ListenMode ParseMode(const std::string& s) {
    if (s == "realtime") return ListenMode::kRealtime;
    if (s == "manual") return ListenMode::kManual;
    return ListenMode::kAuto;
}

std::string DeviceReply::ToJson() const {
    std::string j = "{\"state\":" + Quote(StateName(state));
    j += ",\"mode\":" + Quote(ModeName(mode));
    j += ",\"uplink\":";
    j += uplink ? "true" : "false";
    j += ",\"ack\":";
    j += ack ? "true" : "false";
    j += ",\"music\":{\"action\":" + Quote(music_action);
    j += ",\"pos\":";
    j += std::to_string(static_cast<long long>(pos));
    j += ",\"url\":" + Quote(url);
    j += ",\"index\":";
    j += std::to_string(index);
    j += "}}";
    return j;
}

DeviceReply DeviceStateMachine::Apply(State next, const MusicSnapshot& m) {
    DeviceReply r;
    r.state = state_ = next;
    r.mode = mode_;
    r.ack = false;

    // 用户显式操作优先于对话让路：播放中视为复位
    if (m.busy) {
        user_stopped_ = false;
        user_paused_ = false;
    } else {
        if (m.user_stopped) user_stopped_ = true;
        if (m.user_paused) user_paused_ = true;
    }

    // 规则 1a：任何非 idle 状态都要把音乐真正停掉（释放 HTTP），并记住位置
    if (next != State::kIdle && m.busy && !m.paused &&
        !paused_by_dialog_) {
        paused_by_dialog_ = true;
        resume_url_ = m.url;
        resume_pos_ = m.pos;
        r.music_action = "stop";
        r.pos = m.pos;
        r.url = m.url;
    }

    // 规则 1b：回 idle 续播；用户显式停止/暂停过则不动
    if (next == State::kIdle) {
        if (paused_by_dialog_) {
            paused_by_dialog_ = false;
            const bool blocked = user_paused_ || user_stopped_ || m.user_paused || m.user_stopped;
            if (!blocked && !resume_url_.empty()) {
                r.music_action = "resume";
                r.url = resume_url_;
                r.pos = resume_pos_;
            }
            resume_url_.clear();
            resume_pos_ = 0;
        } else if (!user_stopped_ && !m.user_stopped && !user_paused_ && !m.user_paused &&
                   m.queue_len > 0 && !m.busy && m.index >= 0) {
            r.music_action = "play_index";
            r.index = m.index;
        }
    }

    // 规则 2：播报中只有 realtime 继续上传
    r.uplink = (next == State::kListening) ||
               (next == State::kSpeaking && mode_ == ListenMode::kRealtime);
    return r;
}

DeviceReply DeviceStateMachine::OnEvent(const DeviceEvent& ev) {
    State next = state_;
    bool ack = false;

    if (ev.event == "wake") {
        mode_ = DefaultMode(ev.aec);
        next = State::kConnecting;
        ack = true;
    } else if (ev.event == "ack_done") {
        next = State::kListening;
    } else if (ev.event == "tts_start") {
        next = State::kSpeaking;
    } else if (ev.event == "tts_stop") {
        next = (mode_ == ListenMode::kRealtime) ? State::kListening : State::kIdle;
    } else if (ev.event == "user_start") {
        mode_ = ListenMode::kManual;
        next = State::kListening;
    } else if (ev.event == "user_stop") {
        user_stopped_ = true;
        next = State::kIdle;
    } else if (ev.event == "idle") {
        next = State::kIdle;
    } else if (ev.event == "standby") {
        user_stopped_ = true;
        next = State::kStandby;
    } else if (ev.event == "set_mode") {
        mode_ = ParseMode(ev.mode);
        next = state_;
    }

    DeviceReply r = Apply(next, ev.music);
    r.ack = ack;
    return r;
}

}  // namespace xz