#include <algorithm>
#include <cmath>
#include <lab/session/NetworkSimulator.h>
#include <stdexcept>

namespace lab::session {
namespace {
constexpr double kMaxLinkDelayMs = 10000.0;
constexpr double kReorderMinimumDelayMs = 20.0;
constexpr double kReorderDelayRangeMs = 60.0;
constexpr double kMillisecondsPerSecond = 1000.0;
constexpr uint64_t kDigestOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kDigestPrime = 1099511628211ULL;

std::string Hex(const Bytes &bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

Json LinkJson(const LinkConfig &config) {
    return {{"latencyMs", config.latencyMs},
            {"jitterMs", config.jitterMs},
            {"loss", config.loss},
            {"reorder", config.reorder},
            {"duplicate", config.duplicate}};
}
LinkConfig ParseLinkConfig(const Json &json) {
    LinkConfig config{json.at("latencyMs"), json.at("jitterMs"), json.at("loss"),
                      json.at("reorder"), json.at("duplicate")};
    for (double value :
         {config.latencyMs, config.jitterMs, config.loss, config.reorder, config.duplicate})
        if (!std::isfinite(value) || value < 0)
            throw std::runtime_error("network parameter range");
    if (config.latencyMs > kMaxLinkDelayMs || config.jitterMs > kMaxLinkDelayMs ||
        config.loss > 1 || config.reorder > 1 || config.duplicate > 1)
        throw std::runtime_error("network parameter range");
    return config;
}
} // namespace

Json ConfigJson(const NetworkConfig &config) {
    return {{"seed", config.seed},
            {"upstream", LinkJson(config.upstream)},
            {"downstream", LinkJson(config.downstream)}};
}

NetworkConfig ParseConfig(const Json &json) {
    return {ParseLinkConfig(json.at("upstream")), ParseLinkConfig(json.at("downstream")),
            json.at("seed")};
}

void NetworkSimulator::Send(uint64_t endpoint, bool toServer, const Bytes &bytes, double now) {
    const auto &link = toServer ? config_.upstream : config_.downstream;
    auto enqueue = [&](bool dropped) {
        NetworkEvent event;
        event.id = ++nextEventId_;
        event.endpoint = endpoint;
        event.toServer = toServer;
        event.sent = now;
        event.bytes = bytes;
        event.dropped = dropped;
        auto latencyMs = std::max(0.0, link.latencyMs + (UniformRandom() * 2 - 1) * link.jitterMs);
        // 给部分包追加延迟，让之后发送的包有机会先到达，而非直接交换队列项。
        if (UniformRandom() < link.reorder)
            latencyMs += kReorderMinimumDelayMs + UniformRandom() * kReorderDelayRangeMs;
        event.delivery = now + latencyMs / kMillisecondsPerSecond;
        // 丢包仍记录到轨迹，只有投递队列省略它，便于完整复现网络过程。
        events_.push_back(event);
        if (!dropped)
            queue_.emplace(std::pair(event.delivery, event.id), std::move(event));
    };
    enqueue(UniformRandom() < link.loss);
    // 副本独立采样延迟，即使原包丢失，副本仍可投递。保持抽样顺序稳定。
    if (UniformRandom() < link.duplicate)
        enqueue(false);
}

std::vector<NetworkEvent> NetworkSimulator::Deliver(double now) {
    std::vector<NetworkEvent> delivered;
    while (!queue_.empty() && queue_.begin()->first.first <= now) {
        delivered.push_back(std::move(queue_.begin()->second));
        queue_.erase(queue_.begin());
    }
    return delivered;
}

uint64_t NetworkSimulator::digest() const {
    uint64_t hash = kDigestOffsetBasis;
    auto mix = [&](uint64_t value) {
        for (int byteIndex = 0; byteIndex < 8; ++byteIndex) {
            hash ^= static_cast<uint8_t>(value >> (byteIndex * 8));
            hash *= kDigestPrime;
        }
    };
    // 摘要包含事件顺序、时间的原始位模式和完整报文字节，不包含实测耗时。
    for (const auto &event : events_) {
        mix(event.id);
        mix(event.endpoint);
        mix(event.toServer);
        mix(event.dropped);
        mix(std::bit_cast<uint64_t>(event.sent));
        mix(std::bit_cast<uint64_t>(event.delivery));
        for (auto byte : event.bytes) {
            hash ^= byte;
            hash *= kDigestPrime;
        }
    }
    return hash;
}

Json NetworkSimulator::Trace() const {
    Json trace = Json::array();
    for (const auto &event : events_) {
        trace.push_back({{"id", event.id},
                         {"endpoint", event.endpoint},
                         {"toServer", event.toServer},
                         {"dropped", event.dropped},
                         {"sent", event.sent},
                         {"delivery", event.delivery},
                         {"bytes", Hex(event.bytes)}});
    }
    return trace;
}

Bytes NetworkSimulator::Unhex(const std::string &hex) {
    if (hex.size() % 2 || hex.size() > kMaxDatagram * 2)
        throw std::runtime_error("invalid hex");
    auto nibble = [](char digit) {
        if (digit >= '0' && digit <= '9')
            return digit - '0';
        if (digit >= 'a' && digit <= 'f')
            return digit - 'a' + 10;
        throw std::runtime_error("invalid hex");
    };
    Bytes bytes;
    for (size_t index = 0; index < hex.size(); index += 2)
        bytes.push_back((nibble(hex[index]) << 4) | nibble(hex[index + 1]));
    return bytes;
}
} // namespace lab::session
