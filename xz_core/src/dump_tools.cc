// 原生跑一次 tools/list，把工具表打成 JSON —— 和固件同源的契约快照
#include <cstdio>
#include <string>
#include "xz/mcp.h"
int main() {
    printf("%s\n", xz::ToolsJson().c_str());
    return 0;
}
