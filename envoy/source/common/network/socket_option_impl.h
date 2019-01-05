#pragma once

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "envoy/api/os_sys_calls.h"
#include "envoy/network/listen_socket.h"

#include "common/common/logger.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Network {

// Optional variant of setsockopt(2) optname. The idea here is that if the option is not supported
// on a platform, we can make this the empty value. This allows us to avoid proliferation of #ifdef.
typedef absl::optional<std::pair<int, int>> SocketOptionName;

#ifdef IP_TRANSPARENT
#define ENVOY_SOCKET_IP_TRANSPARENT                                                                \
  Network::SocketOptionName(std::make_pair(IPPROTO_IP, IP_TRANSPARENT))
#else
#define ENVOY_SOCKET_IP_TRANSPARENT Network::SocketOptionName()
#endif

#ifdef IPV6_TRANSPARENT
#define ENVOY_SOCKET_IPV6_TRANSPARENT                                                              \
  Network::SocketOptionName(std::make_pair(IPPROTO_IPV6, IPV6_TRANSPARENT))
#else
#define ENVOY_SOCKET_IPV6_TRANSPARENT Network::SocketOptionName()
#endif

#ifdef IP_FREEBIND
#define ENVOY_SOCKET_IP_FREEBIND Network::SocketOptionName(std::make_pair(IPPROTO_IP, IP_FREEBIND))
#else
#define ENVOY_SOCKET_IP_FREEBIND Network::SocketOptionName()
#endif

#ifdef IPV6_FREEBIND
#define ENVOY_SOCKET_IPV6_FREEBIND                                                                 \
  Network::SocketOptionName(std::make_pair(IPPROTO_IPV6, IPV6_FREEBIND))
#else
#define ENVOY_SOCKET_IPV6_FREEBIND Network::SocketOptionName()
#endif

#ifdef SO_KEEPALIVE
#define ENVOY_SOCKET_SO_KEEPALIVE                                                                  \
  Network::SocketOptionName(std::make_pair(SOL_SOCKET, SO_KEEPALIVE))
#else
#define ENVOY_SOCKET_SO_KEEPALIVE Network::SocketOptionName()
#endif

#ifdef TCP_KEEPCNT
#define ENVOY_SOCKET_TCP_KEEPCNT Network::SocketOptionName(std::make_pair(IPPROTO_TCP, TCP_KEEPCNT))
#else
#define ENVOY_SOCKET_TCP_KEEPCNT Network::SocketOptionName()
#endif

#ifdef TCP_KEEPIDLE
#define ENVOY_SOCKET_TCP_KEEPIDLE                                                                  \
  Network::SocketOptionName(std::make_pair(IPPROTO_TCP, TCP_KEEPIDLE))
#elif TCP_KEEPALIVE // MacOS uses a different name from Linux for just this option.
#define ENVOY_SOCKET_TCP_KEEPIDLE                                                                  \
  Network::SocketOptionName(std::make_pair(IPPROTO_TCP, TCP_KEEPALIVE))
#else
#define ENVOY_SOCKET_TCP_KEEPIDLE Network::SocketOptionName()
#endif

#ifdef TCP_KEEPINTVL
#define ENVOY_SOCKET_TCP_KEEPINTVL                                                                 \
  Network::SocketOptionName(std::make_pair(IPPROTO_TCP, TCP_KEEPINTVL))
#else
#define ENVOY_SOCKET_TCP_KEEPINTVL Network::SocketOptionName()
#endif

#ifdef TCP_FASTOPEN
#define ENVOY_SOCKET_TCP_FASTOPEN                                                                  \
  Network::SocketOptionName(std::make_pair(IPPROTO_TCP, TCP_FASTOPEN))
#else
#define ENVOY_SOCKET_TCP_FASTOPEN Network::SocketOptionName()
#endif

class SocketOptionImpl : public Socket::Option, Logger::Loggable<Logger::Id::connection> {
public:
  SocketOptionImpl(envoy::api::v2::core::SocketOption::SocketState in_state,
                   Network::SocketOptionName optname, int value) // Yup, int. See setsockopt(2).
      : SocketOptionImpl(in_state, optname,
                         absl::string_view(reinterpret_cast<char*>(&value), sizeof(value))) {}

  SocketOptionImpl(envoy::api::v2::core::SocketOption::SocketState in_state,
                   Network::SocketOptionName optname, absl::string_view value)
      : in_state_(in_state), optname_(optname), value_(value) {}

  // Socket::Option
  bool setOption(Socket& socket,
                 envoy::api::v2::core::SocketOption::SocketState state) const override;

  // The common socket options don't require a hash key.
  void hashKey(std::vector<uint8_t>&) const override {}

  bool isSupported() const;

  /**
   * Set the option on the given socket.
   * @param socket the socket on which to apply the option.
   * @param optname the option name.
   * @param value the option value.
   * @return a Api::SysCallIntResult with rc_ = 0 for success and rc = -1 for failure. If the call
   * is successful, errno_ shouldn't be used.
   */
  static Api::SysCallIntResult setSocketOption(Socket& socket, Network::SocketOptionName optname,
                                               absl::string_view value);

private:
  const envoy::api::v2::core::SocketOption::SocketState in_state_;
  const Network::SocketOptionName optname_;
  const std::string value_;
};

} // namespace Network
} // namespace Envoy
