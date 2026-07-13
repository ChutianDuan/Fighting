#include <algorithm>
#include <cstdint>
#include <vector>

#include <event2/event.h>
#include <event2/util.h>

#include <lab/net/UdpSocket.h>
#include <lab/server/AuthoritativeServer.h>
#include <lab/time/Clock.h>
#include <lab/util/log.h>

namespace {

struct ServerApp {
  lab::net::UdpSocket socket;
  lab::server::AuthoritativeServer server;
  double previousSec = 0.0;
  double accumulatorSec = 0.0;
};

void SendAll(ServerApp& app,
             const std::vector<lab::server::OutboundDatagram>& datagrams) {
  for (const auto& datagram : datagrams) {
    if (!app.socket.SendTo(datagram.to, datagram.bytes)) {
      LOGW("Server send failed: %s", datagram.to.ToString().c_str());
    }
  }
}

void OnUdp(void* user,
           const lab::net::UdpAddr& from,
           const uint8_t* data,
           size_t length) {
  auto& app = *static_cast<ServerApp*>(user);
  SendAll(app, app.server.HandleDatagram(from, data, length, Clock::NowSeconds()));
}

void OnTick(evutil_socket_t, short, void* user) {
  auto& app = *static_cast<ServerApp*>(user);
  const double nowSec = Clock::NowSeconds();
  const double elapsedSec = std::min(nowSec - app.previousSec, 0.25);
  app.previousSec = nowSec;
  app.accumulatorSec += std::max(0.0, elapsedSec);

  SendAll(app, app.server.ExpireClients(nowSec));
  constexpr double tickSec = 1.0 / 60.0;
  while (app.accumulatorSec >= tickSec) {
    SendAll(app, app.server.AdvanceOneTick());
    app.accumulatorSec -= tickSec;
  }
}

} // namespace

int main() {
  constexpr uint16_t port = 40000;
  event_base* base = event_base_new();
  if (!base) {
    LOGE("Server: event_base_new failed");
    return 1;
  }

  ServerApp app;
  app.previousSec = Clock::NowSeconds();
  if (!app.socket.Open() || !app.socket.Bind(port) ||
      !app.socket.SetNonBlocking(true)) {
    LOGE("Server: failed to open/bind UDP port=%u", port);
    event_base_free(base);
    return 1;
  }
  app.socket.SetRecvBuf(1 << 20);
  app.socket.SetSendBuf(1 << 20);
  if (!app.socket.StartEventRead(base, &OnUdp, &app)) {
    LOGE("Server: StartEventRead failed");
    event_base_free(base);
    return 1;
  }

  event* tickEvent = event_new(base, -1, EV_PERSIST, &OnTick, &app);
  timeval interval{0, 1000};
  if (!tickEvent || event_add(tickEvent, &interval) != 0) {
    LOGE("Server: failed to register tick event");
    if (tickEvent) event_free(tickEvent);
    event_base_free(base);
    return 1;
  }

  LOGI("Server listening on 0.0.0.0:%u", port);
  event_base_dispatch(base);
  event_free(tickEvent);
  event_base_free(base);
  return 0;
}
