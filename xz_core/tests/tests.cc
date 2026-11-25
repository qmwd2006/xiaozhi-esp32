// 原生回归测试：队列语义 / JSON 转义 / MCP 分发（不依赖 ESP-IDF）
#include <cassert>
#include <cstdio>
#include <string>

#include "xz/json_util.h"
#include "xz/mcp.h"

using namespace xz;

class FakeMusic : public MusicPort {
 public:
    void PlayUrl(const std::string& url, const std::string& title) override {
        url_ = url; title_ = title; playing_ = true; paused_ = false; pos_ = 0;
    }
    void Stop() override { playing_ = false; paused_ = false; pos_ = 0; }
    void Pause() override { paused_ = true; }
    void Resume() override { paused_ = false; playing_ = !url_.empty(); }
    void Seek(int s) override { pos_ = s; }
    void Skip(int d) override { pos_ += d; if (pos_ < 0) pos_ = 0; }
    int Position() const override { return pos_; }
    int Duration() const override { return 200; }
    bool IsPlaying() const override { return playing_ && !paused_; }
    bool IsPaused() const override { return paused_; }
    const std::string& url() const { return url_; }

 private:
    std::string url_, title_;
    bool playing_ = false, paused_ = false;
    int pos_ = 0;
};

class FakeDevice : public DevicePort {
 public:
    void SetVolume(int v) override { volume = v; }
    void SetBrightness(int v) override { brightness = v; }
    void Standby() override { standby = true; }
    std::string Nvs(const std::string&, const std::string&, const std::string&,
                    const std::string&) override { return "{\"ok\":true}"; }
    std::string Status() override { return "{\"ok\":true,\"volume\":70}"; }
    int volume = 70, brightness = 80;
    bool standby = false;
};

static std::string Call(Deps& deps, const std::string& tool, const std::string& args) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":"
                          "{\"name\":\"" + tool + "\",\"arguments\":" + args + "}}";
    std::string res = HandleMcp(payload, deps);
    assert(res.find("\"id\":7") != std::string::npos);
    return res;
}

int main() {
    // JSON 转义（和固件一样：只转义，不加引号）
    assert(JsonEscape("a\"b") == "a\\\"b");
    assert(JsonEscape("x\\y\n") == "x\\\\y\\n");
    assert(Quote("在") == "\"在\"");

    // 扁平参数解析
    Args a;
    assert(a.Parse("{\"url\":\"http://x/a.mp3\",\"volume\":42,\"ok\":true}"));
    assert(a.Str("url") == "http://x/a.mp3");
    assert(a.Int("volume") == 42);
    assert(a.Bool("ok"));

    // 队列语义
    MusicQueue q;
    assert(q.Length() == 0 && q.Index() == -1);
    q.Set({"u1", "u2", "u3"}, {"t1", "t2", "t3"}, 0);
    assert(q.Length() == 3 && q.CurrentUrl() == "u1");
    assert(q.Next() && q.CurrentUrl() == "u2");
    assert(q.Next() && q.CurrentUrl() == "u3");
    assert(q.Next() && q.CurrentUrl() == "u1");          // 列表循环
    assert(q.Prev() && q.CurrentUrl() == "u3");
    assert(!q.PlayIndex(9));
    assert(q.PlayIndex(1) && q.CurrentUrl() == "u2");
    q.Enqueue("u4", "t4");
    assert(q.Length() == 4);
    std::string snap = q.Snapshot(10);
    assert(snap.find("\"length\":4") != std::string::npos);
    assert(snap.find("\"current_index\":1") != std::string::npos);
    assert(snap.find("\"url\":\"u4\"") != std::string::npos);
    q.Clear();
    assert(q.Length() == 0 && q.Index() == -1);

    FakeMusic music;
    FakeDevice device;
    Deps deps;
    deps.queue = &q;
    deps.music = &music;
    deps.device = &device;

    // initialize / tools/list
    std::string init = HandleMcp("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"initialize\"}", deps);
    assert(init.find("\"id\":\"abc\"") != std::string::npos);
    assert(init.find("protocolVersion") != std::string::npos);
    std::string list = HandleMcp("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", deps);
    assert(list.find("self.music.seek") != std::string::npos);
    assert(list.find("self.music.set_queue") != std::string::npos);
    assert(list.find("self.audio_speaker.set_volume") != std::string::npos);

    // 下发队列 → 起播第一首
    Call(deps, "self.music.set_queue",
         "{\"urls\":\"a\\nb\\nc\",\"titles\":\"A\\nB\\nC\",\"start_index\":0}");
    assert(q.Length() == 3 && music.url() == "a" && music.IsPlaying());

    // 下一首 / 上一首
    Call(deps, "self.music.next", "{}");
    assert(q.Index() == 1 && music.url() == "b");
    Call(deps, "self.music.prev", "{}");
    assert(q.Index() == 0 && music.url() == "a");

    // seek / skip / 单位是秒
    Call(deps, "self.music.seek", "{\"position\":30}");
    assert(music.Position() == 30);
    Call(deps, "self.music.skip", "{\"seconds\":-10}");
    assert(music.Position() == 20);

    // 停止 = 回到开头（与固件 stop 语义一致）
    Call(deps, "self.music.stop", "{}");
    assert(!music.IsPlaying() && music.Position() == 0 && q.Length() == 3);

    // 音量/亮度/待机
    Call(deps, "self.audio_speaker.set_volume", "{\"volume\":42}");
    assert(device.volume == 42);
    Call(deps, "self.screen.set_brightness", "{\"brightness\":33}");
    assert(device.brightness == 33);
    Call(deps, "self.device.standby", "{}");
    assert(device.standby);

    // 下划线命名也要认（服务端可能发 self_music_status）
    std::string text = Call(deps, "self_music_status", "{}");
    assert(text.find("queue_length") != std::string::npos);

    // status 里必须有 position_unit=s
    std::string st = Call(deps, "self.music.status", "{}");
    assert(st.find("position_unit") != std::string::npos);

    // 队列 JSON（供模拟器/固件共用）
    std::string qj = Call(deps, "self.music.queue", "{\"limit\":2}");
    assert(qj.find("length") != std::string::npos);

    // 未知工具给出明确错误
    std::string bad = Call(deps, "self.nope", "{}");
    assert(bad.find("未知工具") != std::string::npos);

    printf("xz_core 原生测试全部通过（%d 项断言组）\n", 8);
    return 0;
}