#include <algorithm>
#include <chrono>
#include <lab/session/ServerRuntime.h>
#include <lab/time/Clock.h>

namespace lab::session {
namespace {
constexpr int kSocketBufferBytes = 4 << 20;
constexpr int kPacketsPerPoll = 8192;
constexpr unsigned kCatchUpFramesPerPoll = 4;
constexpr double kAddressSweepSeconds = 1;
} // namespace

bool ServerRuntime::Open(uint16_t port, const std::string &ip) {
    return socket_.Open() && socket_.Bind(port, ip) && socket_.SetNonBlocking(true) &&
           socket_.SetRecvBuf(kSocketBufferBytes) && socket_.SetSendBuf(kSocketBufferBytes);
}

bool ServerRuntime::Attach(event_base *base) { return socket_.StartEventRead(base, &Read, this); }

void ServerRuntime::Read(void *user, const net::UdpAddr &from, const uint8_t *bytes, size_t size) {
    static_cast<ServerRuntime *>(user)->Receive(from, bytes, size, Clock::NowSeconds());
}

void ServerRuntime::Receive(const net::UdpAddr &from, const uint8_t *bytes, size_t size,
                            double now) {
    ++receivedPackets;
    receivedBytes += size;
    auto outgoing = server.HandleDatagram(from.Key(), std::span(bytes, size), now);
    // 只有成功握手才记住长期回包地址；拒绝回复直接发给本次来源。
    for (const auto &datagram : outgoing) {
        auto reply = Decode(datagram.bytes);
        if (reply && reply->kind == Kind::Welcome)
            addresses_[from.Key()] = from;
        if (datagram.endpoint == from.Key()) {
            if (socket_.SendTo(from, datagram.bytes)) {
                ++sentPackets;
                sentBytes += datagram.bytes.size();
            }
        } else
            Send({datagram});
    }
}

void ServerRuntime::Send(const std::vector<Datagram> &outgoing) {
    for (const auto &datagram : outgoing) {
        auto addressIt = addresses_.find(datagram.endpoint);
        if (addressIt != addresses_.end() && socket_.SendTo(addressIt->second, datagram.bytes)) {
            ++sentPackets;
            sentBytes += datagram.bytes.size();
        }
    }
}

void ServerRuntime::Poll(double now) {
    net::UdpAddr source;
    Bytes bytes;
    for (int packetCount = 0; packetCount < kPacketsPerPoll && socket_.RecvFrom(source, bytes);
         ++packetCount)
        Receive(source, bytes.data(), bytes.size(), now);

    if (previous_ < 0)
        previous_ = now;
    accumulator_ += std::max(0.0, now - previous_);
    previous_ = now;
    Send(server.Expire(now));

    // 房间核心决定哪些端点仍有效，传输层定期移除过期地址。
    if (now >= nextAddressSweep_) {
        auto endpoints = server.Endpoints();
        std::sort(endpoints.begin(), endpoints.end());
        for (auto addressIt = addresses_.begin(); addressIt != addresses_.end();) {
            if (!std::binary_search(endpoints.begin(), endpoints.end(), addressIt->first))
                addressIt = addresses_.erase(addressIt);
            else
                ++addressIt;
        }
        nextAddressSweep_ = now + kAddressSweepSeconds;
    }

    unsigned advancedFrames = 0;
    while (accumulator_ >= kStep && advancedFrames < kCatchUpFramesPerPoll) {
        auto begin = std::chrono::steady_clock::now();
        Send(server.AdvanceOneTick(now));
        auto elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        tickMs.push_back(elapsedMs);
        if (elapsedMs > kStep * 1000)
            ++deadlineMiss;
        accumulator_ -= kStep;
        ++advancedFrames;
    }
    // 每次回调最多推进 4 帧；累计时间留到下一回调，不丢弃权威模拟帧。
}
} // namespace lab::session
