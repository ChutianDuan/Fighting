#include "lab/core/FixedTimestepRunner.h"
#include "lab/time/Clock.h"
#include "lab/sim/Hasher.h"
#include "lab/util/log.h"

#include <vector>

FixedTimestepRunner::FixedTimestepRunner(const Config& cfg)
  : cfg_(cfg),
    inputBuf_(cfg.inputBufferCap),
    world_(1),
    snaps_(cfg.snapshotCap == 0 ? 1 : cfg.snapshotCap) {}

InputCmd FixedTimestepRunner::SampleLocalInput(Tick tick) {
  // 可重复的脚本输入：前 120 帧向右，后 120 帧向左，其余不动。
  InputCmd cmd;
  cmd.tick = tick;

  if (tick < 120) cmd.moveX = +1;
  else if (tick < 240) cmd.moveX = -1;
  else cmd.moveX = 0;

  cmd.buttons = 0;
  return cmd;
}

void FixedTimestepRunner::SaveSnapshot(const WorldSnapshot& s) {
  const size_t idx = size_t(s.tick % snaps_.size());
  snaps_[idx].valid = true;
  snaps_[idx].s = s;
}

void FixedTimestepRunner::SimTick(Tick tick) {
  // 1) 生成离线脚本输入。
  InputCmd local = SampleLocalInput(tick);

  // 2) 录制输入，供第二次运行验证可重复性。
  if (rr_.IsRecording()) {
    rr_.PushRecorded(local);
  }

  // 3) 使用与在线输入历史相同的按帧缓冲结构。
  inputBuf_.Put(local);

  // 4) 本步仿真：缺失则 default（回滚/丢包时会用到）
  InputCmd cmd = inputBuf_.Get(tick).value_or(InputBuffer::DefaultForTick(tick));

  // 5) 进行固定 dt 仿真
  world_.Step(std::vector<InputCmd>{cmd}, float(cfg_.dt));

  // 6) 保存教学快照，在线回滚使用独立的 StateHistory。
  const WorldSnapshot snap = world_.Snapshot();
  SaveSnapshot(snap);

  // 7) hash：用于一致性比较
  uint64_t h = Hasher::Hash(snap);
  hashes_.push_back(h);

  // demo 打印少量信息
  if (tick % 60 == 0) {
    const PlayerState p1 = snap.players.empty() ? PlayerState{} : snap.players[0];
    LOGI("tick=%u x=%.3f v=%.3f hash=%llu",
         snap.tick, p1.x, p1.v, (unsigned long long)h);
  }
}

void FixedTimestepRunner::Run() {
  // --- 第一遍：录制运行 ---
  rr_.StartRecord();
  hashes_.clear();

  double prev = Clock::NowSeconds();
  double acc = 0.0;
  Tick tick = 0;

  while (tick < cfg_.maxTicksToRun) {
    double now = Clock::NowSeconds();
    double frameTime = now - prev;
    prev = now;

    if (frameTime > cfg_.maxFrameTime) frameTime = cfg_.maxFrameTime;
    acc += frameTime;

    // 固定步长内层循环消耗累计时间，外层不参与世界积分。

    while (acc >= cfg_.dt && tick < cfg_.maxTicksToRun) {
      SimTick(tick);
      tick++;
      acc -= cfg_.dt;
    }

    // 此离线路径不渲染，不使用剩余累计时间做视觉插值。
  }

  // 备份第一遍 hashes
  std::vector<uint64_t> hashes1 = hashes_;
  const auto recorded = rr_.Recorded();

  // --- 第二遍：回放运行（determinism 验收）---
  // 重置世界和输入缓冲
  world_ = lab::sim::World{1};
  inputBuf_ = InputBuffer(cfg_.inputBufferCap);
  hashes_.clear();

  rr_.StartReplay();

  for (Tick t = 0; t < cfg_.maxTicksToRun; ++t) {
    // 回放输入
    InputCmd cmd = (t < recorded.size()) ? recorded[t] : InputBuffer::DefaultForTick(t);
    inputBuf_.Put(cmd);

    InputCmd use = inputBuf_.Get(t).value_or(InputBuffer::DefaultForTick(t));
    world_.Step(std::vector<InputCmd>{use}, float(cfg_.dt));

    uint64_t h = Hasher::Hash(world_.Snapshot());
    hashes_.push_back(h);
  }

  // 对比两次 hash
  bool ok = (hashes1.size() == hashes_.size());
  if (ok) {
    for (size_t i = 0; i < hashes1.size(); ++i) {
      if (hashes1[i] != hashes_[i]) {
        ok = false;
        LOGE("Determinism FAILED at tick=%zu: h1=%llu h2=%llu",
             i, (unsigned long long)hashes1[i], (unsigned long long)hashes_[i]);
        break;
      }
    }
  }

  if (ok) LOGI("Determinism OK: %zu ticks matched.", hashes_.size());
}
