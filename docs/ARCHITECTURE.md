# 架构总览

顶视角 60Hz 回滚同步的小型“迷宫坦克”对战。Server 负责权威推进与状态下发；Client 做本地预测、回滚重放、渲染与输入采样。核心代码分布：
- `include/lab/sim` / `src/sim`：世界状态 (`WorldSnapshot`)、世界推进 `World`、输入缓冲/历史、哈希。
- `include/lab/net` / `src/net`：UDP 封装，Input/Ack/State/Start 报文编解码（当前协议版本 v3）。
- `apps/server_main.cpp`：权威帧循环、握手、缺帧策略、状态广播。
- `apps/client_main.cpp`：输入采集、预测/回滚、hash 对账、SDL 渲染 HUD。
- `include/lab/app` / `src/app`：全局常量（`GameConfig`）、远端输入预测、渲染。

# 设计模式与架构风格

- **分层/职责分离**：网络、模拟、渲染三层解耦，应用层只拼装流程。
- **数据导向**：`WorldSnapshot` 作为纯数据结构在网络/哈希/回滚间传递，避免耦合逻辑。
- **纯函数式推进**：`World::Step` 输入确定 -> 输出确定，有利于回滚与一致性。
- **事件驱动**：libevent 驱动网络收发与 tick 定时，SDL 事件驱动输入采样。
- **冗余与防抖**：输入包携带冗余，服务器 tick accumulator 防抖；客户端回滚阈值放宽。
- **配置集中**：帧长、人数、冗余、开局延迟等集中在 `GameConfig`，便于调优。

# 网络通信

## 报文格式
- `Start`（Server→Client）：分配 playerId、`totalPlayers`、`startTick`。
- `Input`（Client→Server）：`playerId` 自描述、冗余 K 帧输入、`seq`、`newestTick`、`clientAckServerTick`。
- `Ack`（Server→Client）：`serverTickProcessed`、`serverLastInputTick`、`serverStateHash`（权威哈希，用于对账）。
- `State`（Server→Client）：`tick`、`mazeSeed`、玩家数组（位置/速度/HP/动作/计时/命中/地面/射击冷却/瞄准方向）、弹道数组（pos/vel/owner/life）、`stateHash`。

## 流程
1. Server 启动监听，保持非阻塞 UDP + libevent 定时 tick。
2. Client 启动后周期发送空 `Input` 请求 slot；Server 收齐 `kRequiredPlayers` 后发 `Start`，所有端对齐 `startTick`。
3. Server 每 tick 按收到的输入推进世界（缺帧时 Hold 若干 tick，超出则默认空输入），每 `kStateEvery` 帧下发 `State`，每帧下发 `Ack`。
4. Client 固定 dt 采样键盘 → 本地预测世界；收到 `State` 应用权威、必要时回滚；收到 `Ack` 仅在本地已有权威快照时做哈希对账。

# 回滚与预测

- 输入缓冲：`InputBuffer` 环形存储，客户端本地/远端输入分开；远端缺帧可 Hold 上一次若干 tick。
- 预测：本地玩家用真实输入，远端用 `InputPrediction` 根据速度猜测 moveX/moveY。
- 状态历史：`StateHistory` 环形保存每 tick 的预测/权威快照，供回滚与哈希使用。
- 回滚判定：仅比对“本地玩家”与权威的差异（位置阈值 0.15m + HP/动作/落地），远端不参与判定，避免误触发。
- 回滚流程：收到新的权威 State 后忽略旧包；恢复权威 tick → 重放后续输入到当前 tick，再继续预测。即使本地玩家误差未触发回滚计数，也会 rebase + replay，以便远端玩家/弹道立即采用权威状态。
- 哈希对账：State 直接校验 `stateHash`，Ack 只在本地已存同 tick 权威快照时比较。`Hasher` 使用网络包相同的毫米量化精度和稳定整数混合，避免服务端原始 float 与客户端解码 float 产生假 mismatch。

# 世界逻辑

- 拓扑：固定 seed DFS 生成的迷宫网格（奇数宽高），`mazeSeed` 通过 State 下发，墙体用于玩家/弹道碰撞。
- 玩家：顶视角 2D 移动，无重力，摩擦插值速度，半径 ~0.35m，墙体/推箱碰撞分离，朝向基于最近的移动方向。攻击键发射子弹（朝最后方向），冷却与寿命有限，命中造成 HP 减少与短暂无敌/硬直。
- 弹道：直线运动，撞墙或命中即移除；状态在权威/预测中同步。
- 哈希：`Hasher` 对世界快照（玩家、弹道、迷宫元数据）做 FNV 混合，供一致性检查。射击冷却和瞄准方向属于确定性状态，会进入快照、网络包与哈希。

# 渲染与调试

- SDL2 + SDL_ttf 简易鸟瞰渲染：迷宫墙体、玩家方块、弹道点。HUD 显示 tick、回滚次数、hash mismatch、每玩家 hp/动作/计时、maze seed。
- 输入：键盘 `A/D/←/→` 水平，`W/S/↑/↓` 垂直，`Space/J/K` 开火。
- 典型运行：`./build/lab_server` 后启动两个 `./build/lab_client`，观察 HUD 与迷宫碰撞/弹道命中是否一致。
- 回归测试：`ctest --test-dir build --output-on-failure`。
- 压力测试：`./build/lab_stress --ticks 200000 --history 8192 --state-delay 12`。
