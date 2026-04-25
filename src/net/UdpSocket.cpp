#include <lab/net/UdpSocket.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>

// libevent
#include <event2/event.h>
#include <event2/util.h>

namespace lab::net {

UdpAddr UdpAddr::FromIPv4(const std::string& ip, uint16_t port) {
  UdpAddr a;
  a.addr.sin_family = AF_INET;
  a.addr.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &a.addr.sin_addr);
  return a;
}

std::string UdpAddr::ToString() const {
  char buf[64];
  const char* p = inet_ntop(AF_INET, (void*)&addr.sin_addr, buf, sizeof(buf));
  std::ostringstream oss;
  oss << (p ? p : "0.0.0.0") << ":" << ntohs(addr.sin_port);
  return oss.str();
}

uint64_t UdpAddr::Key() const {
  uint32_t ip = ntohl(addr.sin_addr.s_addr);
  uint16_t port = ntohs(addr.sin_port);
  return (uint64_t(ip) << 16) | uint64_t(port);
}

UdpSocket::~UdpSocket() {
  StopEventRead();
  if (fd_ >= 0) ::close(fd_);
}

bool UdpSocket::Open() {
  if (fd_ >= 0) return true;

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) return false;

  int yes = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  return true;
}

bool UdpSocket::Bind(uint16_t port, const std::string& bindIp) {
  if (fd_ < 0 && !Open()) return false;

  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, bindIp.c_str(), &a.sin_addr);

  return ::bind(fd_, (sockaddr*)&a, sizeof(a)) == 0;
}

bool UdpSocket::SetNonBlocking(bool on) {
  if (fd_ < 0) return false;

  // 推荐用 libevent util（跨平台更稳）；但这里保留你原有语义
  if (on) {
    return evutil_make_socket_nonblocking(fd_) == 0;
  } else {
    // libevent 没有 direct make_blocking，这里用 fcntl 恢复
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return false;
    flags &= ~O_NONBLOCK;
    return fcntl(fd_, F_SETFL, flags) == 0;
  }
}

bool UdpSocket::SetRecvBuf(int bytes) {
  if (fd_ < 0) return false;
  return ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}
bool UdpSocket::SetSendBuf(int bytes) {
  if (fd_ < 0) return false;
  return ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0;
}

bool UdpSocket::SendTo(const UdpAddr& to, const uint8_t* data, size_t len) {
  if (fd_ < 0) return false;
  ssize_t n = ::sendto(fd_, data, len, 0, (sockaddr*)&to.addr, sizeof(to.addr));
  return n == (ssize_t)len;
}
bool UdpSocket::SendTo(const UdpAddr& to, const std::vector<uint8_t>& buf) {
  return SendTo(to, buf.data(), buf.size());
}

bool UdpSocket::RecvFrom(UdpAddr& from, std::vector<uint8_t>& out) {
  if (fd_ < 0) return false;

  uint8_t buf[2048];
  socklen_t sl = sizeof(from.addr);
  ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0, (sockaddr*)&from.addr, &sl);

  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) return false;
    return false;
  }

  out.assign(buf, buf + n);
  return true;
}

// ---------------- libevent ----------------

bool UdpSocket::StartEventRead(event_base* base, OnDatagramFn fn, void* user) {
  if (!base) return false;
  if (fd_ < 0 && !Open()) return false;

  // UDP + libevent 基本要求：非阻塞
  if (!SetNonBlocking(true)) return false;

  // 若重复 Start，先 Stop，保证状态干净
  StopEventRead();

  base_ = base;
  on_datagram_ = fn;
  on_user_ = user;

  // 监听可读事件，持久化（一直触发）
  ev_read_ = event_new(base_, fd_, EV_READ | EV_PERSIST, &UdpSocket::ReadCb, this);
  if (!ev_read_) {
    base_ = nullptr;
    on_datagram_ = nullptr;
    on_user_ = nullptr;
    return false;
  }

  if (event_add(ev_read_, nullptr) != 0) {
    event_free(ev_read_);
    ev_read_ = nullptr;
    base_ = nullptr;
    on_datagram_ = nullptr;
    on_user_ = nullptr;
    return false;
  }

  return true;
}

void UdpSocket::StopEventRead() {
  if (ev_read_) {
    event_del(ev_read_);
    event_free(ev_read_);
    ev_read_ = nullptr;
  }
  base_ = nullptr;
  on_datagram_ = nullptr;
  on_user_ = nullptr;
}

void UdpSocket::ReadCb(evutil_socket_t /*fd*/, short /*what*/, void* arg) {
  auto* self = static_cast<UdpSocket*>(arg);
  self->HandleReadable();
}

void UdpSocket::HandleReadable() {
  if (!on_datagram_) {
    // 没有回调就把数据读空丢弃，避免 event 反复触发（通常不应发生）
  }

  // 必须循环 recv 到 EAGAIN/EWOULDBLOCK，否则 backlog 堆积会导致事件频繁触发
  for (;;) {
    UdpAddr from{};
    uint8_t buf[2048];
    socklen_t sl = sizeof(from.addr);
    ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0, (sockaddr*)&from.addr, &sl);

    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) break;  // 读空
      // 其他错误：这里可加日志
      break;
    }

    if (n == 0) {
      // UDP 一般不会出现 0-length 的“连接关闭”语义；可忽略
      continue;
    }

    if (on_datagram_) {
      on_datagram_(on_user_, from, buf, (size_t)n);
    }
  }
}

} // namespace lab::net
