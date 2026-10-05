#pragma once
#include <lab/net/UdpSocket.h>
#include <lab/session/RoomServer.h>

namespace lab::session {
// 实际 UDP 入口和容量 bot 共用收发、固定帧调度与到期清理。
class ServerRuntime {
  public:
    explicit ServerRuntime(ServerConfig config = {}) : server(config) {}

    // Attach 使用调用方持有的 event_base；Poll 也可由容量测试手动驱动。
    bool Open(uint16_t port, const std::string &ip = "0.0.0.0");
    bool Attach(event_base *base);
    void Poll(double now);
    uint16_t port() const { return socket_.LocalPort(); }

    RoomServer server;

    // 统计覆盖传输与单次权威推进（含该帧编码/发送），供容量报告使用。
    uint64_t sentPackets = 0;
    uint64_t sentBytes = 0;
    uint64_t receivedPackets = 0;
    uint64_t receivedBytes = 0;
    uint64_t deadlineMiss = 0;
    std::vector<double> tickMs;

  private:
    net::UdpSocket socket_;
    std::map<uint64_t, net::UdpAddr> addresses_;

    // 固定步长累计时间；过载时保留余量，下一次 Poll 继续追帧。
    double previous_ = -1;
    double accumulator_ = 0;
    double nextAddressSweep_ = 0;

    void Receive(const net::UdpAddr &from, const uint8_t *data, size_t size, double now);
    void Send(const std::vector<Datagram> &outgoing);
    static void Read(void *, const net::UdpAddr &, const uint8_t *, size_t);
};
} // namespace lab::session
