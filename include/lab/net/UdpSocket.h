#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <event2/event.h>
#include <event2/util.h>

// libevent 事件类型；此头文件同时使用其回调参数类型。
struct event_base;
struct event;

namespace lab::net {

struct UdpAddr {
  sockaddr_in addr{};

  static UdpAddr FromIPv4(const std::string& ip, uint16_t port);
  std::string ToString() const;
  uint64_t Key() const;
};

// 拥有 fd 和读事件，借用 event_base；应在 base 释放前停止读事件。
class UdpSocket {
public:
  // data 指向接收栈缓冲，只在回调期间有效，调用方需当场解码或复制。
  using OnDatagramFn = void(*)(void* user, const UdpAddr& from, const uint8_t* data, size_t len);

  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool Open();
  bool Bind(uint16_t port, const std::string& bindIp = "0.0.0.0");

  bool SetNonBlocking(bool on);
  bool SetRecvBuf(int bytes);
  bool SetSendBuf(int bytes);

  bool SendTo(const UdpAddr& to, const uint8_t* data, size_t len);
  bool SendTo(const UdpAddr& to, const std::vector<uint8_t>& buf);

  // 手动轮询用于无界面测试；false 同时涵盖无数据和接收错误。
  bool RecvFrom(UdpAddr& from, std::vector<uint8_t>& out);

  int fd() const { return fd_; }
  uint16_t LocalPort() const;

  // ---------------- libevent 集成 ----------------
  // 注册到借用的 event_base；收到数据报后串行调用 fn，不额外创建线程。
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
