#include <lab/sim/InputBuffer.h>

InputBuffer::InputBuffer(size_t capacityTicks)
  : cap_(capacityTicks == 0 ? 1 : capacityTicks), ring_(cap_) {}

void InputBuffer::Put(const InputCmd& cmd) {
  const size_t idx = size_t(cmd.tick % cap_);
  ring_[idx].valid = true;
  ring_[idx].cmd = cmd;
}

std::optional<InputCmd> InputBuffer::Get(Tick tick) const {
  const size_t idx = size_t(tick % cap_);
  if (!ring_[idx].valid) return std::nullopt;
  if (ring_[idx].cmd.tick != tick) return std::nullopt; // 环覆盖后要判 tick
  return ring_[idx].cmd;
}

InputCmd InputBuffer::DefaultForTick(Tick tick) {
  InputCmd c{};
  c.tick = tick;
  c.buttons = 0;
  c.moveX = 0;
  c.moveY = 0;
  return c;
}
