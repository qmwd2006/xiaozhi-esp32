// 回放 scenarios/device_scenarios.tsv：固件与 wasm 用同一份契约。
// 用法：xz_scenarios [path]（默认 scenarios/device_scenarios.tsv，由 ctest 指定 cwd）
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "xz/state_machine.h"

struct Row {
    std::string scenario, event;
    int busy, paused, user_paused, user_stopped;
    int pos, queue_len, index;
    std::string url;
    std::string exp_state, exp_music, exp_url;
    int exp_uplink, exp_pos;
};

static std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(line);
    while (std::getline(is, cur, sep)) out.push_back(cur);
    return out;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "scenarios/device_scenarios.tsv";
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "打不开场景文件：%s\n", path);
        return 2;
    }
    std::string line;
    std::map<std::string, xz::DeviceStateMachine> machines;
    int total = 0, failed = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto c = Split(line, '\t');
        if (c.size() < 14) continue;
        if (c[0] == "scenario") continue;  // 表头
        Row r;
        r.scenario = c[0];
        r.event = c[1];
        r.busy = std::atoi(c[2].c_str());
        r.paused = std::atoi(c[3].c_str());
        r.user_paused = std::atoi(c[4].c_str());
        r.user_stopped = std::atoi(c[5].c_str());
        r.pos = std::atoi(c[6].c_str());
        r.url = c[7];
        r.queue_len = std::atoi(c[8].c_str());
        r.index = std::atoi(c[9].c_str());
        r.exp_state = c[10];
        r.exp_uplink = std::atoi(c[11].c_str());
        r.exp_music = c[12];
        r.exp_pos = std::atoi(c[13].c_str());
        r.exp_url = c.size() > 14 ? c[14] : "-";

        xz::DeviceEvent ev;
        ev.event = r.event;
        ev.music.busy = r.busy != 0;
        ev.music.paused = r.paused != 0;
        ev.music.user_paused = r.user_paused != 0;
        ev.music.user_stopped = r.user_stopped != 0;
        ev.music.pos = r.pos;
        ev.music.url = r.url == "-" ? "" : r.url;
        ev.music.queue_len = r.queue_len;
        ev.music.index = r.index;
        // music_request 用 url 列当「请求播放的地址」，其余场景 url 是当前曲目
        if (r.event == "music_request") ev.url = r.url == "-" ? "" : r.url;

        auto it = machines.find(r.scenario);
        if (it == machines.end()) it = machines.emplace(r.scenario, xz::DeviceStateMachine()).first;
        xz::DeviceReply got = it->second.OnEvent(ev);

        std::string music = got.music_action.empty() ? "none" : got.music_action;
        ++total;
        const std::string exp_url = r.exp_url == "-" ? "" : r.exp_url;
        const bool ok = std::string(xz::StateName(got.state)) == r.exp_state &&
                        got.uplink == (r.exp_uplink != 0) && music == r.exp_music &&
                        static_cast<int>(got.pos) == r.exp_pos && got.url == exp_url;
        if (!ok) {
            ++failed;
            std::printf("FAIL %s/%s: got state=%s uplink=%d music=%s pos=%d url=%s,"
                        " want %s/%d/%s/%d/%s\n",
                        r.scenario.c_str(), r.event.c_str(), xz::StateName(got.state),
                        got.uplink ? 1 : 0, music.c_str(), static_cast<int>(got.pos),
                        got.url.c_str(), r.exp_state.c_str(), r.exp_uplink, r.exp_music.c_str(),
                        r.exp_pos, exp_url.c_str());
        }
    }
    std::printf("设备状态场景：%d 步，%d 失败\n", total, failed);
    return failed == 0 ? 0 : 1;
}