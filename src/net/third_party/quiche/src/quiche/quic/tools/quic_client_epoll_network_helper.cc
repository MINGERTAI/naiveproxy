// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/tools/quic_client_epoll_network_helper.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "absl/cleanup/cleanup.h"
#include "quiche/quic/core/crypto/quic_random.h"
#include "quiche/quic/core/http/spdy_utils.h"
#include "quiche/quic/core/quic_connection.h"
#include "quiche/quic/core/quic_data_reader.h"
#include "quiche/quic/core/quic_epoll_alarm_factory.h"
#include "quiche/quic/core/quic_epoll_connection_helper.h"
#include "quiche/quic/core/quic_packets.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/quic_udp_socket.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/common/platform/api/quiche_system_event_loop.h"

namespace quic {

namespace {
const int kEpollFlags = EPOLLIN | EPOLLOUT | EPOLLET;
}  // namespace

QuicClientEpollNetworkHelper::QuicClientEpollNetworkHelper(
    QuicEpollServer* epoll_server, QuicClientBase* client)
    : epoll_server_(epoll_server),
      packets_dropped_(0),
      overflow_supported_(false),
      packet_reader_(new QuicPacketReader()),
      client_(client),
      max_reads_per_epoll_loop_(std::numeric_limits<int>::max()) {}

QuicClientEpollNetworkHelper::~QuicClientEpollNetworkHelper() {
  if (client_->connected()) {
    client_->session()->connection()->CloseConnection(
        QUIC_PEER_GOING_AWAY, "Client being torn down",
        ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  }

  CleanUpAllUDPSockets();
}

std::string QuicClientEpollNetworkHelper::Name() const {
  return "QuicClientEpollNetworkHelper";
}

bool QuicClientEpollNetworkHelper::CreateUDPSocketAndBind(
    QuicSocketAddress server_address, QuicIpAddress bind_to_address,
    int bind_to_port) {
  epoll_server_->set_timeout_in_us(50 * 1000);

  int fd = CreateUDPSocket(server_address, &overflow_supported_);
  if (fd < 0) {
    return false;
  }
  auto closer = absl::MakeCleanup([fd] { close(fd); });

  QuicSocketAddress client_address;
  if (bind_to_address.IsInitialized()) {
    client_address = QuicSocketAddress(bind_to_address, client_->local_port());
  } else if (server_address.host().address_family() == IpAddressFamily::IP_V4) {
    client_address = QuicSocketAddress(QuicIpAddress::Any4(), bind_to_port);
  } else {
    client_address = QuicSocketAddress(QuicIpAddress::Any6(), bind_to_port);
  }

  // Some platforms expect that the addrlen given to bind() exactly matches the
  // size of the associated protocol family's sockaddr struct.
  // TODO(b/179430548): Revert this when affected platforms are updated to
  // to support binding with an addrelen of sizeof(sockaddr_storage)
  socklen_t addrlen;
  switch (client_address.host().address_family()) {
    case IpAddressFamily::IP_V4:
      addrlen = sizeof(sockaddr_in);
      break;
    case IpAddressFamily::IP_V6:
      addrlen = sizeof(sockaddr_in6);
      break;
    case IpAddressFamily::IP_UNSPEC:
      addrlen = 0;
      break;
  }

  sockaddr_storage addr = client_address.generic_address();
  int rc = bind(fd, reinterpret_cast<sockaddr*>(&addr), addrlen);
  if (rc < 0) {
    QUIC_LOG(ERROR) << "Bind failed: " << strerror(errno)
                    << " bind_to_address:" << bind_to_address
                    << ", bind_to_port:" << bind_to_port
                    << ", client_address:" << client_address;
    return false;
  }

  if (client_address.FromSocket(fd) != 0) {
    QUIC_LOG(ERROR) << "Unable to get self address.  Error: "
                    << strerror(errno);
  }

  fd_address_map_[fd] = client_address;
  epoll_server_->RegisterFD(fd, this, kEpollFlags);
  std::move(closer).Cancel();
  return true;
}

void QuicClientEpollNetworkHelper::CleanUpUDPSocket(int fd) {
  CleanUpUDPSocketImpl(fd);
  fd_address_map_.erase(fd);
}

void QuicClientEpollNetworkHelper::CleanUpAllUDPSockets() {
  for (std::pair<int, QuicSocketAddress> fd_address : fd_address_map_) {
    CleanUpUDPSocketImpl(fd_address.first);
  }
  fd_address_map_.clear();
}

void QuicClientEpollNetworkHelper::CleanUpUDPSocketImpl(int fd) {
  if (fd > -1) {
    epoll_server_->UnregisterFD(fd);
    int rc = close(fd);
    QUICHE_DCHECK_EQ(0, rc);
  }
}

void QuicClientEpollNetworkHelper::RunEventLoop() {
  quiche::QuicheRunSystemEventLoopIteration();
  epoll_server_->WaitForEventsAndExecuteCallbacks();
}

void QuicClientEpollNetworkHelper::OnRegistration(QuicEpollServer* /*eps*/,
                                                  int /*fd*/,
                                                  int /*event_mask*/) {}
void QuicClientEpollNetworkHelper::OnModification(int /*fd*/,
                                                  int /*event_mask*/) {}
void QuicClientEpollNetworkHelper::OnUnregistration(int /*fd*/,
                                                    bool /*replaced*/) {}
void QuicClientEpollNetworkHelper::OnShutdown(QuicEpollServer* /*eps*/,
                                              int /*fd*/) {}

void QuicClientEpollNetworkHelper::OnEvent(int fd, QuicEpollEvent* event) {
  if (event->in_events & EPOLLIN) {
    QUIC_DVLOG(1) << "Read packets on EPOLLIN";
    int times_to_read = max_reads_per_epoll_loop_;
    bool more_to_read = true;
    QuicPacketCount packets_dropped = 0;
    while (client_->connected() && more_to_read && times_to_read > 0) {
      more_to_read = packet_reader_->ReadAndDispatchPackets(
          fd, GetLatestClientAddress().port(), *client_->helper()->GetClock(),
          this, overflow_supported_ ? &packets_dropped : nullptr);
      --times_to_read;
    }
    if (packets_dropped_ < packets_dropped) {
      QUIC_LOG(ERROR)
          << packets_dropped - packets_dropped_
          << " more packets are dropped in the socket receive buffer.";
      packets_dropped_ = packets_dropped;
    }
    if (client_->connected() && more_to_read) {
      event->out_ready_mask |= EPOLLIN;
    }
  }
  if (client_->connected() && (event->in_events & EPOLLOUT)) {
    client_->writer()->SetWritable();
    client_->session()->connection()->OnCanWrite();
  }
  if (event->in_events & EPOLLERR) {
    QUIC_DLOG(INFO) << "Epollerr";
  }
}

QuicPacketWriter* QuicClientEpollNetworkHelper::CreateQuicPacketWriter() {
  return new QuicDefaultPacketWriter(GetLatestFD());
}

void QuicClientEpollNetworkHelper::SetClientPort(int port) {
  fd_address_map_.back().second =
      QuicSocketAddress(GetLatestClientAddress().host(), port);
}

QuicSocketAddress QuicClientEpollNetworkHelper::GetLatestClientAddress() const {
  if (fd_address_map_.empty()) {
    return QuicSocketAddress();
  }

  return fd_address_map_.back().second;
}

int QuicClientEpollNetworkHelper::GetLatestFD() const {
  if (fd_address_map_.empty()) {
    return -1;
  }

  return fd_address_map_.back().first;
}

void QuicClientEpollNetworkHelper::ProcessPacket(
    const QuicSocketAddress& self_address,
    const QuicSocketAddress& peer_address, const QuicReceivedPacket& packet) {
  client_->session()->ProcessUdpPacket(self_address, peer_address, packet);
}

int QuicClientEpollNetworkHelper::CreateUDPSocket(
    QuicSocketAddress server_address, bool* overflow_supported) {
  QuicUdpSocketApi api;
  int fd = api.Create(server_address.host().AddressFamilyToInt(),
                      /*receive_buffer_size =*/kDefaultSocketReceiveBuffer,
                      /*send_buffer_size =*/kDefaultSocketReceiveBuffer);
  if (fd < 0) {
    return fd;
  }

  *overflow_supported = api.EnableDroppedPacketCount(fd);
  api.EnableReceiveTimestamp(fd);

  std::string interface_name = client_->interface_name();
  if (!interface_name.empty()) {
    if (!api.BindInterface(fd, interface_name)) {
      QUIC_DLOG(WARNING) << "Failed to bind socket (" << fd
                         << ") to interface (" << interface_name << ").";

      CleanUpUDPSocket(fd);
      return kQuicInvalidSocketFd;
    }
  }

  return fd;
}
}  // namespace quic
