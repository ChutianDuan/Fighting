# 压力测试报告

测试日期：2026-04-25  
测试目录：`/Volumes/拯救者PSSD/Fighting`  
构建目录：`build-codex`  
测试目标：`lab_tests`、`lab_stress`

## 测试范围

本次测试使用仓库内 `tests/stress_tests.cpp` 提供的压力测试脚本，覆盖以下行为：

- 输入包编码/解码稳定性：`EncodeInput` / `DecodeInput`
- 状态包编码/解码稳定性：`EncodeState` / `DecodeState`
- 客户端预测后基于权威状态的回滚重放
- 回滚后客户端状态哈希与期望状态哈希一致性
- 权威快照 raw restore/replay 一致性

## 执行命令

```bash
cmake -S . -B build-codex -DLAB_BUILD_TESTS=ON
cmake --build build-codex --target lab_tests lab_stress
ctest --test-dir build-codex --output-on-failure

./build-codex/lab_stress --ticks 60000 --players 2 --history 4096 --state-every 2 --state-delay 7 --redundancy 8
./build-codex/lab_stress --ticks 120000 --players 4 --history 8192 --state-every 2 --state-delay 9 --redundancy 12
./build-codex/lab_stress --ticks 180000 --players 8 --history 16384 --state-every 1 --state-delay 12 --redundancy 16
```

## 结果摘要

CTest 回归结果：

| 测试项 | 结果 | 耗时 |
| --- | --- | ---: |
| `lab_tests` | Passed | 2.30s |
| `lab_stress_smoke` | Passed | 0.76s |
| 总计 | 2/2 Passed | 3.07s |

压力测试结果：

| 场景 | ticks | 玩家 | 输入包 | 状态包 | 网络字节 | 回滚次数 | 重放 tick | 哈希校验 | raw restore 校验 | 耗时 | TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 基准长跑 | 60,000 | 2 | 120,000 | 30,000 | 13,254,234 | 29,996 | 239,967 | 29,996 | 29,996 | 23.7665s | 2,524.56 |
| 中等负载 | 120,000 | 4 | 480,000 | 60,000 | 66,279,588 | 59,995 | 599,949 | 59,995 | 59,995 | 78.3031s | 1,532.51 |
| 极限负载 | 180,000 | 8 | 1,440,000 | 180,000 | 264,534,066 | 179,987 | 2,399,825 | 179,987 | 179,987 | 393.098s | 457.901 |

## 观察

- 三组压力测试全部以 `stress OK` 结束，没有出现解码失败、哈希不一致、历史缺失或 raw restore/replay mismatch。
- 玩家数、输入冗余和状态包频率上升后，TPS 从约 2524 降到约 458，主要压力来自更频繁的状态投递以及大量回滚重放校验。
- 极限负载场景每 tick 发送状态包，完成约 18 万次回滚校验，属于明显偏重的离线验证场景，不代表实际游戏运行帧率。
- `build/` 目录的 CMake cache 指向旧路径 `/Users/chutian/Desktop/Fighting/build`，本次新建 `build-codex/` 避免复用旧缓存造成构建错误。

## 结论

当前压力测试未发现确定性同步、网络包编解码或回滚重放一致性问题。  
在 8 玩家、18 万 tick、每 tick 状态同步的高压配置下，测试仍能完整通过，说明当前 netcode 核心路径具备较好的稳定性；后续如果要做性能优化，建议优先分析 `ReplayFrom` 相关回滚路径和高频状态校验成本。
