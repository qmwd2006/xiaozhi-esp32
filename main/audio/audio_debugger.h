#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <cstdint>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

class AudioDebugger {
public:
    // port_override: 0 表示使用 CONFIG_AUDIO_DEBUG_UDP_SERVER 的端口；
    // 非 0 时复用该配置的 IP，改用指定端口（用于区分 AFE 前/后两路）。
    explicit AudioDebugger(uint16_t port_override = 0);
    ~AudioDebugger();

    void Feed(const std::vector<int16_t>& data);

private:
    int udp_sockfd_ = -1;
    struct sockaddr_in udp_server_addr_;
};

#endif
