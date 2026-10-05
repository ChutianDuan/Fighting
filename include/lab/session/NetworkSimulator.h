#pragma once
#include <lab/session/Protocol.h>
#include <map>
#include <random>

namespace lab::session {
struct LinkConfig {
    double latencyMs = 25; // 单向基础延迟，单位为毫秒。
    double jitterMs = 20;  // 在 [-jitterMs, +jitterMs] 内均匀采样。
    double loss = .01;
    double reorder = .02;
    double duplicate = .01;
};

struct NetworkConfig {
    LinkConfig upstream;
    LinkConfig downstream;
    uint64_t seed = 1;
};

struct NetworkEvent {
    uint64_t id = 0; // 同一投递时间下的稳定次序，也用于完整轨迹重演。
    uint64_t endpoint = 0;
    bool toServer = false;
    bool dropped = false;
    double sent = 0; // 注入的单调时间，单位为秒。
    double delivery = 0;
    Bytes bytes;
};

// 对控制包和玩法包统一模拟；不读取真实时钟，不依赖 socket 或操作系统调度。
class NetworkSimulator {
  public:
    explicit NetworkSimulator(NetworkConfig config = {}) : config_(config), random_(config.seed) {}
    void Send(uint64_t endpoint, bool toServer, const Bytes &bytes, double now);
    std::vector<NetworkEvent> Deliver(double now);
    void Healthy() {
        // 仅恢复后续发送的链路，已排队的数据报仍按原定时间投递。
        config_.upstream = {0, 0, 0, 0, 0};
        config_.downstream = config_.upstream;
    }
    const std::vector<NetworkEvent> &events() const { return events_; }
    uint64_t digest() const;
    Json Trace() const;
    static Bytes Unhex(const std::string &hex);

  private:
    NetworkConfig config_;
    std::mt19937_64 random_;
    uint64_t nextEventId_ = 0;
    // 先按投递时间，再按事件编号排序，确保同种子得到同样的投递顺序。
    std::map<std::pair<double, uint64_t>, NetworkEvent> queue_;
    std::vector<NetworkEvent> events_;
    // 直接将 RNG 的高 53 位映射到 [0, 1)，避免标准分布实现差异。
    double UniformRandom() { return (random_() >> 11) * 0x1.0p-53; }
};

Json ConfigJson(const NetworkConfig &config);
NetworkConfig ParseConfig(const Json &json);
} // namespace lab::session
