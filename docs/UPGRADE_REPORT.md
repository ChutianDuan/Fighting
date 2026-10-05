# v6 六阶段升级验证报告

验收环境为当前 macOS / Apple Silicon。协议与模拟版本分别为 v6 / 1，固定步长 1/60 秒。没有新增生产依赖；历史 v5 性能报告和工作区无关修改保留。

## 六阶段交付

| 阶段 | 实现 | 验证 |
| --- | --- | --- |
| 同步可靠性 | 无 SDL/socket 调度依赖的 ClientSession；严格 v6 校验后提交；完成帧 0/输入帧 1；回绕比较；120 帧恢复预算；凭证重连与墙体约束 | 坏包、未来状态、缺失历史、回绕区间、重启、三秒恢复、十秒到期与贴墙推箱回归 |
| 网络实验 | NetworkSimulator 双向数据报扰动、稳定事件编号、注入时钟/身份随机源、JSON 指标、失败轨迹 | 9 场 ×3000 tick 标准矩阵全部通过；恢复正常网络后一秒内收到有效权威进度；种子重演逐事件对照 |
| 时序与显示 | 64 RTT 样本，2–10 帧目标领先；每次最多四帧；本地百毫秒偏移消除、远端百毫秒插值、平滑开关 | 调度预算、偏移期限和开关不修改模拟哈希；不足样本保持最新权威显示状态 |
| 回放诊断 | 原始 float JSON Lines、实际应用输入、完整初值/地图、客户端记录累计确认、顺序验证、SDL 暂停/单步/速度/重新开始 | 服务端/客户端逐帧校验；篡改定位 players[1].shotCooldown；截断与版本检查；暂停/单步控制测试 |
| 房间与 UDP | 默认 64 独立房间、凭证/代次隔离、空房间回收、共用 ServerRuntime 的真实 socket bot | 两房间并发、单房间重连/过期不影响另一房间、旧代次拒绝；实际 SDL 入口 UDP 冒烟 |
| 性能 | 保存阶段 5 Release 二进制；共享只读历史地图、内部视图、命令复用、缓存快照读取及预留确定容器容量 | 相同构建、机器、场景和录制设置的 1/16/64 房间前后对照；事件、哈希和功能统计保持一致 |

## 构建、回归与窗口检查

Debug、Release、无 SDL 及 AddressSanitizer/UBSan 最终构建均无编译警告，四组回归各 8/8 通过。回归组包含 `lab_session_tests`、`lab_replay_tests`、`lab_v6_udp_tests`、旧核心/压力/网络测试以及网络/UDP bot 冒烟，共 8 项；测试 presets 排除历史完整性能套件。

```bash
cmake --preset vcpkg-debug -DVCPKG_MANIFEST_INSTALL=OFF
CCACHE_DISABLE=1 cmake --build --preset build-debug
ctest --preset test-debug --timeout 120

cmake --preset vcpkg-release -DVCPKG_MANIFEST_INSTALL=OFF
CCACHE_DISABLE=1 cmake --build --preset build-release
ctest --preset test-release --timeout 120

cmake --preset vcpkg-debug -B build/v6-headless \
  -DVCPKG_MANIFEST_INSTALL=OFF -DLAB_BUILD_CLIENT=OFF \
  -DLAB_BUILD_PERFORMANCE=OFF -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build build/v6-headless -j 8
ctest --test-dir build/v6-headless --output-on-failure --timeout 120

cmake --preset vcpkg-debug -B build/v6-sanitize \
  -DVCPKG_MANIFEST_INSTALL=OFF -DLAB_BUILD_PERFORMANCE=OFF \
  -DCMAKE_CXX_COMPILER_LAUNCHER= \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build build/v6-sanitize -j 8
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/v6-sanitize --output-on-failure --timeout 120
```

本地 UDP 检查在允许 loopback 绑定/收发后运行。macOS 的 LeakSanitizer 未启用；报告只声称已执行 ASan/UBSan 检查，不以此证明零泄漏。额外检查无效字体路径的初始化失败退出，无访问错误。

```bash
python3 tools/sdl_smoke.py --build build/v6-sanitize --output build/sdl-smoke-v6-final
```

自动 SDL 检查使用 dummy 视频驱动，实际执行 libevent 服务端、两个 SDL 客户端、字体加载/软件渲染、真实 UDP、两端录制、回放窗口和退出；最终轮分别校验了 192/189 帧。它证明入口执行与控制逻辑，不替代人在物理窗口观察长延迟下的视觉手感。日志与录制保留在指定输出目录。

## 网络矩阵与可重复范围

[矩阵原始数据](v6_network_matrix.json) 保存每场配置、客户端统计、事件摘要和权威哈希。RTT 50/100/150ms ×丢包 0/1/5%，双向抖动 ±20ms、乱序 2%、重复 1%；每场 3000 tick，结束后正常网络 60 tick。协议/权威哈希错误为零，最终权威进度差不超过两帧，正常网络后一秒内恢复有效同步（本轮最大观测恢复时间 33.33ms）。

```bash
./build/vcpkg-release/lab_network_lab --matrix --json build/network-matrix-final.json
./build/vcpkg-release/lab_network_lab --ticks 300 --seed 42 \
  --trace build/v6-trace.json --json build/v6-trace-report.json
./build/vcpkg-release/lab_network_lab --replay-trace build/v6-trace.json
```

最终正确性修复一致的基线与优化后 9 场的事件摘要、哈希和去掉 `replayMs` 的全部功能统计一致。另有逐事件原始字节/发送时间/投递时间/丢弃标志重演对照。时间成本由真实单调时钟测量，不能由随机种子重现；真实 UDP 的内核调度也不作种子一致性承诺。

同帧误差是对应已完成帧的**本地玩家位置**差，使用预测历史与量化权威状态比较；不能拿不同帧的渲染位置当同步误差。位置校正计数阈值为 0.5mm，每个有效新快照都执行恢复重放，计数不决定是否校正。

## 性能口径、命令与原始数据

[基线原始数据](v6_performance_before.json) 与 [优化后原始数据](v6_performance_after.json) 包含机器信息、二进制 SHA-256、完整命令与三轮数据。优化版使用相同 Release preset、同一机器、相同场景与**关闭录制**设置。标准对照每档预热 5 秒、墙钟采样 30 秒、重复三轮，顺序运行 1/16/64 房间的 core 和 UDP 模式。

阶段 5 二进制和首轮数据保留在 `build/v6-performance/baseline` / `before`。最终检查补充了控制包 RTT、旧代次时间戳与边界校验修复，因此最终对照在独立源码副本中只撤回复制/容器优化，两组保留相同正确性逻辑；不拿旧二进制与新逻辑直接比较。重建脚本不会改写工作区源码。

```bash
python3 tools/prepare_v6_baseline.py
cmake --preset vcpkg-release \
  -S build/v6-performance/baseline-source -B build/v6-performance/baseline-build \
  -DVCPKG_MANIFEST_INSTALL=OFF -DLAB_BUILD_CLIENT=OFF -DLAB_BUILD_TESTS=OFF \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg_installed" -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build build/v6-performance/baseline-build -j 8
python3 tools/run_v6_benchmark.py \
  --binary build/v6-performance/baseline-build/lab_udp_bots \
  --output build/v6-performance/before-final
python3 tools/run_v6_benchmark.py \
  --binary build/vcpkg-release/lab_udp_bots \
  --output build/v6-performance/after-final
```

`core` 是不含 socket 系统调用的完整同步测试流程，计时包含客户端、服务端、编解码和校正；因此其总 tick 成本是该场景服务端推进成本的上界，**不是单独 World::Step 的极限容量**。`udp` 的总 tick 成本包含实际 loopback 收发与 bot；其 `serverTickP99Ms` 另外测量生产 ServerRuntime 的权威推进/发送。core 原始数据中的 `serverTickP99Ms=0` 是未测项，不用于结论。

标准矩阵与 bot 的 ACK RTT 都包含 60Hz 轮询接收等待；配置传播延迟不等于观测 RTT。分配次数由测试可执行文件的全局 new 计数，包含核心与测试流程，未统计 C 库直接 malloc；峰值 RSS 为 macOS getrusage 的进程高水位，包含同进程的客户端 bot、服务端和诊断容器，不是单独服务器内存；各档 bot 使用 256 帧历史，窗口客户端默认 4096 帧。吞吐同时列出收到的输入数据报与实际应用输入（包含保持/归零）；包字节数为 UDP 载荷，未计 IP/UDP 头。

实测机器为 Mac17,3 / Apple M5（10 核、16GiB），macOS 27.0.1（26A434）、arm64。两组均为同一 Release 构建配置，无录制。下表数值是三轮指标的中位数；耗时/分配变化为负数表示减少，RSS 为进程高水位。

| 模式 / 房间 | 基线 P99 ms | 优化 P99 ms | 耗时变化 | 分配变化 | RSS MiB 前 → 后 |
| --- | ---: | ---: | ---: | ---: | ---: |
| core / 1 | 0.119 | 0.122 | +2.1% | -6.7% | 9.23 → 9.23 |
| core / 16 | 1.636 | 1.484 | -9.3% | -6.9% | 12.67 → 10.91 |
| core / 64 | 6.279 | 6.049 | -3.7% | -6.9% | 40.83 → 32.94 |
| udp / 1 | 0.177 | 0.159 | -10.4% | -7.1% | 9.23 → 9.23 |
| udp / 16 | 2.406 | 1.548 | -35.7% | -6.7% | 12.95 → 11.16 |
| udp / 64 | 4.711 | 4.677 | -0.7% | -6.6% | 41.03 → 33.59 |

[汇总及逐轮 P99](v6_performance_comparison.json) 与两份原始数据同时保留。分配次数下降约 6.6%–7.1%；64 房间 core / UDP 的峰值 RSS 分别下降约 19.3% / 18.1%。保留共享地图与容器复用，因为分配和内存收益明确。1 房间 core P99 略升 2.1%，64 房间 UDP 中位数只下降 0.7%，不能据此声称这两档存在稳定 CPU 收益。16 房间 UDP 的三轮波动较大，中位数差异也不能当作可推广的固定加速比例。

| 优化 UDP 房间 | 接收输入报文 / 秒 | 实际采用输入 / 秒 | ACK RTT P50 / P99 ms | 服务端 tick P99 ms | 双向载荷 MB/s | 最大 deadline miss |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 120 | 120 | 33.30 / 50.05 | 0.053 | 0.134 | 0.000% |
| 16 | 1920 | 1920 | 16.66 / 16.78 | 0.580 | 2.154 | 0.000% |
| 64 | 7680 | 7680 | 16.67 / 16.78 | 2.339 | 8.604 | 0.167% |

**容量结论：**1/16 房间的全部优化版样本满足 P99 ≤16.67ms、deadline miss ≤0.1%；64 房间 UDP 三轮 P99 为 4.529 / 4.677 / 10.496ms，第三轮出现 0.167% deadline miss（3 / 1800 个采样），未满足全部验收预算。基线对应三轮 miss 为零。长尾原因尚未定位，不把它归因于某一性能改动或操作系统；64 是房间配置上限，本轮不能宣称其为稳定承载量。后续先增加生产服务端独立采样和编码/发送分段计时，再评估过载策略。核心模式全部样本符合预算，但不能替代实际 UDP 结论。

所有 36 个前后采样的协议/权威哈希错误为零。64 房间 UDP 双向全量快照载荷约 8.60MB/s（68.8Mbit/s，未计网络头）；优化没有带宽压缩目标，前后字节量接近。下一轮应依据带宽和编码分析决定是否改变协议，当前保持全量快照。录制开启时的容量与更长时间负载未纳入本轮性能结论。

## 后续可读性整理验证

在六阶段升级后，局部整理了 `session` 的六组实现/头文件：明确局部变量和阈值命名，将客户端接收逻辑拆为欢迎初始化与权威校正两个内部函数，头文件按职责分组，并补充控制请求、完整重放、房间到期、显示副本及回放文件尾的中文说明。公开接口、字段/成员顺序、v6 格式、校验顺序及阈值保持不变。

本轮 Debug 构建无警告，8/8 回归通过；9 场完整网络矩阵与整理前的事件摘要、世界哈希、报文/字节数及全部功能统计一致（实测 `replayMs` 除外），旧逐事件轨迹重演通过。同步调整性能基线生成脚本并增加匹配缺失时的明确报错；生成后的 Release 基线无警告，与当前实现的 9 场功能数据一致。相关源码的格式检查通过。

本轮未重跑标准性能对照，上述性能原始数据与 SHA-256 保留为六阶段升级时的实测记录；不能视为整理后重新测量的结果。

## 保留的边界与后续优先级

1. 首先扩展畸形数据报、长断线、地址变化和录制积压的组合回归；对其他编译器/平台做原始回放字段对照。
2. 根据同版本结果继续分析 JSON 构造/编码与全量地图带宽；输出数据报需要独立生命周期，首轮没有引入复杂缓冲池或改变哈希字段顺序。
3. 对物理窗口做同种子手感对照和录屏，再评估任意回放跳转与观战。

保留全量快照，不实现增量压缩。公网认证、部署、账号、匹配与玩法扩展不属于本轮；同一 macOS 机器通过不代表跨平台浮点完全确定。旧 v5 报告不作为新版收益证据。
