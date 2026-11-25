#pragma once
#include <string>

#include "xz/music_queue.h"
#include "xz/ports.h"

namespace xz {

struct Deps {
    MusicQueue* queue = nullptr;
    MusicPort* music = nullptr;
    DevicePort* device = nullptr;
};

// 23 个 self.* 工具的元数据（和固件 mcp_server.cc 同名同参）
std::string ToolsJson();

// 处理一条 MCP payload（initialize / tools/list / tools/call），返回响应 payload
std::string HandleMcp(const std::string& payload, Deps& deps);

// self.music.status 的 JSON（模拟器和固件都用同一份形状）
std::string MusicStatusJson(Deps& deps);

}  // namespace xz