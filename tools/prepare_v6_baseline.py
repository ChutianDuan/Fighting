#!/usr/bin/env python3
"""在独立源码副本撤回性能候选；保留最终 v6 正确性修复，用于同版本对照。"""
from pathlib import Path
import re
import shutil


def replace_required(source, old, new):
    """源码结构变化时明确失败，避免悄悄生成未撤回优化的基线。"""
    if old not in source:
        raise RuntimeError(f'baseline source fragment missing: {old!r}')
    return source.replace(old, new)

root = Path(__file__).resolve().parents[1]
destination = root / 'build/v6-performance/baseline-source'
if destination.exists():
    shutil.rmtree(destination)
destination.mkdir(parents=True)
for directory in ('include', 'src', 'apps', 'tests'):
    shutil.copytree(root / directory, destination / directory)
for filename in ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'vcpkg-configuration.json'):
    shutil.copy2(root / filename, destination / filename)
# 基线保留最终正确性修复，只撤回可测量的复制/容器优化。
state_h = destination / 'include/lab/sim/StateHistory.h'
state_h.write_text(r"""#pragma once
#include <lab/sim/World.h>
#include <optional>
namespace lab::sim {
class StateHistory {
public:
    explicit StateHistory(size_t n):cap_(n?n:1),ring_(cap_) {}
    void Put(const WorldSnapshot& s) { auto& slot=ring_[s.tick%cap_]; slot.valid=true; slot.tick=s.tick; slot.snap=s; }
    std::optional<WorldSnapshot> Get(Tick t) const { const auto& slot=ring_[t%cap_]; if(!slot.valid || slot.tick!=t) return {}; return slot.snap; }
    size_t Capacity() const { return cap_; }
private:
    struct Slot { bool valid=false; Tick tick=0; WorldSnapshot snap; };
    size_t cap_; std::vector<Slot> ring_;
};
}
""")
world_cpp = destination / 'src/sim/World.cpp'
s = world_cpp.read_text()
a = s.index('WorldSnapshot World::Snapshot() const')
b = s.index('void World::Restore', a)
s = s[:a] + """WorldSnapshot World::Snapshot() const {
    WorldSnapshot out=snap_;
    SnapshotMaze(out);
    SyncProjectiles(out);
    return out;
}

""" + s[b:]
a = s.rfind('    SyncProjectiles(snap_);') + len('    SyncProjectiles(snap_);')
s = s[:a] + '\n    SnapshotMaze(snap_);' + s[a:]
world_cpp.write_text(s)

client_h = destination / 'include/lab/session/ClientSession.h'
s, count = re.subn(
    r'const std::vector<InputCmd> &BuildPredictedCommands\(InputCmd localInput,\s+const WorldSnapshot &snapshot\);',
    'std::vector<InputCmd> BuildPredictedCommands(InputCmd localInput, const WorldSnapshot &snapshot) const;',
    client_h.read_text())
if count != 1:
    raise RuntimeError('baseline command declaration missing')
s = replace_required(s, '    std::vector<InputCmd> commands_{2};\n', '')
client_h.write_text(s)
client_cpp = destination / 'src/session/ClientSession.cpp'
s = client_cpp.read_text()
s, count = re.subn(
    r'const std::vector<InputCmd> &ClientSession::BuildPredictedCommands\(InputCmd localInput,\s+const WorldSnapshot &snapshot\) \{\n    auto &commands = commands_;\n    std::fill\(commands.begin\(\), commands.end\(\), InputCmd\{\}\);',
    'std::vector<InputCmd> ClientSession::BuildPredictedCommands(InputCmd localInput, const WorldSnapshot &snapshot) const {\n    std::vector<InputCmd> commands(2);', s)
if count != 1:
    raise RuntimeError('baseline command implementation missing')
s = replace_required(s, 'BuildPredictedCommands(*localInput, world_.View())',
                     'BuildPredictedCommands(*localInput, world_.Snapshot())')
s = replace_required(s, 'BuildPredictedCommands(input, world_.View())',
                     'BuildPredictedCommands(input, world_.Snapshot())')
s = replace_required(s, 'history_.Put(world_.View())', 'history_.Put(world_.Snapshot())')
s = replace_required(s, 'auto predictedState = history_.GetView(snapshot.tick);',
                     'auto predictedState = history_.Get(snapshot.tick);')
s = replace_required(s, 'predictedState->dynamic.players', 'predictedState->players')
a = s.index('    float previousDisplayX = 0, previousDisplayY = 0;')
b = s.index('    world_.Restore(snapshot);', a)
s = s[:a] + '    const auto previousDisplay = Display(now);\n' + s[b:]
s = replace_required(s, 'const auto &current = world_.View();', 'auto current = world_.Snapshot();')
s = replace_required(s, 'previousDisplayX - current.players[localIndex].x',
                     'previousDisplay.players[localIndex].x - current.players[localIndex].x')
s = replace_required(s, 'previousDisplayY - current.players[localIndex].y',
                     'previousDisplay.players[localIndex].y - current.players[localIndex].y')
s = replace_required(s, '        message.inputs.reserve(kInputRedundancy);\n', '')
client_cpp.write_text(s)
room_h = destination / 'include/lab/session/RoomServer.h'
room_h.write_text(replace_required(room_h.read_text(), '        std::vector<InputCmd> commands{2};\n', ''))
room_cpp = destination / 'src/session/RoomServer.cpp'
s = replace_required(room_cpp.read_text(), 'room.world.View().tick', 'room.world.Snapshot().tick')
s = replace_required(s, 'auto &commands = room.commands;', 'std::vector<InputCmd> commands(2);')
room_cpp.write_text(s)
protocol_cpp = destination / 'src/session/Protocol.cpp'
s = protocol_cpp.read_text()
for line in ('        snapshot.maze.reserve(mazeCells.size());\n',
             '    snapshot.players.reserve(players.size());\n',
             '    snapshot.projectiles.reserve(projectiles.size());\n',
             '    json["players"].get_ref<Json::array_t &>().reserve(snapshot.players.size());\n',
             '    json["projectiles"].get_ref<Json::array_t &>().reserve(snapshot.projectiles.size());\n',
             '    json["inputs"].get_ref<Json::array_t &>().reserve(message.inputs.size());\n',
             '        message.inputs.reserve(json.at("inputs").size());\n'):
    s = replace_required(s, line, '')
a = s.index('    Bytes bytes(body.size() + kHeaderSize);')
b = s.index('    return bytes;', a)
s = s[:a] + '''    Bytes bytes{0x4c, 0x41, 0x42, 0x30, 0, kProtocolVersion, 0, static_cast<uint8_t>(message.kind)};
    uint32_t bodyLength = body.size();
    for (int shift = (kBodyLengthBytes - 1) * 8; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<uint8_t>(bodyLength >> shift));
    bytes.insert(bytes.end(), body.begin(), body.end());
''' + s[b:]
protocol_cpp.write_text(s)
print(destination)
