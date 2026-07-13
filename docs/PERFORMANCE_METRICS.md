# 性能指标监控与指标测试

本文说明当前项目已经实现的性能指标监控方式，以及这些指标如何在回归测试、真实 UDP loopback 集成测试、实时容量套件和离线压力测试中被验证。这里描述的是仓库内现有能力：项目没有接入 Prometheus、Grafana 或外部 APM，监控主要由客户端 HUD/日志、服务端 ACK 统计、状态哈希对账、`lab_performance` 和 `lab_stress` 输出组成。

## 监控目标

本项目是 60Hz server authoritative + client prediction + rollback/replay 的实时同步 Demo。性能指标监控重点不是传统 Web 服务的接口耗时，而是网络同步链路是否稳定、客户端是否落后、回滚成本是否可接受，以及状态是否发生分叉。

核心问题可以拆成四类：

| 目标 | 观察指标 | 主要位置 |
| --- | --- | --- |
| 网络质量 | RTT、输入包接收数、估算丢包数、丢包率 | `apps/client_main.cpp`、`apps/server_main.cpp` |
| 同步延迟 | input lead、state delay | `apps/client_main.cpp`、HUD |
| 回滚成本 | rollback count、replay ticks、replay cost | `apps/client_main.cpp`、`tests/stress_tests.cpp` |
| 一致性 | state hash mismatch、post-replay hash check、raw restore/replay check | 客户端、`lab_stress`、`lab_network_integration` |

## 运行时指标实现

### 客户端指标结构

运行时网络指标集中放在 `include/lab/app/ClientRender.h` 的 `NetworkStats` 中：

```cpp
struct NetworkStats {
  double rttMs = 0.0;
  double packetLossPct = 0.0;
  int32_t inputLeadTicks = 0;
  int32_t stateDelayTicks = 0;
  double replayCostMs = 0.0;
  uint32_t replayTicks = 0;
  uint32_t inputPacketsReceived = 0;
  uint32_t inputPacketsLost = 0;
};
```

这些字段不会改变模拟结果，只用于显示和日志。客户端主循环每帧把 `NetworkStats` 传给 `RenderFrame`，由 `src/app/ClientRender.cpp` 在右上角 HUD 展示。

### RTT

RTT 使用客户端发送输入包的时间与服务端 ACK 回传的 `serverRecvInputSeq` 做关联。

实现流程：

1. 客户端发送 `InputPacket` 前，把 `seq` 和发送时刻存入有界记录。
2. 服务端接收并验证输入后更新 `lastInputSeq`，在 ACK 中回传 `serverRecvInputSeq`。
3. 客户端收到 ACK 后查找对应 seq 的发送记录。
4. 若记录有效，则计算 `sampleMs = (now - sentSec) * 1000`。
5. 首个样本直接写入 `rttMs`，之后使用指数平滑：`rttMs = old * 0.85 + sample * 0.15`。

这样得到的是输入包到服务端处理并 ACK 回来的近似往返时间。它不是 ICMP ping，也不是严格的网络 RTT；其中包含了服务端 tick 调度、ACK 发送和客户端事件循环处理时间。对于游戏同步调试，这个口径更接近玩家真实感受到的输入确认延迟。

### 输入包丢包率

输入包丢包估计由服务端根据 `InputPacket.seq` 计算。每个客户端维护：

- `inputPacketsReceived`：收到的非 hello 输入包数量。
- `inputPacketsLost`：根据 seq 跳号推算的缺失包数量。
- `lastInputSeq`：该连接已观察到的最大输入包序号。

服务端在 `ObserveInputPacketStats` 中处理这些计数。若当前包 `seq > lastInputSeq + 1`，中间缺口会累加到 `inputPacketsLost`。hello 包不包含玩法输入，不计入统计。

服务端随后把统计写进 ACK：

```cpp
ack.serverInputPacketsReceived = pc->inputPacketsReceived;
ack.serverInputPacketsLost = pc->inputPacketsLost;
```

客户端在 `ApplyAckNetworkStats` 中计算：

```text
packetLossPct = lost * 100 / (received + lost)
```

这个指标只统计客户端到服务端方向的输入包序列缺口，不统计服务端到客户端的 ACK/State 丢包，也不区分网络真实丢包、乱序迟到和进程调度导致的晚到。当前 loopback 集成测试期望该值为 0。

### Input Lead

`inputLeadTicks` 表示客户端本地预测 tick 领先服务端已处理 tick 多少帧：

```text
inputLeadTicks = clientLocalNextTick - ack.serverTickProcessed
```

它反映客户端预测走在权威服务器前面的距离。正常情况下客户端会略微领先，因为客户端每 tick 立即预测，而服务端需要等输入包到达后推进。该值过大时，说明客户端预测领先太多；网络变差时，回滚跨度和视觉修正都可能变大。

### State Delay

`stateDelayTicks` 表示客户端收到某个权威 `StatePacket` 时，本地 tick 已经领先该权威 state 多少帧：

```text
stateDelayTicks = clientLocalNextTick - state.tick
```

它直接描述权威状态到达客户端时的“陈旧程度”。在 `OnUdp` 处理 State 时更新：

```cpp
ctx->netStats.stateDelayTicks = int32_t(ctx->tick) - int32_t(st->tick);
```

这个指标通常受到服务端 `kStateEvery`、网络延迟、客户端事件循环和本地预测速度影响。state delay 越大，收到权威状态后需要重放的 tick 通常也越多。

### 回滚次数与重放成本

客户端收到权威 State 后会构造 `WorldSnapshot`，然后决定是否计入一次“有感回滚”。当前回滚判定只比较本地玩家的位置、HP、动作和落地状态，不把远端玩家误差作为触发条件，避免远端预测误差导致回滚计数持续膨胀。

无论是否计入 `rollbackCount`，客户端都会执行 rebase + replay：

1. `worldPred.Restore(auth)` 恢复权威快照。
2. 从 `auth.tick + 1` 重放到当前本地 tick。
3. 每次重放调用同一个 `World::Step`。
4. 重放后的快照重新写入 `StateHistory`。

`replayTicks` 和 `replayCostMs` 在 `ApplyAuthoritativeState` 中采集：

```text
replayTicks = max(0, clientLocalNextTick - auth.tick - 1)
replayCostMs = RestoreAndReplay wall time in milliseconds
```

`replayTicks` 衡量重放跨度，`replayCostMs` 衡量本机执行这次恢复和重放实际花费的墙钟时间。二者结合可以判断压力来源：如果 tick 跨度正常但 cost 偏高，可能是 `World::Step`、快照复制、哈希或历史写入变慢；如果 tick 跨度持续升高，可能是网络延迟或 state 下发频率问题。

### 哈希不一致

一致性指标用 `Hasher::Hash(WorldSnapshot)` 实现。服务端把权威哈希写入 State 和 ACK，客户端收到后重新计算本地还原快照的哈希。

State 校验路径：

1. 客户端解码 `StatePacket`。
2. 把网络量化字段还原为 `WorldSnapshot`。
3. 使用 `Hasher::Hash(auth)` 重新计算。
4. 与 `st.stateHash` 比较。
5. 不一致时累加 `hashMismatchCount` 并记录 `lastHashMismatchTick`。

ACK 对账路径更保守：只有当客户端 `stateHist` 中已经保存了同 tick 的权威快照，才把本地哈希和 `ack.serverStateHash` 比较。这样可以避免拿预测态和权威态直接比较造成误报。

## 指标展示与日志

客户端 HUD 显示两组信息。

左侧是同步正确性和世界状态：

- `tick`
- `rollbacks`
- `hash mismatch`
- `maze seed`
- 每个玩家的 HP、action、state timer

右侧是网络与性能指标：

- `rtt`
- `loss`
- `lead`
- `state delay`
- `replay`
- `input pkts`

客户端还会每 60 tick 输出一次 `STAT` 日志，字段包括：

```text
tick, lead, rtt, loss, state_delay, replay ticks/cost,
rollbacks, last rollback tick, hash_mismatch, last hash mismatch tick
```

服务端每 60 tick 输出一次权威 tick、玩家位置和 hash，便于和客户端日志对照。

## 压力测试指标实现

`tests/stress_tests.cpp` 提供离线压力测试。它不依赖 SDL 窗口和真实 socket，而是在单进程里同时模拟权威端、客户端预测端、输入编解码、状态编解码、延迟投递和回滚重放。

### 可调参数

`lab_stress` 支持这些参数：

| 参数 | 含义 |
| --- | --- |
| `--ticks N` | 总模拟 tick 数 |
| `--players N` | 玩家数，范围 1 到 8 |
| `--history N` | 输入历史和状态历史环形缓冲容量 |
| `--state-every N` | 每隔多少 tick 生成一个权威 State |
| `--state-delay N` | State 延迟多少 tick 后投递给客户端 |
| `--redundancy N` | 每个输入包携带多少帧冗余输入 |

### 输出指标

压力测试成功时输出一行 `stress OK`，后面跟聚合指标：

| 指标 | 含义 |
| --- | --- |
| `ticks` | 本次模拟总 tick 数 |
| `players` | 玩家数 |
| `inputPackets` | 编码并解码的输入包数量，理论上约为 `ticks * players` |
| `statePackets` | 编码并延迟投递的权威状态包数量，理论上约为 `ticks / stateEvery` |
| `bytes` | 输入包和状态包编码后的累计字节数 |
| `replays` | 客户端收到 State 后执行回滚重放的次数 |
| `replayedTicks` | 所有回滚重放累计重放 tick 数 |
| `hashChecks` | post-replay 客户端状态与期望状态的哈希比较次数 |
| `rawRestoreChecks` | 从原始权威快照 restore/replay 到当前 tick 的一致性检查次数 |
| `timeSec` | 压力测试墙钟耗时 |
| `ticksPerSec` | 离线模拟吞吐，`ticks / timeSec` |

JSON 输出还包含完整 tick、State encode、State decode 和 restore/replay 的 `mean/stddev/p50/p95/p99/p99.9/max`，以及 `p99-p50` 抖动。采样使用 `steady_clock` 纳秒计时，percentile 使用 nearest-rank。

这些指标既能验证正确性，也能粗略观察性能趋势。例如玩家数、状态频率和状态延迟上升时，`replayedTicks` 与 `timeSec` 通常会上升，`ticksPerSec` 会下降。

### 压力测试校验点

压力测试不是只跑吞吐，它会在高频路径上做强校验：

- 每 tick 为每个玩家生成确定性输入，写入历史。
- 每 tick 构造 `InputPacket`，执行 `EncodeInput` 和 `DecodeInput`，校验 count 与 newest tick。
- 权威端用真实输入推进，客户端用故意带偏差的预测输入推进。
- 每隔 `stateEvery` tick 把权威快照编码为 State，并按 `stateDelay + jitter` 延迟投递。
- State 到达后执行 `DecodeState`，并校验解码快照 hash 等于包内 `stateHash`。
- 从原始权威历史快照 restore/replay 到当前 tick，校验 hash 等于权威当前历史。
- 客户端从网络量化后的权威快照 restore/replay 到当前 tick，校验 hash 等于期望结果。

只要任一校验失败，测试会输出 `stress FAIL` 并非零退出；CTest 和脚本都能捕获失败。

## 真实 UDP 集成测试指标

`tests/network_integration_tests.cpp` 使用真实 loopback UDP socket，直接驱动生产代码中的 `AuthoritativeServer` 和两个 bot client，避免测试复制服务端实现。

测试覆盖：

- 两个 bot client 发送 hello。
- 生产服务端核心分配 session/player 并发送 Start。
- 丢弃首个 Start 后通过 hello 重发恢复。
- 拒绝越界 tick 和非法方向输入。
- 玩家超时后即使首个 Reset 丢失，也能重试并补位开启新比赛。
- bot client 每 tick 发送带冗余历史的 Input。
- 服务端每 tick 推进权威世界，发送 State 和 Ack。
- 客户端解码 State 并校验 `stateHash`。
- 客户端读取 ACK 中的输入包接收/丢包统计。

验收条件包括：

- 首个 Start 被丢弃后，客户端通过 hello 重试进入同一场比赛。
- 非法 tick/方向输入不会改变权威世界。
- 两个客户端都能收到身份匹配且 hash 正确的 State。
- 玩家超时后服务端退出运行态；首个 Reset 被丢弃后仍能重试送达。
- 新客户端补位后，双方进入递增 `matchId` 的新比赛。

## 实时容量与 UDP 性能套件

`tests/performance/performance_tests.cpp` 生成 `lab_performance`，直接实例化生产 `AuthoritativeServer`，不复制服务端规则。它包含两个互相独立的口径：

- `capacity`：单线程批量推进多个生产服务端核心，每房间 2 玩家、60Hz，指数搜索首个失败档并二分细化。正式候选预热 5 秒、采样 20 秒、重复 3 次；安全容量追加 60 秒 RSS 稳态采样。
- `udp`：一个生产服务端核心和两个真实 loopback UDP 客户端，分别提供 60、120、240、480 PPS/客户端，统计 offered/accepted/rejected PPS、ACK RTT、Ack/State PPS、上下行 Mbps、CPU、RSS 和错误。

最大稳定容量要求三轮同时满足批量 tick P99 ≤16.67ms、deadline miss ≤0.1%、协议及 hash 错误为 0。建议安全容量为最大稳定容量的 80%，并受按本机 60% 物理内存预算推算的上限约束。

多房间容量只衡量单线程生产核心，不包含多房间 UDP 路由、socket 系统调用和公网开销。`lab_stress` 的 `ticksPerSec` 是带一致性校验的离线吞吐，也不能替代 60Hz deadline 下的实时容量。

## 执行方式

常规回归和集成测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLAB_BUILD_TESTS=ON -DLAB_BUILD_PERFORMANCE=ON
cmake --build build --target lab_tests lab_stress lab_network_integration lab_performance
ctest --test-dir build --output-on-failure
```

单独运行压力测试：

```bash
./build/lab_stress --ticks 60000 --players 2 --history 4096 --state-every 2 --state-delay 7 --redundancy 8
./build/lab_stress --ticks 120000 --players 4 --history 8192 --state-every 2 --state-delay 9 --redundancy 12
./build/lab_stress --ticks 180000 --players 8 --history 16384 --state-every 1 --state-delay 12 --redundancy 16
```

快速性能诊断和正式报告：

```bash
./build/lab_performance --profile smoke --mode all --json /tmp/performance-smoke.json
./build/lab_performance --profile standard --mode all --json performance_results.json
python3 tools/run_performance_report.py
```

标准性能测试注册为默认 CTest 项，标签为 `performance`、串行执行、超时 900 秒。只跑它可使用 `ctest --test-dir build -L performance --output-on-failure`；只跑常规快速回归可临时使用 `ctest --test-dir build -LE performance --output-on-failure`。正式基线和逐轮原始数据见 [并发容量、P99 与完整性能报告](PERFORMANCE_REPORT.md)。

运行实际客户端观察 HUD：

```bash
./build/lab_server
./build/lab_client
./build/lab_client
```

## 当前测试结果解读

已有压力测试报告见 `docs/STRESS_TEST_REPORT.md`。报告中的三组场景全部通过，说明当时没有出现输入/状态解码失败、哈希不一致、历史缺失或 raw restore/replay mismatch。

结果里 `ticksPerSec` 从基准长跑的约 2524 降到极限负载的约 458，主要原因是：

- 玩家数从 2 增加到 8，输入包数量和每 tick 模拟成本上升。
- `stateEvery` 从 2 降到 1，状态包生成和投递频率翻倍。
- `stateDelay` 增大后，每次 State 到达要重放更多 tick。
- 极限场景产生约 240 万个累计重放 tick，回滚重放成为主要成本。

这些结果不能直接等同于真实游戏帧率，因为 `lab_stress` 是离线强校验场景，会额外执行大量 hash、编解码和 restore/replay 检查。但它很适合用来比较优化前后的相对变化。

## 验收标准

当前项目的指标测试可以按下面标准判断是否通过：

- `ctest --test-dir build --output-on-failure` 全部通过。
- `lab_stress` 输出 `stress OK`。
- `hashChecks` 和 `rawRestoreChecks` 均大于 0，说明一致性校验确实执行了。
- `hash mismatch` 在正常 loopback 运行中保持 0。
- `lab_network_integration` 输出 `network integration OK`。
- loopback 集成测试中 ACK 报告的输入包丢失为 0。
- 手动观察客户端 HUD 时，RTT、loss、lead、state delay、replay 指标能持续刷新，没有出现异常增长。

## 已知边界

当前指标监控仍是轻量调试实现，有几个明确边界：

- 没有外部时序数据库、告警规则和长期趋势图。
- 丢包率只估算客户端到服务端的输入包 seq 缺口。
- RTT 是基于输入 ACK 的游戏链路 RTT，包含 tick 调度成本。
- `replayCostMs` 是单次客户端 restore/replay 墙钟耗时，受本机性能和调度影响。
- 集成测试使用 loopback，不覆盖真实公网抖动、乱序、带宽限制和连接 churn。
- 压力测试输出是单次聚合值，还没有自动保存 CSV/JSON 供长期对比。

后续如果要扩展，可以把 `NetworkStats` 和 `lab_stress` 输出改成结构化 JSON，增加滑动窗口百分位数、State 下行丢包统计、真实网络故障注入和基准阈值回归检查。
