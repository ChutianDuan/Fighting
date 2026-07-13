#include "PerfStats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lab/net/NetCodec.h>
#include <lab/net/UdpSocket.h>
#include <lab/server/AuthoritativeServer.h>
#include <lab/sim/Hasher.h>

namespace {

using lab::perf::Clock;
using lab::perf::Distribution;
using lab::server::AuthoritativeServer;
using lab::server::OutboundDatagram;

constexpr double kTickBudgetMs = 1000.0 / 60.0;
constexpr double kMaxDeadlineMissPct = 0.1;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "performance FAIL: " << message << "\n";
  std::exit(1);
}

struct Options {
  enum class Profile { Smoke, Standard } profile = Profile::Standard;
  enum class Mode { All, Capacity, Udp } mode = Mode::All;
  std::string jsonPath;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) Fail("missing value for " + argument);
      return argv[++i];
    };
    if (argument == "--profile") {
      const std::string profile = value();
      if (profile == "smoke") options.profile = Options::Profile::Smoke;
      else if (profile == "standard") options.profile = Options::Profile::Standard;
      else Fail("invalid profile: " + profile);
    } else if (argument == "--mode") {
      const std::string mode = value();
      if (mode == "all") options.mode = Options::Mode::All;
      else if (mode == "capacity") options.mode = Options::Mode::Capacity;
      else if (mode == "udp") options.mode = Options::Mode::Udp;
      else Fail("invalid mode: " + mode);
    } else if (argument == "--json") {
      options.jsonPath = value();
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: lab_performance [--profile smoke|standard] "
                   "[--mode all|capacity|udp] [--json PATH]\n";
      std::exit(0);
    } else {
      Fail("unknown argument: " + argument);
    }
  }
  return options;
}

uint64_t PhysicalMemoryBytes() {
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  if (pages <= 0 || pageSize <= 0) return 0;
  return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
}

lab::net::UdpAddr Address(uint16_t port) {
  return lab::net::UdpAddr::FromIPv4("127.0.0.1", port);
}

struct RoomClient {
  uint8_t playerId = 0;
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
  uint32_t sequence = 0;
};

struct Room {
  AuthoritativeServer server{20240625u, false};
  std::array<lab::net::UdpAddr, 2> addresses{Address(31001), Address(31002)};
  std::array<RoomClient, 2> clients{};

  Room() {
    for (size_t i = 0; i < clients.size(); ++i) {
      lab::net::InputPacket hello{};
      const auto bytes = lab::net::EncodeInput(hello);
      const auto output = server.HandleDatagram(
          addresses[i], bytes.data(), bytes.size(), 0.0);
      for (const auto& datagram : output) {
        auto start = lab::net::DecodeStart(datagram.bytes.data(), datagram.bytes.size());
        if (!start || start->playerId == 0 || start->playerId > clients.size()) continue;
        auto& client = clients[start->playerId - 1];
        client.playerId = start->playerId;
        client.sessionId = start->sessionId;
        client.matchId = start->matchId;
      }
    }
    if (!server.started()) Fail("capacity room failed to start");
    for (const auto& client : clients) {
      if (client.sessionId == 0 || client.matchId == 0) {
        Fail("capacity room missing Start identity");
      }
    }
  }
};

std::vector<uint8_t> MakeInput(RoomClient& client, Tick tick) {
  lab::net::InputPacket packet{};
  packet.playerId = client.playerId;
  packet.sessionId = client.sessionId;
  packet.matchId = client.matchId;
  packet.seq = client.sequence++;
  packet.newestTick = tick;
  packet.clientAckServerTick = tick > 2 ? tick - 2 : 0;
  for (int redundancy = 0; redundancy < 4; ++redundancy) {
    InputCmd command{};
    command.tick = tick >= Tick(redundancy) ? tick - Tick(redundancy) : 0;
    command.moveX = ((tick / 30 + client.playerId) % 2 == 0) ? 1 : -1;
    if ((tick + client.playerId) % 37 == 0) command.buttons = BIN_ATK;
    packet.cmds.push_back(command);
  }
  return lab::net::EncodeInput(packet);
}

struct CapacityRun {
  size_t rooms = 0;
  bool finalValidation = false;
  bool soak = false;
  Distribution frameMs{};
  double wallSec = 0.0;
  double deadlineMissPct = 0.0;
  double processCpuPct = 0.0;
  double projectedRealtimeCpuPct = 0.0;
  double benchmarkFramesPerSec = 0.0;
  double realtimeInputPps = 0.0;
  double realtimeOutputPps = 0.0;
  double realtimeOutputMbps = 0.0;
  uint64_t errors = 0;
  uint64_t frames = 0;
  uint64_t rssBeforeSetup = 0;
  uint64_t rssAfterSetup = 0;
  uint64_t rssAfterWarmup = 0;
  uint64_t rssEnd = 0;
  uint64_t peakRss = 0;
  double rssGrowthPct = 0.0;
  double rssGrowthMbPerMin = 0.0;
};

bool CapacityPasses(const CapacityRun& run, uint64_t minimumSamples) {
  return run.errors == 0 && run.frameMs.samples >= minimumSamples &&
         run.frameMs.p99 <= kTickBudgetMs &&
         run.deadlineMissPct <= kMaxDeadlineMissPct;
}

CapacityRun RunCapacity(size_t roomCount,
                        double warmupSec,
                        double measureSec,
                        uint64_t minimumSamples,
                        bool finalValidation,
                        bool soak) {
  CapacityRun result{};
  result.rooms = roomCount;
  result.finalValidation = finalValidation;
  result.soak = soak;
  result.rssBeforeSetup = lab::perf::CurrentRssBytes();

  std::vector<std::unique_ptr<Room>> rooms;
  rooms.reserve(roomCount);
  for (size_t i = 0; i < roomCount; ++i) rooms.push_back(std::make_unique<Room>());
  result.rssAfterSetup = lab::perf::CurrentRssBytes();

  std::vector<std::array<std::vector<uint8_t>, 2>> inputs(roomCount);
  std::vector<std::vector<OutboundDatagram>> outputs(roomCount);
  uint64_t frameIndex = 0;

  auto executeFrame = [&](bool collect, std::vector<double>& samples,
                          uint64_t& deadlineMisses, uint64_t& outputPackets,
                          uint64_t& outputBytes, uint64_t& errors) {
    for (size_t roomIndex = 0; roomIndex < rooms.size(); ++roomIndex) {
      const Tick tick = rooms[roomIndex]->server.tick();
      for (size_t player = 0; player < 2; ++player) {
        inputs[roomIndex][player] = MakeInput(rooms[roomIndex]->clients[player], tick);
      }
    }

    const double simulatedNow = static_cast<double>(frameIndex) / 60.0;
    const auto frameStarted = Clock::now();
    for (size_t roomIndex = 0; roomIndex < rooms.size(); ++roomIndex) {
      Room& room = *rooms[roomIndex];
      for (size_t player = 0; player < 2; ++player) {
        const auto& bytes = inputs[roomIndex][player];
        auto response = room.server.HandleDatagram(
            room.addresses[player], bytes.data(), bytes.size(), simulatedNow);
        if (!response.empty()) ++errors;
      }
      outputs[roomIndex] = room.server.AdvanceOneTick();
    }
    const double frameMs = lab::perf::Milliseconds(Clock::now() - frameStarted);

    if (collect) {
      samples.push_back(frameMs);
      if (frameMs > kTickBudgetMs) ++deadlineMisses;
      for (size_t roomIndex = 0; roomIndex < rooms.size(); ++roomIndex) {
        const uint64_t expectedHash = Hasher::Hash(rooms[roomIndex]->server.snapshot());
        for (const auto& datagram : outputs[roomIndex]) {
          ++outputPackets;
          outputBytes += datagram.bytes.size();
          if (auto state = lab::net::DecodeState(
                  datagram.bytes.data(), datagram.bytes.size())) {
            if (state->stateHash != expectedHash) ++errors;
          } else if (!lab::net::DecodeAck(datagram.bytes.data(), datagram.bytes.size())) {
            ++errors;
          }
        }
      }
    }
    ++frameIndex;
  };

  std::vector<double> ignored;
  uint64_t ignoredCount = 0;
  const auto frameInterval = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / 60.0));
  auto nextFrame = Clock::now();
  const auto warmupEnd = nextFrame + std::chrono::duration<double>(warmupSec);
  while (Clock::now() < warmupEnd) {
    executeFrame(false, ignored, ignoredCount, ignoredCount, ignoredCount, ignoredCount);
    nextFrame += frameInterval;
    if (Clock::now() < nextFrame) std::this_thread::sleep_until(nextFrame);
  }
  result.rssAfterWarmup = lab::perf::CurrentRssBytes();

  std::vector<double> frameSamples;
  frameSamples.reserve(static_cast<size_t>(minimumSamples + 256));
  uint64_t deadlineMisses = 0;
  uint64_t outputPackets = 0;
  uint64_t outputBytes = 0;
  uint64_t errors = 0;
  const auto usageBefore = lab::perf::ReadResourceUsage();
  const auto measuredStart = Clock::now();
  nextFrame = measuredStart;
  const auto measuredEnd = measuredStart + std::chrono::duration<double>(measureSec);
  const auto hardEnd = measuredStart + std::chrono::duration<double>(measureSec * 1.5 + 1.0);
  while ((Clock::now() < measuredEnd || frameSamples.size() < minimumSamples) &&
         Clock::now() < hardEnd) {
    executeFrame(true, frameSamples, deadlineMisses, outputPackets, outputBytes, errors);
    nextFrame += frameInterval;
    if (Clock::now() < nextFrame) std::this_thread::sleep_until(nextFrame);
  }
  result.wallSec = std::chrono::duration<double>(Clock::now() - measuredStart).count();
  const auto usageAfter = lab::perf::ReadResourceUsage();

  result.frameMs = lab::perf::Summarize(std::move(frameSamples));
  result.frames = result.frameMs.samples;
  result.errors = errors;
  result.deadlineMissPct = result.frames > 0
      ? 100.0 * static_cast<double>(deadlineMisses) / static_cast<double>(result.frames)
      : 100.0;
  const double cpuSec = (usageAfter.userSec - usageBefore.userSec) +
                        (usageAfter.systemSec - usageBefore.systemSec);
  result.processCpuPct = result.wallSec > 0.0 ? cpuSec * 100.0 / result.wallSec : 0.0;
  result.projectedRealtimeCpuPct = result.frameMs.mean * 100.0 / kTickBudgetMs;
  result.benchmarkFramesPerSec = result.wallSec > 0.0
      ? static_cast<double>(result.frames) / result.wallSec : 0.0;
  result.realtimeInputPps = static_cast<double>(roomCount) * 2.0 * 60.0;
  result.realtimeOutputPps = result.frames > 0
      ? static_cast<double>(outputPackets) * 60.0 / static_cast<double>(result.frames) : 0.0;
  result.realtimeOutputMbps = result.frames > 0
      ? static_cast<double>(outputBytes) * 60.0 * 8.0 /
            static_cast<double>(result.frames) / 1'000'000.0
      : 0.0;
  result.rssEnd = usageAfter.currentRssBytes;
  result.peakRss = usageAfter.peakRssBytes;
  if (result.rssAfterWarmup > 0) {
    const double growthBytes = static_cast<double>(result.rssEnd) -
                               static_cast<double>(result.rssAfterWarmup);
    result.rssGrowthPct = 100.0 *
        growthBytes / static_cast<double>(result.rssAfterWarmup);
    result.rssGrowthMbPerMin =
        growthBytes / (1024.0 * 1024.0) * 60.0 / std::max(0.001, result.wallSec);
  }
  return result;
}

struct CapacitySuite {
  std::vector<CapacityRun> runs;
  size_t maxStableRooms = 0;
  size_t firstFailRooms = 0;
  size_t safeRooms = 0;
  size_t memoryLimitRooms = 0;
  double bytesPerRoom = 0.0;
  bool passed = false;
};

CapacitySuite RunCapacitySuite(bool standard) {
  CapacitySuite suite;
  const double quickWarmup = standard ? 0.25 : 0.02;
  const double quickMeasure = standard ? 1.0 : 0.08;
  const uint64_t quickSamples = standard ? 50 : 5;
  const size_t maximumRooms = standard ? 16384 : 64;

  size_t low = 0;
  size_t high = 1;
  while (high <= maximumRooms) {
    auto run = RunCapacity(high, quickWarmup, quickMeasure,
                           quickSamples, false, false);
    const bool pass = CapacityPasses(run, quickSamples);
    suite.runs.push_back(std::move(run));
    if (!pass) break;
    low = high;
    high *= 2;
  }
  if (high > maximumRooms) high = maximumRooms;
  if (low == high) {
    suite.firstFailRooms = 0;
  } else {
    while (high > low + std::max<size_t>(1, low / 10)) {
      const size_t middle = low + (high - low) / 2;
      auto run = RunCapacity(middle, quickWarmup, quickMeasure,
                             quickSamples, false, false);
      const bool pass = CapacityPasses(run, quickSamples);
      suite.runs.push_back(std::move(run));
      if (pass) low = middle;
      else high = middle;
    }
    suite.firstFailRooms = high;
  }

  const double finalWarmup = standard ? 5.0 : 0.05;
  const double finalMeasure = standard ? 20.0 : 0.25;
  const uint64_t finalSamples = standard ? 1000 : 50;
  const int repeats = standard ? 3 : 1;
  size_t candidate = std::max<size_t>(1, low);
  for (;;) {
    bool allPassed = true;
    for (int repeat = 0; repeat < repeats; ++repeat) {
      auto run = RunCapacity(candidate, finalWarmup, finalMeasure,
                             finalSamples, true, false);
      allPassed = allPassed && CapacityPasses(run, finalSamples);
      suite.runs.push_back(std::move(run));
    }
    if (allPassed || candidate == 1) break;
    candidate = std::max<size_t>(1, candidate * 8 / 10);
  }
  suite.maxStableRooms = candidate;

  size_t failCandidate = suite.firstFailRooms;
  if (failCandidate <= suite.maxStableRooms) {
    failCandidate = suite.maxStableRooms + std::max<size_t>(1, suite.maxStableRooms / 5);
  }
  failCandidate = std::min(failCandidate, maximumRooms);
  bool observedFailure = false;
  while (!observedFailure) {
    bool allPassed = true;
    for (int repeat = 0; repeat < repeats; ++repeat) {
      auto run = RunCapacity(failCandidate, finalWarmup, finalMeasure,
                             finalSamples, true, false);
      allPassed = allPassed && CapacityPasses(run, finalSamples);
      suite.runs.push_back(std::move(run));
    }
    if (!allPassed) {
      observedFailure = true;
      suite.firstFailRooms = failCandidate;
      break;
    }
    suite.maxStableRooms = std::max(suite.maxStableRooms, failCandidate);
    if (failCandidate >= maximumRooms) break;
    failCandidate = std::min(maximumRooms,
        std::max(failCandidate + 1, failCandidate * 3 / 2));
  }

  const size_t cpuSafeRooms = std::max<size_t>(1, suite.maxStableRooms * 8 / 10);
  const double soakWarmup = standard ? 5.0 : 0.02;
  const double soakMeasure = standard ? 60.0 : 0.20;
  auto soak = RunCapacity(cpuSafeRooms, soakWarmup, soakMeasure,
                          standard ? 1000 : 30, true, true);
  const bool soakPassed = soak.errors == 0 && soak.rssGrowthPct <= 10.0;
  suite.runs.push_back(std::move(soak));

  for (const auto& run : suite.runs) {
    if (run.rooms == 0 || run.rssAfterSetup <= run.rssBeforeSetup) continue;
    const double estimate = static_cast<double>(run.rssAfterSetup - run.rssBeforeSetup) /
                            static_cast<double>(run.rooms);
    suite.bytesPerRoom = std::max(suite.bytesPerRoom, estimate);
  }
  const uint64_t memoryBytes = PhysicalMemoryBytes();
  if (suite.bytesPerRoom > 0.0 && memoryBytes > 0) {
    suite.memoryLimitRooms = static_cast<size_t>(
        static_cast<double>(memoryBytes) * 0.60 / suite.bytesPerRoom);
  }
  suite.safeRooms = cpuSafeRooms;
  if (suite.memoryLimitRooms > 0) {
    suite.safeRooms = std::min(suite.safeRooms, suite.memoryLimitRooms);
  }
  suite.passed = suite.maxStableRooms >= 1 && (!standard || observedFailure) && soakPassed;
  return suite;
}

template <typename Callback>
void DrainSocket(lab::net::UdpSocket& socket, Callback&& callback) {
  for (;;) {
    lab::net::UdpAddr from{};
    std::vector<uint8_t> bytes;
    if (!socket.RecvFrom(from, bytes)) break;
    callback(from, bytes);
  }
}

struct UdpClient {
  lab::net::UdpSocket socket;
  lab::net::UdpAddr serverAddress{};
  uint8_t playerId = 0;
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
  uint32_t sequence = 0;
  uint32_t latestAccepted = 0;
  bool hasAccepted = false;
  std::unordered_map<uint32_t, Clock::time_point> sentAt;
};

struct UdpRun {
  int offeredPpsPerClient = 0;
  Distribution ackRttMs{};
  Distribution tickMs{};
  double wallSec = 0.0;
  double successPct = 0.0;
  double deadlineMissPct = 0.0;
  double offeredPps = 0.0;
  double acceptedPps = 0.0;
  double rejectedPps = 0.0;
  double ingressMbps = 0.0;
  double egressMbps = 0.0;
  double ackPps = 0.0;
  double statePps = 0.0;
  double processCpuPct = 0.0;
  uint64_t currentRss = 0;
  uint64_t peakRss = 0;
  uint64_t sent = 0;
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  uint64_t ackPackets = 0;
  uint64_t statePackets = 0;
  uint64_t errors = 0;
};

class UdpRig {
public:
  UdpRig() {
    if (!serverSocket_.Open() || !serverSocket_.Bind(0, "127.0.0.1") ||
        !serverSocket_.SetNonBlocking(true)) {
      Fail("UDP benchmark server socket setup failed");
    }
    const uint16_t port = serverSocket_.LocalPort();
    for (auto& client : clients_) {
      if (!client.socket.Open() || !client.socket.Bind(0, "127.0.0.1") ||
          !client.socket.SetNonBlocking(true)) {
        Fail("UDP benchmark client socket setup failed");
      }
      client.serverAddress = Address(port);
    }
    Handshake();
  }

  UdpRun Run(int offeredPpsPerClient, double warmupSec, double measureSec,
             uint64_t minimumSamples) {
    UdpRun result{};
    result.offeredPpsPerClient = offeredPpsPerClient;
    const auto started = Clock::now();
    const auto warmupEnd = started + std::chrono::duration<double>(warmupSec);
    const auto measuredEnd = warmupEnd + std::chrono::duration<double>(measureSec);
    auto nextTick = started;
    std::array<Clock::time_point, 2> nextSend{started, started};
    const auto sendInterval = std::chrono::duration<double>(
        1.0 / static_cast<double>(offeredPpsPerClient));
    const auto tickInterval = std::chrono::duration<double>(1.0 / 60.0);
    std::array<uint32_t, 2> acceptedAtStart{};
    bool measurementStarted = false;
    std::optional<lab::perf::ResourceUsage> usageBefore;
    std::vector<double> rttSamples;
    std::vector<double> tickSamples;
    uint64_t deadlineMisses = 0;
    uint64_t ingressBytes = 0;
    uint64_t egressBytes = 0;

    while (Clock::now() < measuredEnd) {
      auto now = Clock::now();
      for (size_t i = 0; i < clients_.size(); ++i) {
        int catchup = 0;
        while (now >= nextSend[i] && catchup++ < 8) {
          SendInput(clients_[i], measurementStarted, result, ingressBytes);
          nextSend[i] += std::chrono::duration_cast<Clock::duration>(sendInterval);
        }
      }
      DrainServer(measurementStarted, egressBytes);

      int tickCatchup = 0;
      while (now >= nextTick && tickCatchup++ < 4) {
        const auto tickStarted = Clock::now();
        auto output = server_.AdvanceOneTick();
        const double elapsedMs = lab::perf::Milliseconds(Clock::now() - tickStarted);
        if (measurementStarted) {
          tickSamples.push_back(elapsedMs);
          if (elapsedMs > kTickBudgetMs) ++deadlineMisses;
        }
        SendOutput(output, measurementStarted, egressBytes);
        nextTick += std::chrono::duration_cast<Clock::duration>(tickInterval);
      }
      DrainClients(measurementStarted, rttSamples, result);

      if (!measurementStarted && Clock::now() >= warmupEnd) {
        // Flush warm-up ACKs before taking the cumulative accepted counter baseline.
        for (int flush = 0; flush < 4; ++flush) {
          DrainServer(false, egressBytes);
          SendOutput(server_.AdvanceOneTick(), false, egressBytes);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          DrainClients(false, rttSamples, result);
        }
        measurementStarted = true;
        for (size_t i = 0; i < clients_.size(); ++i) {
          acceptedAtStart[i] = clients_[i].latestAccepted;
          clients_[i].sentAt.clear();
          nextSend[i] = Clock::now();
        }
        nextTick = Clock::now() +
            std::chrono::duration_cast<Clock::duration>(tickInterval);
        usageBefore = lab::perf::ReadResourceUsage();
      }

      const auto wake = std::min({nextTick, nextSend[0], nextSend[1]});
      now = Clock::now();
      if (wake > now + std::chrono::microseconds(100)) {
        std::this_thread::sleep_until(wake);
      }
    }

    const auto graceEnd = Clock::now() + std::chrono::milliseconds(50);
    while (Clock::now() < graceEnd) {
      DrainServer(false, egressBytes);
      SendOutput(server_.AdvanceOneTick(), false, egressBytes);
      DrainClients(false, rttSamples, result);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    result.wallSec = measureSec;
    for (size_t i = 0; i < clients_.size(); ++i) {
      result.accepted += static_cast<uint32_t>(clients_[i].latestAccepted - acceptedAtStart[i]);
    }
    result.rejected = result.sent > result.accepted ? result.sent - result.accepted : 0;
    result.successPct = result.sent > 0
        ? 100.0 * static_cast<double>(result.accepted) / static_cast<double>(result.sent) : 0.0;
    result.offeredPps = static_cast<double>(result.sent) / result.wallSec;
    result.acceptedPps = static_cast<double>(result.accepted) / result.wallSec;
    result.rejectedPps = static_cast<double>(result.rejected) / result.wallSec;
    result.ingressMbps = static_cast<double>(ingressBytes) * 8.0 /
                         result.wallSec / 1'000'000.0;
    result.egressMbps = static_cast<double>(egressBytes) * 8.0 /
                        result.wallSec / 1'000'000.0;
    result.ackPps = static_cast<double>(result.ackPackets) / result.wallSec;
    result.statePps = static_cast<double>(result.statePackets) / result.wallSec;
    result.ackRttMs = lab::perf::Summarize(std::move(rttSamples));
    result.tickMs = lab::perf::Summarize(std::move(tickSamples));
    result.deadlineMissPct = result.tickMs.samples > 0
        ? 100.0 * static_cast<double>(deadlineMisses) /
              static_cast<double>(result.tickMs.samples)
        : 100.0;
    if (result.ackRttMs.samples < minimumSamples) ++result.errors;
    const auto usageAfter = lab::perf::ReadResourceUsage();
    if (usageBefore) {
      const double cpuSec = (usageAfter.userSec - usageBefore->userSec) +
                            (usageAfter.systemSec - usageBefore->systemSec);
      result.processCpuPct = result.wallSec > 0.0
          ? cpuSec * 100.0 / result.wallSec : 0.0;
    }
    result.currentRss = usageAfter.currentRssBytes;
    result.peakRss = usageAfter.peakRssBytes;
    return result;
  }

private:
  AuthoritativeServer server_{20240625u, false};
  lab::net::UdpSocket serverSocket_;
  std::array<UdpClient, 2> clients_{};

  void Handshake() {
    for (auto& client : clients_) {
      lab::net::InputPacket hello{};
      const auto bytes = lab::net::EncodeInput(hello);
      if (!client.socket.SendTo(client.serverAddress, bytes)) {
        Fail("UDP benchmark hello send failed");
      }
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
      DrainServer(false, ignoredBytes_);
      DrainClients(false, ignoredRtt_, ignoredRun_);
      if (server_.started() && std::all_of(
              clients_.begin(), clients_.end(),
              [](const UdpClient& client) { return client.sessionId != 0; })) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Fail("UDP benchmark handshake timed out");
  }

  void SendInput(UdpClient& client, bool measuring,
                 UdpRun& result, uint64_t& ingressBytes) {
    lab::net::InputPacket packet{};
    packet.playerId = client.playerId;
    packet.sessionId = client.sessionId;
    packet.matchId = client.matchId;
    packet.seq = client.sequence++;
    packet.newestTick = server_.tick();
    packet.cmds.push_back(InputBuffer::DefaultForTick(packet.newestTick));
    const auto bytes = lab::net::EncodeInput(packet);
    if (!client.socket.SendTo(client.serverAddress, bytes)) {
      if (measuring) ++result.errors;
      return;
    }
    if (measuring) {
      ++result.sent;
      ingressBytes += bytes.size();
      client.sentAt[packet.seq] = Clock::now();
    }
  }

  void DrainServer(bool measuring, uint64_t& egressBytes) {
    DrainSocket(serverSocket_, [&](const lab::net::UdpAddr& from,
                                   const std::vector<uint8_t>& bytes) {
      const double nowSec = std::chrono::duration<double>(
          Clock::now().time_since_epoch()).count();
      SendOutput(server_.HandleDatagram(from, bytes.data(), bytes.size(), nowSec),
                 measuring, egressBytes);
    });
  }

  void SendOutput(const std::vector<OutboundDatagram>& output,
                  bool measuring, uint64_t& egressBytes) {
    for (const auto& datagram : output) {
      if (!serverSocket_.SendTo(datagram.to, datagram.bytes)) continue;
      if (measuring) egressBytes += datagram.bytes.size();
    }
  }

  void DrainClients(bool measuring, std::vector<double>& rttSamples, UdpRun& result) {
    for (auto& client : clients_) {
      DrainSocket(client.socket, [&](const lab::net::UdpAddr&,
                                     const std::vector<uint8_t>& bytes) {
        if (auto start = lab::net::DecodeStart(bytes.data(), bytes.size())) {
          client.playerId = start->playerId;
          client.sessionId = start->sessionId;
          client.matchId = start->matchId;
          return;
        }
        if (auto ack = lab::net::DecodeAck(bytes.data(), bytes.size())) {
          if (client.sessionId != ack->sessionId || client.matchId != ack->matchId ||
              client.playerId != ack->playerId) {
            if (measuring) ++result.errors;
            return;
          }
          client.latestAccepted = ack->serverInputPacketsReceived;
          client.hasAccepted = true;
          if (measuring) {
            ++result.ackPackets;
            auto sent = client.sentAt.find(ack->serverRecvInputSeq);
            if (sent != client.sentAt.end()) {
              rttSamples.push_back(lab::perf::Milliseconds(Clock::now() - sent->second));
              client.sentAt.erase(sent);
            }
          }
          return;
        }
        if (auto state = lab::net::DecodeState(bytes.data(), bytes.size())) {
          if (measuring) {
            ++result.statePackets;
            if (client.sessionId != state->sessionId || client.matchId != state->matchId ||
                client.playerId != state->playerId) ++result.errors;
          }
          return;
        }
        if (measuring) ++result.errors;
      });
    }
  }

  uint64_t ignoredBytes_ = 0;
  std::vector<double> ignoredRtt_;
  UdpRun ignoredRun_{};
};

struct UdpSuite {
  std::vector<UdpRun> runs;
  int maxNoDropPpsPerClient = 0;
  bool passed = false;
};

UdpSuite RunUdpSuite(bool standard) {
  UdpSuite suite;
  const std::array<int, 4> rates{60, 120, 240, 480};
  const int repeats = standard ? 3 : 1;
  const double warmupSec = standard ? 5.0 : 0.05;
  const double measureSec = standard ? 20.0 : 0.50;
  const uint64_t minimumSamples = standard ? 1000 : 20;

  bool baselinePassed = true;
  bool overloadLimited = true;
  for (int rate : rates) {
    bool rateNoDrop = true;
    for (int repeat = 0; repeat < repeats; ++repeat) {
      UdpRig rig;
      UdpRun run = rig.Run(rate, warmupSec, measureSec, minimumSamples);
      if (rate == 60) {
        baselinePassed = baselinePassed && run.errors == 0 &&
                         run.successPct >= 99.9 && run.ackRttMs.p99 <= 50.0 &&
                         run.deadlineMissPct <= kMaxDeadlineMissPct;
      }
      if (rate == 480) {
        overloadLimited = overloadLimited && run.acceptedPps <= 520.0;
      }
      rateNoDrop = rateNoDrop && run.successPct >= 99.9 && run.errors == 0;
      suite.runs.push_back(std::move(run));
    }
    if (rateNoDrop) suite.maxNoDropPpsPerClient = rate;
  }
  suite.passed = baselinePassed && (!standard || overloadLimited);
  return suite;
}

void WriteDistribution(std::ostream& output, const Distribution& value) {
  output << "{\"samples\":" << value.samples
         << ",\"mean\":" << value.mean
         << ",\"stddev\":" << value.stddev
         << ",\"p50\":" << value.p50
         << ",\"p95\":" << value.p95
         << ",\"p99\":" << value.p99
         << ",\"p999\":" << value.p999
         << ",\"max\":" << value.maximum
         << ",\"jitterP99P50\":" << value.jitterP99P50 << "}";
}

void WriteCapacityRun(std::ostream& output, const CapacityRun& run) {
  output << "{\"rooms\":" << run.rooms
         << ",\"players\":" << run.rooms * 2
         << ",\"finalValidation\":" << (run.finalValidation ? "true" : "false")
         << ",\"soak\":" << (run.soak ? "true" : "false")
         << ",\"frameMs\":";
  WriteDistribution(output, run.frameMs);
  output << ",\"wallSec\":" << run.wallSec
         << ",\"deadlineMissPct\":" << run.deadlineMissPct
         << ",\"processCpuPct\":" << run.processCpuPct
         << ",\"projectedRealtimeCpuPct\":" << run.projectedRealtimeCpuPct
         << ",\"benchmarkFramesPerSec\":" << run.benchmarkFramesPerSec
         << ",\"realtimeInputPps\":" << run.realtimeInputPps
         << ",\"realtimeOutputPps\":" << run.realtimeOutputPps
         << ",\"realtimeOutputMbps\":" << run.realtimeOutputMbps
         << ",\"errors\":" << run.errors
         << ",\"rssBeforeSetup\":" << run.rssBeforeSetup
         << ",\"rssAfterSetup\":" << run.rssAfterSetup
         << ",\"rssAfterWarmup\":" << run.rssAfterWarmup
         << ",\"rssEnd\":" << run.rssEnd
         << ",\"peakRss\":" << run.peakRss
         << ",\"rssGrowthPct\":" << run.rssGrowthPct
         << ",\"rssGrowthMbPerMin\":" << run.rssGrowthMbPerMin << "}";
}

void WriteUdpRun(std::ostream& output, const UdpRun& run) {
  output << "{\"offeredPpsPerClient\":" << run.offeredPpsPerClient
         << ",\"ackRttMs\":";
  WriteDistribution(output, run.ackRttMs);
  output << ",\"tickMs\":";
  WriteDistribution(output, run.tickMs);
  output << ",\"wallSec\":" << run.wallSec
         << ",\"successPct\":" << run.successPct
         << ",\"deadlineMissPct\":" << run.deadlineMissPct
         << ",\"offeredPps\":" << run.offeredPps
         << ",\"acceptedPps\":" << run.acceptedPps
         << ",\"rejectedPps\":" << run.rejectedPps
         << ",\"ingressMbps\":" << run.ingressMbps
         << ",\"egressMbps\":" << run.egressMbps
         << ",\"ackPps\":" << run.ackPps
         << ",\"statePps\":" << run.statePps
         << ",\"processCpuPct\":" << run.processCpuPct
         << ",\"currentRss\":" << run.currentRss
         << ",\"peakRss\":" << run.peakRss
         << ",\"sent\":" << run.sent
         << ",\"accepted\":" << run.accepted
         << ",\"rejected\":" << run.rejected
         << ",\"ackPackets\":" << run.ackPackets
         << ",\"statePackets\":" << run.statePackets
         << ",\"errors\":" << run.errors << "}";
}

std::string BuildJson(const Options& options,
                      const std::optional<CapacitySuite>& capacity,
                      const std::optional<UdpSuite>& udp) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(6);
  output << "{\"schemaVersion\":1"
         << ",\"profile\":\""
         << (options.profile == Options::Profile::Standard ? "standard" : "smoke")
         << "\",\"criteria\":{\"tickBudgetMs\":" << kTickBudgetMs
         << ",\"maxDeadlineMissPct\":" << kMaxDeadlineMissPct
         << ",\"safeCapacityRatio\":0.8,\"baselineUdpSuccessPct\":99.9"
         << ",\"baselineUdpP99RttMs\":50.0}"
         << ",\"parameters\":{\"playersPerRoom\":2,\"tickRateHz\":60"
         << ",\"udpOfferedPpsPerClient\":[60,120,240,480]"
         << ",\"warmupSec\":"
         << (options.profile == Options::Profile::Standard ? 5.0 : 0.05)
         << ",\"sampleSec\":"
         << (options.profile == Options::Profile::Standard ? 20.0 : 0.5)
         << ",\"repeats\":"
         << (options.profile == Options::Profile::Standard ? 3 : 1)
         << ",\"soakSec\":"
         << (options.profile == Options::Profile::Standard ? 60.0 : 0.2)
         << ",\"minimumSamples\":"
         << (options.profile == Options::Profile::Standard ? 1000 : 20)
         << "}"
         << ",\"environment\":{\"hardwareThreads\":"
         << std::thread::hardware_concurrency()
         << ",\"physicalMemoryBytes\":" << PhysicalMemoryBytes()
#ifdef NDEBUG
         << ",\"buildType\":\"Release\"}"
#else
         << ",\"buildType\":\"DebugOrUnspecified\"}"
#endif
         << ",\"capacity\":";
  if (!capacity) {
    output << "null";
  } else {
    output << "{\"passed\":" << (capacity->passed ? "true" : "false")
           << ",\"maxStableRooms\":" << capacity->maxStableRooms
           << ",\"maxStablePlayers\":" << capacity->maxStableRooms * 2
           << ",\"firstFailRooms\":" << capacity->firstFailRooms
           << ",\"safeRooms\":" << capacity->safeRooms
           << ",\"safePlayers\":" << capacity->safeRooms * 2
           << ",\"memoryLimitRooms\":" << capacity->memoryLimitRooms
           << ",\"bytesPerRoom\":" << capacity->bytesPerRoom
           << ",\"limitingResource\":\""
           << ((capacity->memoryLimitRooms > 0 &&
                capacity->memoryLimitRooms < capacity->maxStableRooms * 8 / 10)
                   ? "memory" : "cpuRealtimeBudget")
           << "\""
           << ",\"runs\":[";
    for (size_t i = 0; i < capacity->runs.size(); ++i) {
      if (i) output << ',';
      WriteCapacityRun(output, capacity->runs[i]);
    }
    output << "]}";
  }
  output << ",\"udp\":";
  if (!udp) {
    output << "null";
  } else {
    output << "{\"passed\":" << (udp->passed ? "true" : "false")
           << ",\"maxNoDropPpsPerClient\":" << udp->maxNoDropPpsPerClient
           << ",\"runs\":[";
    for (size_t i = 0; i < udp->runs.size(); ++i) {
      if (i) output << ',';
      WriteUdpRun(output, udp->runs[i]);
    }
    output << "]}";
  }
  output << "}";
  return output.str();
}

} // namespace

int main(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  const bool standard = options.profile == Options::Profile::Standard;
  std::optional<CapacitySuite> capacity;
  std::optional<UdpSuite> udp;

  if (options.mode != Options::Mode::Udp) {
    std::cout << "performance: running capacity suite\n";
    capacity = RunCapacitySuite(standard);
  }
  if (options.mode != Options::Mode::Capacity) {
    std::cout << "performance: running UDP suite\n";
    udp = RunUdpSuite(standard);
  }

  const std::string json = BuildJson(options, capacity, udp);
  if (!options.jsonPath.empty()) {
    std::ofstream output(options.jsonPath);
    if (!output) Fail("cannot open JSON output: " + options.jsonPath);
    output << json << '\n';
  }
  std::cout << json << '\n';

  const bool passed = (!capacity || capacity->passed) && (!udp || udp->passed);
  if (!passed) Fail("one or more performance acceptance gates failed");
  std::cout << "performance OK\n";
  return 0;
}
