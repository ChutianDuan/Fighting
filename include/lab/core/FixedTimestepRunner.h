#pragma once
#include "lab/sim/InputBuffer.h"
#include "lab/sim/World.h"
#include "lab/io/RecordReplay.h"
#include "lab/net/NetStub.h"
#include <cstdint>

class FixedTimestepRunner {
public:
  struct Config {
    double dt = 1.0 / 60.0;        // 固定仿真步长
    double maxFrameTime = 0.25;    // clamp，防止死亡螺旋
    Tick   maxTicksToRun = 600;    // demo：跑 10 秒（60*10）
    size_t inputBufferCap = 2048;  // 输入环形缓冲容量
    size_t snapshotCap = 2048;     // 状态快照容量（为回滚预留）
  };

  explicit FixedTimestepRunner(const Config& cfg);

  // 运行一次 demo（内部会跑 fixed timestep）
  void Run();

private:
  // 采样本地输入：你可以接 SDL/GLFW/控制台输入
  InputCmd SampleLocalInput(Tick tick);

  // tick 级：取输入->仿真->存快照/hash
  void SimTick(Tick tick);

  // 为回滚预留：保存快照（ring buffer）
  void SaveSnapshot(const WorldSnapshot& s);

private:
  Config cfg_;
  InputBuffer inputBuf_;
  lab::sim::World world_;
  RecordReplay rr_;
  NetStub net_;

  // 快照 ring：用于未来 rollback
  struct SnapSlot { bool valid=false; WorldSnapshot s{}; };
  std::vector<SnapSlot> snaps_;

  // hash 日志用于验收 determinism
  std::vector<uint64_t> hashes_;
};
