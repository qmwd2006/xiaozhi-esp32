#include <string>

#include "xz/json_util.h"
#include "xz/mcp.h"
#include "xz/state_machine.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {
xz::MusicQueue g_queue;
xz::DeviceStateMachine g_sm;
std::string g_out;

#ifdef __EMSCRIPTEN__
// 这些由 sim.js 实现（浏览器 <audio> / localStorage / 页面亮度）
EM_JS(void, js_music_play, (const char* url, const char* title), {
    if (globalThis.xzPlay) globalThis.xzPlay(UTF8ToString(url), UTF8ToString(title));
});
EM_JS(void, js_music_stop, (void), { if (globalThis.xzStop) globalThis.xzStop(); });
EM_JS(void, js_music_pause, (void), { if (globalThis.xzPause) globalThis.xzPause(); });
EM_JS(void, js_music_resume, (void), { if (globalThis.xzResume) globalThis.xzResume(); });
EM_JS(void, js_music_seek, (int s), { if (globalThis.xzSeek) globalThis.xzSeek(s); });
EM_JS(void, js_music_skip, (int d), { if (globalThis.xzSkip) globalThis.xzSkip(d); });
EM_JS(int, js_music_position, (void), { return globalThis.xzPosition ? globalThis.xzPosition() : 0; });
EM_JS(int, js_music_duration, (void), { return globalThis.xzDuration ? globalThis.xzDuration() : 0; });
EM_JS(int, js_music_playing, (void), { return globalThis.xzPlaying ? (globalThis.xzPlaying() ? 1 : 0) : 0; });
EM_JS(int, js_music_paused, (void), { return globalThis.xzPaused ? (globalThis.xzPaused() ? 1 : 0) : 0; });
EM_JS(void, js_set_volume, (int v), { if (globalThis.xzVolume) globalThis.xzVolume(v); });
EM_JS(void, js_set_brightness, (int v), { if (globalThis.xzBrightness) globalThis.xzBrightness(v); });
EM_JS(void, js_standby, (void), { if (globalThis.xzStandby) globalThis.xzStandby(); });
EM_JS(void, js_reboot, (void), { if (globalThis.xzReboot) globalThis.xzReboot(); });

class WasmMusicPort : public xz::MusicPort {
 public:
    void PlayUrl(const std::string& url, const std::string& title) override {
        js_music_play(url.c_str(), title.c_str());
    }
    void Stop() override { js_music_stop(); }
    void Pause() override { js_music_pause(); }
    void Resume() override { js_music_resume(); }
    void Seek(int s) override { js_music_seek(s); }
    void Skip(int d) override { js_music_skip(d); }
    int Position() const override { return js_music_position(); }
    int Duration() const override { return js_music_duration(); }
    bool IsPlaying() const override { return js_music_playing() != 0; }
    bool IsPaused() const override { return js_music_paused() != 0; }
};

class WasmDevicePort : public xz::DevicePort {
 public:
    void SetVolume(int v) override { js_set_volume(v); }
    void SetBrightness(int v) override { js_set_brightness(v); }
    void Standby() override { js_standby(); }
    void Reboot() override { js_reboot(); }
    std::string Nvs(const std::string& action, const std::string& ns,
                    const std::string& key, const std::string& value) override {
        (void)action; (void)ns; (void)key; (void)value;
        return "{\"ok\":true}";
    }
    std::string Status() override { return "{\"ok\":true}"; }
};

WasmMusicPort g_music;
WasmDevicePort g_device;
#endif

xz::Deps MakeDeps() {
    xz::Deps d;
    d.queue = &g_queue;
#ifdef __EMSCRIPTEN__
    d.music = &g_music;
    d.device = &g_device;
#endif
    return d;
}

}  // namespace

extern "C" {

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* xz_tools_json(void) {
    g_out = xz::ToolsJson();
    return g_out.c_str();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* xz_mcp_handle(const char* payload) {
    xz::Deps deps = MakeDeps();
    g_out = xz::HandleMcp(payload ? payload : "", deps);
    return g_out.c_str();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* xz_music_state(void) {
    xz::Deps deps = MakeDeps();
    g_out = xz::MusicStatusJson(deps);
    return g_out.c_str();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void xz_music_set_index(int index) { g_queue.SetIndex(index); }

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* xz_queue_snapshot(int limit) {
    g_out = g_queue.Snapshot(limit);
    return g_out.c_str();
}

}  // extern "C"