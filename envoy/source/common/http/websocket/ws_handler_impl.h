#pragma once

#include <string>

#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"
#include "envoy/http/websocket.h"
#include "envoy/network/filter.h"
#include "envoy/router/router.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/buffer/buffer_impl.h"
#include "common/tcp_proxy/tcp_proxy.h"

namespace Envoy {
namespace Http {
namespace WebSocket {

/**
 * @return TcpProxy::ConfigSharedPtr to use in creating instances of WsHandlerImpl.
 */
TcpProxy::ConfigSharedPtr Config(const envoy::api::v2::route::RouteAction& route_config,
                                 Server::Configuration::FactoryContext& factory_context);

/**
 * An implementation of a WebSocket proxy based on TCP proxy. This will be used for
 * handling client connection only after a WebSocket upgrade request succeeds
 * (i.e, it is requested by client and allowed by config). This implementation will
 * instantiate a new outgoing TCP connection for the configured upstream cluster.
 * All data will be proxied back and forth between the two connections, without any
 * knowledge of the underlying WebSocket protocol.
 */
class WsHandlerImpl : public TcpProxy::Filter, public Http::WebSocketProxy {
public:
  WsHandlerImpl(HeaderMap& request_headers, RequestInfo::RequestInfo& request_info,
                const Router::RouteEntry& route_entry, WebSocketProxyCallbacks& callbacks,
                Upstream::ClusterManager& cluster_manager,
                Network::ReadFilterCallbacks* read_callbacks, TcpProxy::ConfigSharedPtr config,
                Event::TimeSystem& time_system);

  // Upstream::LoadBalancerContext
  const Router::MetadataMatchCriteria* metadataMatchCriteria() override {
    return route_entry_.metadataMatchCriteria();
  }

  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;

protected:
  // TcpProxy::Filter
  const std::string& getUpstreamCluster() override { return route_entry_.clusterName(); }
  void onInitFailure(UpstreamFailureReason failure_reason) override;
  void onConnectionSuccess() override;
  RequestInfo::RequestInfo& getRequestInfo() override;

private:
  struct NullHttpConnectionCallbacks : public ConnectionCallbacks {
    // Http::ConnectionCallbacks
    void onGoAway() override {}
  };

  enum class ConnectState { PreConnect, Connected, Failed };

  HeaderMap& request_headers_;
  RequestInfo::RequestInfo& request_info_;
  const Router::RouteEntry& route_entry_;
  WebSocketProxyCallbacks& ws_callbacks_;
  NullHttpConnectionCallbacks http_conn_callbacks_;
  Buffer::OwnedImpl queued_data_;
  bool queued_end_stream_{false};
  ConnectState state_{ConnectState::PreConnect};
};

typedef std::unique_ptr<WsHandlerImpl> WsHandlerImplPtr;

} // namespace WebSocket
} // namespace Http
} // namespace Envoy
