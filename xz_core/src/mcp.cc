#include "xz/mcp.h"

#include <cstdlib>
#include <vector>

#include "xz/json_util.h"

namespace xz {
namespace {

struct ToolDef {
    const char* name;
    const char* desc;
    const char* props;  // JSON 片段，直接拼进 inputSchema.properties
    const char* required;
};

const ToolDef kTools[] = {
    {"self.get_device_status", "Get device status (volume, screen, music playing state).",
     "{}", ""},
    {"self.audio_speaker.set_volume", "Set output volume (0-100).",
     "{\"volume\":{\"type\":\"integer\"}}", "\"volume\""},
    {"self.screen.set_brightness", "Set screen brightness (0-100).",
     "{\"brightness\":{\"type\":\"integer\"}}", "\"brightness\""},
    {"self.screen.set_theme", "Set screen theme.",
     "{\"theme\":{\"type\":\"string\"}}", "\"theme\""},
    {"self.device.standby", "Enter standby (end the conversation).", "{}", ""},
    {"self.device.reboot", "Reboot the device.", "{}", ""},
    {"self.device.nvs", "Read or write NVS keys.",
     "{\"action\":{\"type\":\"string\"},\"namespace\":{\"type\":\"string\"},"
     "\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}}", "\"action\""},
    {"self.camera.take_photo", "Take a photo.", "{}", ""},
    {"self.music.play_url", "Play an audio URL now.",
     "{\"url\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"}}", "\"url\""},
    {"self.music.set_queue", "Replace the playlist with newline separated URLs.",
     "{\"urls\":{\"type\":\"string\"},\"titles\":{\"type\":\"string\"},"
     "\"start_index\":{\"type\":\"integer\"}}", "\"urls\""},
    {"self.music.enqueue_url", "Append one URL to the playlist.",
     "{\"url\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"}}", "\"url\""},
    {"self.music.play_index", "Play the queue item at index.", "{\"index\":{\"type\":\"integer\"}}",
     "\"index\""},
    {"self.music.next", "Next track.", "{}", ""},
    {"self.music.prev", "Previous track.", "{}", ""},
    {"self.music.pause", "Pause playback (keep position).", "{}", ""},
    {"self.music.resume", "Resume playback.", "{}", ""},
    {"self.music.stop", "Stop playback and rewind to 0 (playlist kept).", "{}", ""},
    {"self.music.seek", "Jump to a position in seconds.",
     "{\"position\":{\"type\":\"integer\"}}", "\"position\""},
    {"self.music.skip", "Fast-forward/rewind by seconds.",
     "{\"seconds\":{\"type\":\"integer\"}}", "\"seconds\""},
    {"self.music.set_repeat", "Set repeat mode: off, list, one.",
     "{\"mode\":{\"type\":\"string\"}}", "\"mode\""},
    {"self.music.status", "Current playback status.", "{}", ""},
    {"self.music.queue", "Read the playlist.", "{\"limit\":{\"type\":\"integer\"}}", ""},
    {"self.music.clear_queue", "Clear the playlist and stop.", "{}", ""},
};

constexpr int kToolCount = static_cast<int>(sizeof(kTools) / sizeof(kTools[0]));

int ToolCount() { return kToolCount; }

std::string NormalizeName(const std::string& name) {
    if (name.rfind("self_", 0) != 0) return name;
    std::string out = name;
    for (char& c : out) {
        if (c == '_') c = '.';
    }
    return out;
}

std::string TextResult(const std::string& text) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + Quote(text) + "}]}";
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

std::string ToolsJson() {
    std::string out = "[";
    for (int i = 0; i < kToolCount; ++i) {
        if (i) out += ',';
        out += "{\"name\":" + Quote(kTools[i].name) +
               ",\"description\":" + Quote(kTools[i].desc) +
               ",\"inputSchema\":{\"type\":\"object\",\"properties\":" +
               std::string(kTools[i].props[0] ? kTools[i].props : "{}");
        if (kTools[i].required[0]) {
            out += ",\"required\":[" + std::string(kTools[i].required) + "]";
        }
        out += "}}";
    }
    out += "]";
    return out;
}

std::string MusicStatusJson(Deps& deps) {
    MusicQueue* q = deps.queue;
    int len = q ? q->Length() : 0;
    std::string out = "{";
    if (deps.music) {
        out += "\"playing\":" + std::string(deps.music->IsPlaying() ? "true" : "false");
        out += ",\"paused\":" + std::string(deps.music->IsPaused() ? "true" : "false");
        out += ",\"position\":" + std::to_string(deps.music->Position());
        out += ",\"duration\":" + std::to_string(deps.music->Duration());
    } else {
        out += "\"playing\":false,\"paused\":false,\"position\":0,\"duration\":0";
    }
    out += ",\"position_unit\":\"s\",\"queue_length\":" + std::to_string(len);
    out += ",\"repeat\":\"" + std::string(q ? RepeatName(q->GetRepeat()) : "list") + "\"";
    out += ",\"current_url\":" + Quote(q ? q->CurrentUrl() : "");
    out += ",\"title\":" + Quote(q ? q->CurrentTitle() : "");
    out += ",\"ok\":true}";
    return out;
}

std::string HandleMcp(const std::string& payload, Deps& deps) {
    std::string method, id;
    bool id_is_str = false;
    JsonRawValue(payload, "method", &method, nullptr);
    JsonRawValue(payload, "id", &id, &id_is_str);
    if (method.empty()) return "";
    std::string id_json = id_is_str ? Quote(id) : (id.empty() ? "null" : id);

    std::string result;
    if (method == "initialize") {
        result = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                 "\"serverInfo\":{\"name\":\"xz-core\",\"version\":\"1.0.0\"}}";
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":" + result + "}";
    }
    if (method == "tools/list") {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":{\"tools\":" +
               ToolsJson() + "}}";
    }
    if (method != "tools/call") {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id_json +
               ",\"error\":{\"code\":-32601,\"message\":\"method not found\"}}";
    }

    std::string params;
    JsonRawValue(payload, "params", &params, nullptr);
    std::string name;
    JsonRawValue(params, "name", &name, nullptr);
    name = NormalizeName(name);
    std::string arg_json;
    JsonRawValue(params, "arguments", &arg_json, nullptr);
    Args a;
    a.Parse(arg_json.empty() ? "{}" : arg_json);

    MusicQueue* q = deps.queue;
    MusicPort* m = deps.music;
    DevicePort* dev = deps.device;
    std::string text;
    std::string err;

    if (name == "self.music.play_url") {
        std::string url = a.Str("url");
        std::string title = a.Str("title");
        if (!q || url.empty()) {
            err = "缺少 url 或队列未就绪";
        } else {
            q->Set({url}, {title}, 0);
            if (m) m->PlayUrl(url, title);
            text = "{\"ok\":true}";
        }
    } else if (name == "self.music.set_queue") {
        std::string urls = a.Str("urls");
        auto list = SplitLines(urls);
        auto titles = SplitLines(a.Str("titles"));
        if (!q || list.empty()) {
            err = "urls 为空";
        } else {
            q->Set(list, titles, static_cast<int>(a.Int("start_index", 0)));
            if (m) m->PlayUrl(q->CurrentUrl(), q->CurrentTitle());
            text = MusicStatusJson(deps);
        }
    } else if (name == "self.music.enqueue_url") {
        std::string url = a.Str("url");
        if (!q || url.empty()) {
            err = "缺少 url";
        } else {
            bool was_empty = q->Length() == 0;
            q->Enqueue(url, a.Str("title"));
            if (was_empty && m) m->PlayUrl(q->CurrentUrl(), q->CurrentTitle());
            text = MusicStatusJson(deps);
        }
    } else if (name == "self.music.play_index") {
        int idx = static_cast<int>(a.Int("index", -1));
        if (!q || !q->PlayIndex(idx)) {
            err = "索引越界";
        } else {
            if (m) m->PlayUrl(q->CurrentUrl(), q->CurrentTitle());
            text = MusicStatusJson(deps);
        }
    } else if (name == "self.music.next" || name == "self.music.prev") {
        if (!q || q->Length() == 0) {
            err = "队列为空";
        } else {
            bool ok = name == "self.music.next" ? q->Next() : q->Prev();
            if (!ok) {
                err = "队列为空";
            } else {
                if (m) m->PlayUrl(q->CurrentUrl(), q->CurrentTitle());
                text = MusicStatusJson(deps);
            }
        }
    } else if (name == "self.music.pause") {
        if (m) m->Pause();
        text = MusicStatusJson(deps);
    } else if (name == "self.music.resume") {
        if (m) m->Resume();
        text = MusicStatusJson(deps);
    } else if (name == "self.music.stop") {
        if (m) m->Stop();
        if (q) q->SetIndex(q->Index() < 0 ? -1 : q->Index());
        text = MusicStatusJson(deps);
    } else if (name == "self.music.seek") {
        if (m) m->Seek(static_cast<int>(a.Int("position", 0)));
        text = MusicStatusJson(deps);
    } else if (name == "self.music.skip") {
        if (m) m->Skip(static_cast<int>(a.Int("seconds", 0)));
        text = MusicStatusJson(deps);
    } else if (name == "self.music.set_repeat") {
        std::string mode = a.Str("mode", "list");
        if (q) {
            if (mode == "one") q->SetRepeat(Repeat::kOne);
            else if (mode == "off") q->SetRepeat(Repeat::kOff);
            else q->SetRepeat(Repeat::kList);
        }
        text = MusicStatusJson(deps);
    } else if (name == "self.music.status") {
        text = MusicStatusJson(deps);
    } else if (name == "self.music.queue") {
        int limit = static_cast<int>(a.Int("limit", 20));
        text = q ? q->Snapshot(limit) : "{\"length\":0,\"current_index\":-1,\"items\":[]}";
    } else if (name == "self.music.clear_queue") {
        if (q) q->Clear();
        if (m) m->Stop();
        text = MusicStatusJson(deps);
    } else if (name == "self.audio_speaker.set_volume") {
        if (dev) dev->SetVolume(static_cast<int>(a.Int("volume", 50)));
        text = "{\"ok\":true}";
    } else if (name == "self.screen.set_brightness") {
        if (dev) dev->SetBrightness(static_cast<int>(a.Int("brightness", 50)));
        text = "{\"ok\":true}";
    } else if (name == "self.screen.set_theme") {
        if (dev) dev->SetTheme(a.Str("theme"));
        text = "{\"ok\":true}";
    } else if (name == "self.device.standby") {
        if (dev) dev->Standby();
        text = "{\"ok\":true}";
    } else if (name == "self.device.reboot") {
        if (dev) dev->Reboot();
        text = "{\"ok\":true,\"rebooting\":true}";
    } else if (name == "self.device.nvs") {
        text = dev ? dev->Nvs(a.Str("action"), a.Str("namespace"), a.Str("key"),
                              a.Str("value"))
                   : "{\"ok\":false,\"error\":\"no nvs\"}";
    } else if (name == "self.get_device_status") {
        text = dev ? dev->Status() : MusicStatusJson(deps);
    } else if (name == "self.camera.take_photo") {
        text = dev ? dev->CameraPhoto() : "{\"ok\":false,\"error\":\"no camera\"}";
    } else {
        err = "未知工具：" + name;
    }

    if (!err.empty()) {
        text = "{\"ok\":false,\"error\":" + Quote(err) + "}";
    }
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":" + TextResult(text) + "}";
}

}  // namespace xz