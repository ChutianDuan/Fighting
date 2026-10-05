#pragma once
#include <cstddef>
#include <lab/sim/World.h>
#include <memory>
#include <optional>
#include <vector>

namespace lab::sim {
struct HistoryMap {
    uint32_t seed = 0, width = 0, height = 0;
    std::vector<uint8_t> cells;
};
// 非拥有只读视图：只在对应环槽再次写入前有效，不得跨 Put 保存。
struct HistoryView {
    const WorldSnapshot &dynamic;
    const HistoryMap &map;
};
class StateHistory {
  public:
    explicit StateHistory(size_t capacityTicks)
        : cap_(capacityTicks ? capacityTicks : 1), ring_(cap_) {}
    void Put(const WorldSnapshot &s) {
        if (!map_ || map_->seed != s.mazeSeed || map_->width != s.mazeWidth ||
            map_->height != s.mazeHeight || map_->cells != s.maze) {
            map_ = std::make_shared<const HistoryMap>(
                HistoryMap{s.mazeSeed, s.mazeWidth, s.mazeHeight, s.maze});
        }
        auto &slot = ring_[s.tick % cap_];
        slot.valid = true;
        slot.tick = s.tick;
        slot.map = map_;
        // 只复制动态容器；不能先复制完整快照再 clear，否则仍保留每槽地图容量。
        auto &d = slot.dynamic;
        d.tick = s.tick;
        d.players = s.players;
        d.projectiles = s.projectiles;
        d.mazeSeed = s.mazeSeed;
        d.mazeWidth = s.mazeWidth;
        d.mazeHeight = s.mazeHeight;
    }
    std::optional<HistoryView> GetView(Tick tick) const {
        const auto &slot = ring_[tick % cap_];
        if (!slot.valid || slot.tick != tick)
            return {};
        return HistoryView{slot.dynamic, *slot.map};
    }
    std::optional<WorldSnapshot> Get(Tick tick) const {
        auto view = GetView(tick);
        if (!view)
            return {};
        WorldSnapshot s = view->dynamic;
        s.maze = view->map.cells;
        return s;
    }
    size_t Capacity() const { return cap_; }

  private:
    struct Slot {
        bool valid = false;
        Tick tick = 0;
        WorldSnapshot dynamic;
        std::shared_ptr<const HistoryMap> map;
    };
    size_t cap_;
    std::vector<Slot> ring_;
    std::shared_ptr<const HistoryMap> map_;
};
} // namespace lab::sim
