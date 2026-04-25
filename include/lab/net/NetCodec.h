# pragma once
# include <cstdint>
# include <vector>
# include <optional>

# include <lab/net/Packets.h>

namespace lab::net{

std::vector<uint8_t> EncodeInput(const InputPacket& p);
std::vector<uint8_t> EncodeAck(const AckPacket& p);
std::vector<uint8_t> EncodeState(const StatePacket& p);
std::vector<uint8_t> EncodeStart(const StartPacket& p);


std::optional<InputPacket> DecodeInput(const uint8_t* data, size_t len);
std::optional<AckPacket>   DecodeAck(const uint8_t* data, size_t len);
std::optional<StatePacket> DecodeState(const uint8_t* data, size_t len); // NEW
std::optional<StartPacket> DecodeStart(const uint8_t* data, size_t len);

}
