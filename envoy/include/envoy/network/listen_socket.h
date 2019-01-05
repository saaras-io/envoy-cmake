#pragma once

#include <memory>
#include <vector>

#include "envoy/api/v2/core/base.pb.h"
#include "envoy/common/pure.h"
#include "envoy/network/address.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Network {

/**
 * Base class for Sockets
 */
class Socket {
public:
  virtual ~Socket() {}

  /**
   * @return the local address of the socket.
   */
  virtual const Address::InstanceConstSharedPtr& localAddress() const PURE;

  /**
   * @return fd the socket's file descriptor.
   */
  virtual int fd() const PURE;

  /**
   * Close the underlying socket.
   */
  virtual void close() PURE;

  /**
   * Visitor class for setting socket options.
   */
  class Option {
  public:
    virtual ~Option() {}

    /**
     * @param socket the socket on which to apply options.
     * @param state the current state of the socket. Significant for options that can only be
     *        set for some particular state of the socket.
     * @return true if succeeded, false otherwise.
     */
    virtual bool setOption(Socket& socket,
                           envoy::api::v2::core::SocketOption::SocketState state) const PURE;

    /**
     * @param vector of bytes to which the option should append hash key data that will be used
     *        to separate connections based on the option. Any data already in the key vector must
     *        not be modified.
     */
    virtual void hashKey(std::vector<uint8_t>& key) const PURE;
  };
  typedef std::shared_ptr<const Option> OptionConstSharedPtr;
  typedef std::vector<OptionConstSharedPtr> Options;
  typedef std::shared_ptr<Options> OptionsSharedPtr;

  static OptionsSharedPtr& appendOptions(OptionsSharedPtr& to, const OptionsSharedPtr& from) {
    to->insert(to->end(), from->begin(), from->end());
    return to;
  }

  static bool applyOptions(const OptionsSharedPtr& options, Socket& socket,
                           envoy::api::v2::core::SocketOption::SocketState state) {
    if (options == nullptr) {
      return true;
    }
    for (const auto& option : *options) {
      if (!option->setOption(socket, state)) {
        return false;
      }
    }
    return true;
  }

  /**
   * Add a socket option visitor for later retrieval with options().
   */
  virtual void addOption(const OptionConstSharedPtr&) PURE;

  /**
   * Add socket option visitors for later retrieval with options().
   */
  virtual void addOptions(const OptionsSharedPtr&) PURE;

  /**
   * @return the socket options stored earlier with addOption() and addOptions() calls, if any.
   */
  virtual const OptionsSharedPtr& options() const PURE;
};

typedef std::unique_ptr<Socket> SocketPtr;
typedef std::shared_ptr<Socket> SocketSharedPtr;

/**
 * A socket passed to a connection. For server connections this represents the accepted socket, and
 * for client connections this represents the socket being connected to a remote address.
 *
 * TODO(jrajahalme): Hide internals (e.g., fd) from listener filters by providing callbacks filters
 * may need (set/getsockopt(), peek(), recv(), etc.)
 */
class ConnectionSocket : public virtual Socket {
public:
  virtual ~ConnectionSocket() {}

  /**
   * @return the remote address of the socket.
   */
  virtual const Address::InstanceConstSharedPtr& remoteAddress() const PURE;

  /**
   * Set the local address of the socket. On accepted sockets the local address defaults to the
   * one at which the connection was received at, which is the same as the listener's address, if
   * the listener is bound to a specific address.
   *
   * @param local_address the new local address.
   * @param restored a flag marking the local address as being restored to a value that is
   *        different from the one the socket was initially accepted at. This should only be set
   *        to 'true' when restoring the original destination address of a connection redirected
   *        by iptables REDIRECT. The caller is responsible for making sure the new address is
   *        actually different when passing restored as 'true'.
   */
  virtual void setLocalAddress(const Address::InstanceConstSharedPtr& local_address,
                               bool restored) PURE;

  /**
   * Set the remote address of the socket.
   */
  virtual void setRemoteAddress(const Address::InstanceConstSharedPtr& remote_address) PURE;

  /**
   * @return true if the local address has been restored to a value that is different from the
   *         address the socket was initially accepted at.
   */
  virtual bool localAddressRestored() const PURE;

  /**
   * Set detected transport protocol (e.g. RAW_BUFFER, TLS).
   */
  virtual void setDetectedTransportProtocol(absl::string_view protocol) PURE;

  /**
   * @return detected transport protocol (e.g. RAW_BUFFER, TLS), if any.
   */
  virtual absl::string_view detectedTransportProtocol() const PURE;

  /**
   * Set requested application protocol(s) (e.g. ALPN in TLS).
   */
  virtual void
  setRequestedApplicationProtocols(const std::vector<absl::string_view>& protocol) PURE;

  /**
   * @return requested application protocol(s) (e.g. ALPN in TLS), if any.
   */
  virtual const std::vector<std::string>& requestedApplicationProtocols() const PURE;

  /**
   * Set requested server name (e.g. SNI in TLS).
   */
  virtual void setRequestedServerName(absl::string_view server_name) PURE;

  /**
   * @return requested server name (e.g. SNI in TLS), if any.
   */
  virtual absl::string_view requestedServerName() const PURE;
};

typedef std::unique_ptr<ConnectionSocket> ConnectionSocketPtr;

} // namespace Network
} // namespace Envoy
