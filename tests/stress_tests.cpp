#include <lab/net/NetCodec.h>
#include <lab/sim/Hasher.h>
#include <lab/sim/InputBuffer.h>
#include <lab/sim/StateHistory.h>
#include <lab/sim/World.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
  Tick ticks = 60000;
  uint8_t players = 2;
  size_t history = 4096;
  Tick stateEvery = 2;
  Tick stateDelay = 7;
  int redundancy = 8;
};

struct PendingState {
  Tick deliverTick = 0;
  std::vector<uint8_t> bytes;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "stress FAIL: " << message << "\n";
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

uint32_t ParseU32(const char* value, const char* name) {
  try {
    const auto parsed = std::stoul(value);
    if (parsed > UINT32_MAX) throw std::out_of_range(name);
    return static_cast<uint32_t>(parsed);
  } catch (const std::exception&) {
    Fail(std::string("invalid ") + name + ": " + value);
  }
}

size_t ParseSize(const char* value, const char* name) {
  try {
    return static_cast<size_t>(std::stoull(value));
  } catch (const std::exception&) {
    Fail(std::string("invalid ") + name + ": " + value);
  }
}

void PrintUsage(const char* argv0) {
  std::cout
      << "Usage: " << argv0 << " [--ticks N] [--players N] [--history N]\n"
      << "       [--state-every N] [--state-delay N] [--redundancy N]\n";
}

Options ParseOptions(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc) Fail(std::string("missing value for ") + name);
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (arg == "--ticks") {
      opt.ticks = ParseU32(needValue("--ticks"), "--ticks");
    } else if (arg == "--players") {
      const uint32_t players = ParseU32(needValue("--players"), "--players");
      Require(players >= 1 && players <= 8, "--players must be in [1, 8]");
      opt.players = static_cast<uint8_t>(players);
    } else if (arg == "--history") {
      opt.history = ParseSize(needValue("--history"), "--history");
    } else if (arg == "--state-every") {
      opt.stateEvery = ParseU32(needValue("--state-every"), "--state-every");
    } else if (arg == "--state-delay") {
      opt.stateDelay = ParseU32(needValue("--state-delay"), "--state-delay");
    } else if (arg == "--redundancy") {
      const uint32_t redundancy = ParseU32(needValue("--redundancy"), "--redundancy");
      Require(redundancy >= 1 && redundancy <= 255, "--redundancy must be in [1, 255]");
      opt.redundancy = static_cast<int>(redundancy);
    } else {
      Fail("unknown argument: " + arg);
    }
  }

  Require(opt.ticks > 0, "--ticks must be > 0");
  Require(opt.history > opt.stateDelay + 16, "--history must be larger than replay delay window");
  Require(opt.stateEvery > 0, "--state-every must be > 0");
  return opt;
}

InputCmd MakeCmd(Tick tick, uint8_t playerIndex) {
  InputCmd cmd{};
  cmd.tick = tick;

  const uint32_t phase = ((tick / (11 + playerIndex * 3)) + playerIndex) % 5;
  switch (phase) {
    case 0: cmd.moveX = 1; break;
    case 1: cmd.moveY = 1; break;
    case 2: cmd.moveX = -1; break;
    case 3: cmd.moveY = -1; break;
    default: break;
  }

  const Tick attackPeriod = 23 + Tick(playerIndex) * 7;
  if (((tick + Tick(playerIndex) * 5) % attackPeriod) == 0) {
    cmd.buttons |= BIN_ATK;
  }

  return cmd;
}

InputCmd PredictCmd(const InputCmd& truth, const InputCmd& previousPrediction, Tick tick, uint8_t playerIndex) {
  if (playerIndex == 0) return truth;

  InputCmd predicted = previousPrediction;
  predicted.tick = tick;
  predicted.buttons = 0;

  const uint32_t mode = (tick + playerIndex * 13) % 19;
  if (mode < 9) {
    return predicted;
  }
  if (mode < 14) {
    return InputBuffer::DefaultForTick(tick);
  }
  return truth;
}

int32_t ToMM(float value) {
  return static_cast<int32_t>(std::lround(value * 1000.0f));
}

float FromMM(int32_t mm) {
  return static_cast<float>(mm) / 1000.0f;
}

lab::net::StatePacket SnapshotToState(const WorldSnapshot& snap, uint8_t recipientPlayer) {
  lab::net::StatePacket st{};
  st.playerId = recipientPlayer;
  st.tick = snap.tick;
  st.mazeSeed = snap.mazeSeed;
  st.playerCount = static_cast<uint8_t>(std::min<size_t>(snap.players.size(), 255));
  st.projectileCount = static_cast<uint8_t>(std::min<size_t>(snap.projectiles.size(), 255));
  st.stateHash = Hasher::Hash(snap);

  st.players.reserve(st.playerCount);
  for (uint8_t i = 0; i < st.playerCount; ++i) {
    const auto& p = snap.players[i];
    lab::net::PackedPlayerState ps{};
    ps.x_mm = ToMM(p.x);
    ps.v_mm = ToMM(p.v);
    ps.y_mm = ToMM(p.y);
    ps.vy_mm = ToMM(p.vy);
    ps.hp = p.hp;
    ps.action = static_cast<uint8_t>(p.action);
    ps.facing = p.facing;
    ps.stateTimer = p.stateTimer;
    ps.atkActive = p.atkActive;
    ps.attackConnected = p.attackConnected;
    ps.onGround = p.onGround;
    ps.shotCooldown = p.shotCooldown;
    ps.aimX = p.aimX;
    ps.aimY = p.aimY;
    st.players.push_back(ps);
  }

  st.projectiles.reserve(st.projectileCount);
  for (uint8_t i = 0; i < st.projectileCount; ++i) {
    const auto& p = snap.projectiles[i];
    lab::net::PackedProjectile pr{};
    pr.x_mm = ToMM(p.x);
    pr.y_mm = ToMM(p.y);
    pr.vx_mm = ToMM(p.vx);
    pr.vy_mm = ToMM(p.vy);
    pr.owner = p.owner;
    pr.life = p.life;
    st.projectiles.push_back(pr);
  }

  return st;
}

Action ToAction(uint8_t value) {
  switch (value) {
    case 1: return Action::Attack;
    case 2: return Action::Hitstun;
    default: return Action::Idle;
  }
}

WorldSnapshot StateToSnapshot(const lab::net::StatePacket& st, uint8_t playerCount) {
  lab::sim::World seedWorld(playerCount);
  seedWorld.SetMazeSeed(st.mazeSeed, true);
  WorldSnapshot snap = seedWorld.Snapshot();
  snap.tick = st.tick;
  snap.players.resize(playerCount);

  const size_t count = std::min<size_t>(st.players.size(), playerCount);
  for (size_t i = 0; i < count; ++i) {
    const auto& ps = st.players[i];
    auto& p = snap.players[i];
    p.x = FromMM(ps.x_mm);
    p.v = FromMM(ps.v_mm);
    p.y = FromMM(ps.y_mm);
    p.vy = FromMM(ps.vy_mm);
    p.hp = ps.hp;
    p.action = ToAction(ps.action);
    p.facing = ps.facing;
    p.stateTimer = ps.stateTimer;
    p.atkActive = ps.atkActive;
    p.attackConnected = ps.attackConnected;
    p.onGround = ps.onGround;
    p.shotCooldown = ps.shotCooldown;
    p.aimX = ps.aimX;
    p.aimY = ps.aimY;
  }

  snap.projectiles.clear();
  snap.projectiles.reserve(st.projectiles.size());
  for (const auto& pr : st.projectiles) {
    ProjectileState out{};
    out.x = FromMM(pr.x_mm);
    out.y = FromMM(pr.y_mm);
    out.vx = FromMM(pr.vx_mm);
    out.vy = FromMM(pr.vy_mm);
    out.life = pr.life;
    out.owner = pr.owner;
    out.alive = pr.life > 0 ? 1 : 0;
    snap.projectiles.push_back(out);
  }

  return snap;
}

WorldSnapshot ReplayFrom(lab::sim::World& world,
                         const WorldSnapshot& auth,
                         Tick currentTick,
                         const std::vector<InputBuffer>& truthHistory,
                         lab::sim::StateHistory& replayHistory) {
  world.Restore(auth);
  for (Tick tick = auth.tick + 1; tick <= currentTick; ++tick) {
    std::vector<InputCmd> cmds;
    cmds.reserve(truthHistory.size());
    for (const auto& hist : truthHistory) {
      cmds.push_back(hist.Get(tick).value_or(InputBuffer::DefaultForTick(tick)));
    }
    world.Step(cmds, 1.0f / 60.0f);
    replayHistory.Put(world.Snapshot());
  }
  return world.Snapshot();
}

void ExerciseInputCodec(const std::vector<InputBuffer>& truthHistory,
                        Tick tick,
                        const Options& opt,
                        uint64_t& packetCount,
                        uint64_t& byteCount) {
  for (uint8_t player = 0; player < opt.players; ++player) {
    lab::net::InputPacket p{};
    p.playerId = static_cast<uint8_t>(player + 1);
    p.seq = tick;
    p.newestTick = tick;
    p.clientAckServerTick = tick > opt.stateDelay ? tick - opt.stateDelay : 0;
    p.cmds.reserve(static_cast<size_t>(opt.redundancy));
    for (int i = 0; i < opt.redundancy; ++i) {
      const Tick t = tick >= static_cast<Tick>(i) ? tick - static_cast<Tick>(i) : 0;
      p.cmds.push_back(truthHistory[player].Get(t).value_or(InputBuffer::DefaultForTick(t)));
    }
    p.count = static_cast<uint8_t>(p.cmds.size());

    const auto bytes = lab::net::EncodeInput(p);
    const auto decoded = lab::net::DecodeInput(bytes.data(), bytes.size());
    Require(decoded.has_value(), "DecodeInput failed at tick " + std::to_string(tick));
    Require(decoded->count == p.cmds.size(), "Input count mismatch at tick " + std::to_string(tick));
    Require(decoded->newestTick == tick, "Input newestTick mismatch at tick " + std::to_string(tick));
    packetCount++;
    byteCount += bytes.size();
  }
}

} // namespace

int main(int argc, char** argv) {
  const Options opt = ParseOptions(argc, argv);
  constexpr uint32_t kMazeSeed = 20240625u;

  lab::sim::World authority(opt.players);
  lab::sim::World client(opt.players);
  authority.SetMazeSeed(kMazeSeed, true);
  client.SetMazeSeed(kMazeSeed, true);

  std::vector<InputBuffer> truthHistory;
  truthHistory.reserve(opt.players);
  for (uint8_t i = 0; i < opt.players; ++i) {
    truthHistory.emplace_back(opt.history);
  }

  lab::sim::StateHistory authorityHistory(opt.history);
  lab::sim::StateHistory clientHistory(opt.history);
  std::vector<InputCmd> previousPrediction(opt.players, InputBuffer::DefaultForTick(0));
  std::deque<PendingState> pendingStates;

  uint64_t inputPackets = 0;
  uint64_t statePackets = 0;
  uint64_t networkBytes = 0;
  uint64_t replays = 0;
  uint64_t replayedTicks = 0;
  uint64_t hashChecks = 0;
  uint64_t rawRestoreChecks = 0;

  const auto started = std::chrono::steady_clock::now();

  for (Tick tick = 0; tick < opt.ticks; ++tick) {
    std::vector<InputCmd> truthCmds;
    std::vector<InputCmd> predictedCmds;
    truthCmds.reserve(opt.players);
    predictedCmds.reserve(opt.players);

    for (uint8_t player = 0; player < opt.players; ++player) {
      const InputCmd truth = MakeCmd(tick, player);
      truthHistory[player].Put(truth);
      truthCmds.push_back(truth);

      InputCmd predicted = PredictCmd(truth, previousPrediction[player], tick, player);
      predicted.tick = tick;
      predictedCmds.push_back(predicted);
      previousPrediction[player] = predicted;
    }

    ExerciseInputCodec(truthHistory, tick, opt, inputPackets, networkBytes);

    authority.Step(truthCmds, 1.0f / 60.0f);
    client.Step(predictedCmds, 1.0f / 60.0f);

    const WorldSnapshot authoritySnap = authority.Snapshot();
    authorityHistory.Put(authoritySnap);
    clientHistory.Put(client.Snapshot());

    if (tick % opt.stateEvery == 0) {
      const lab::net::StatePacket st = SnapshotToState(authoritySnap, 1);
      const auto bytes = lab::net::EncodeState(st);
      const Tick jitter = (tick / opt.stateEvery) % 3;
      pendingStates.push_back(PendingState{tick + opt.stateDelay + jitter, bytes});
      statePackets++;
      networkBytes += bytes.size();
    }

    while (!pendingStates.empty() && pendingStates.front().deliverTick <= tick) {
      const auto bytes = pendingStates.front().bytes;
      pendingStates.pop_front();

      const auto decoded = lab::net::DecodeState(bytes.data(), bytes.size());
      Require(decoded.has_value(), "DecodeState failed at tick " + std::to_string(tick));

      const WorldSnapshot auth = StateToSnapshot(*decoded, opt.players);
      Require(Hasher::Hash(auth) == decoded->stateHash,
              "decoded State hash mismatch for auth tick " + std::to_string(decoded->tick));

      const auto rawAuth = authorityHistory.Get(decoded->tick);
      Require(rawAuth.has_value(), "authority history missing auth tick " + std::to_string(decoded->tick));
      lab::sim::World rawReplay(opt.players);
      lab::sim::StateHistory rawReplayHistory(opt.history);
      const WorldSnapshot rawReplaySnap = ReplayFrom(rawReplay, *rawAuth, tick, truthHistory, rawReplayHistory);

      const auto rawExpected = authorityHistory.Get(tick);
      Require(rawExpected.has_value(), "authority history missing current tick " + std::to_string(tick));
      if (Hasher::Hash(rawReplaySnap) != Hasher::Hash(*rawExpected)) {
        Fail("raw restore/replay mismatch at tick " + std::to_string(tick) +
             " authTick=" + std::to_string(rawAuth->tick));
      }
      rawRestoreChecks++;

      const WorldSnapshot clientSnap = ReplayFrom(client, auth, tick, truthHistory, clientHistory);
      replays++;
      replayedTicks += tick - auth.tick;

      lab::sim::World networkExpected(opt.players);
      lab::sim::StateHistory networkExpectedHistory(opt.history);
      const WorldSnapshot networkExpectedSnap = ReplayFrom(
          networkExpected, auth, tick, truthHistory, networkExpectedHistory);

      const uint64_t clientHash = Hasher::Hash(clientSnap);
      const uint64_t authorityHash = Hasher::Hash(networkExpectedSnap);
      hashChecks++;
      if (clientHash != authorityHash) {
        Fail("post-replay hash mismatch at tick " + std::to_string(tick) +
             " authTick=" + std::to_string(auth.tick) +
             " client=" + std::to_string(clientHash) +
             " expected=" + std::to_string(authorityHash));
      }
    }
  }

  const auto finished = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(finished - started).count();
  const double ticksPerSecond = seconds > 0.0 ? double(opt.ticks) / seconds : 0.0;

  std::cout << "stress OK"
            << " ticks=" << opt.ticks
            << " players=" << int(opt.players)
            << " inputPackets=" << inputPackets
            << " statePackets=" << statePackets
            << " bytes=" << networkBytes
            << " replays=" << replays
            << " replayedTicks=" << replayedTicks
            << " hashChecks=" << hashChecks
            << " rawRestoreChecks=" << rawRestoreChecks
            << " timeSec=" << seconds
            << " ticksPerSec=" << ticksPerSecond
            << "\n";

  return 0;
}
