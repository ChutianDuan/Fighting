#pragma once
#include "lab/sim/InputBuffer.h"
#include "lab/sim/World.h"
#include "lab/io/RecordReplay.h"
#include "lab/net/NetStub.h"
#include <cstdint>

// 离线教学辅助：脚本输入录制一遍，再按相同固定步长回放比较哈希。
// 在线客户端/服务端入口不调用此类，实际回滚历史使用 StateHistory。
class FixedTimestepRunner {
public:
  struct Config {
    double dt = 1.0 / 60.0;        // 固定仿真步长
    double maxFrameTime = 0.25;    // clamp，防止死亡螺旋
    Tick   maxTicksToRun = 600;    // demo：跑 10 秒（60*10）
    size_t inputBufferCap = 2048;  // 输入环形缓冲容量
    size_t snapshotCap = 2048;     // 教学用快照环容量，当前不执行回滚
  };

  explicit FixedTimestepRunner(const Config& cfg);

  // 运行固定帧录制和回放，将逐帧哈希比较结果写入日志。
  void Run();

private:
  // 生成可重复的脚本输入，不读取 SDL 或真实键盘。
  InputCmd SampleLocalInput(Tick tick);

  // tick 级：取输入->仿真->存快照/hash
  void SimTick(Tick tick);

  // 保存教学快照到环形缓冲；当前 Run 不读取该历史。
  void SaveSnapshot(const WorldSnapshot& s);

private:
  Config cfg_;
  InputBuffer inputBuf_;
  lab::sim::World world_;
  RecordReplay rr_;
  NetStub net_; // 保留的教学占位，Run 当前不使用网络

  // 教学快照环；在线预测与校正不经过这里。
  struct SnapSlot { bool valid=false; WorldSnapshot s{}; };
  std::vector<SnapSlot> snaps_;

  // 两次运行的逐帧哈希，用于检查同环境下的可重复性。
  std::vector<uint64_t> hashes_;
};
