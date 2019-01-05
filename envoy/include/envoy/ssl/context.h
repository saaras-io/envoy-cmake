#pragma once

#include <memory>
#include <string>

#include "envoy/common/pure.h"

namespace Envoy {
namespace Ssl {

/**
 * SSL Context is used as a template for SSL connection configuration.
 */
class Context {
public:
  virtual ~Context() {}

  /**
   * @return the number of days in this context until the next certificate will expire
   */
  virtual size_t daysUntilFirstCertExpires() const PURE;

  /**
   * @return a string of ca certificate path, certificate serial number and days until certificate
   * expiration
   */
  virtual std::string getCaCertInformation() const PURE;

  /**
   * @return a string of cert chain certificate path, certificate serial number and days until
   * certificate expiration
   */
  virtual std::string getCertChainInformation() const PURE;
};
typedef std::shared_ptr<Context> ContextSharedPtr;

class ClientContext : public virtual Context {};
typedef std::shared_ptr<ClientContext> ClientContextSharedPtr;

class ServerContext : public virtual Context {};
typedef std::shared_ptr<ServerContext> ServerContextSharedPtr;

} // namespace Ssl
} // namespace Envoy
