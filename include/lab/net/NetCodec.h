# pragma once
# include <cstdint>
# include <vector>
# include <optional>

# include <lab/net/Packets.h>

namespace lab::net{

// 逐字段使用网络字节序编码，不把 C++ 结构体内存布局直接当作协议。
std::vector<uint8_t> EncodeInput(const InputPacket& p);
std::vector<uint8_t> EncodeAck(const AckPacket& p);
std::vector<uint8_t> EncodeState(const StatePacket& p);
std::vector<uint8_t> EncodeStart(const StartPacket& p);
std::vector<uint8_t> EncodeReset(const ResetPacket& p);


// 解码检查协议头、字段长度与尾部多余数据；身份和值域由调用方继续校验。
std::optional<InputPacket> DecodeInput(const uint8_t* data, size_t len);
std::optional<AckPacket>   DecodeAck(const uint8_t* data, size_t len);
std::optional<StatePacket> DecodeState(const uint8_t* data, size_t len);
std::optional<StartPacket> DecodeStart(const uint8_t* data, size_t len);
std::optional<ResetPacket> DecodeReset(const uint8_t* data, size_t len);

}
