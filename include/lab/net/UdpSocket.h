#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <event2/event.h>
#include <event2/util.h>

// 前置声明（避免头文件引入 libevent 过多）
struct event_base;
struct event;

namespace lab::net {

struct UdpAddr {
  sockaddr_in addr{};

  static UdpAddr FromIPv4(const std::string& ip, uint16_t port);
  std::string ToString() const;
  uint64_t Key() const;
};

class UdpSocket {
public:
  using OnDatagramFn = void(*)(void* user, const UdpAddr& from, const uint8_t* data, size_t len);

  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool Open();
  bool Bind(uint16_t port, const std::string& bindIp = "0.0.0.0");

  bool SetNonBlocking(bool on);   // 仍保留（内部实现可以换成 evutil）
  bool SetRecvBuf(int bytes);
  bool SetSendBuf(int bytes);

  bool SendTo(const UdpAddr& to, const uint8_t* data, size_t len);
  bool SendTo(const UdpAddr& to, const std::vector<uint8_t>& buf);

  // 保留原“手动轮询收包”接口（可选）
  bool RecvFrom(UdpAddr& from, std::vector<uint8_t>& out);

  int fd() const { return fd_; }

  // ---------------- libevent 集成 ----------------
  // 将该 UDP socket 注册到 event_base；收到任何 datagram 时触发回调
  bool StartEventRead(event_base* base, OnDatagramFn fn, void* user);

  // 解除事件注册（可重复调用）
  void StopEventRead();

private:
  static void ReadCb(evutil_socket_t fd, short what, void* arg);
  void HandleReadable();

private:
  int fd_ = -1;

  event_base* base_ = nullptr;
  event* ev_read_ = nullptr;

  OnDatagramFn on_datagram_ = nullptr;
  void* on_user_ = nullptr;
};

} // namespace lab::net
