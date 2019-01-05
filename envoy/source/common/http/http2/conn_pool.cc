#include "common/http/http2/conn_pool.h"

#include <cstdint>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/upstream/upstream.h"

#include "common/http/http2/codec_impl.h"
#include "common/network/utility.h"
#include "common/upstream/upstream_impl.h"

namespace Envoy {
namespace Http {
namespace Http2 {

ConnPoolImpl::ConnPoolImpl(Event::Dispatcher& dispatcher, Upstream::HostConstSharedPtr host,
                           Upstream::ResourcePriority priority,
                           const Network::ConnectionSocket::OptionsSharedPtr& options)
    : dispatcher_(dispatcher), host_(host), priority_(priority), socket_options_(options) {}

ConnPoolImpl::~ConnPoolImpl() {
  if (primary_client_) {
    primary_client_->client_->close();
  }

  if (draining_client_) {
    draining_client_->client_->close();
  }

  // Make sure all clients are destroyed before we are destroyed.
  dispatcher_.clearDeferredDeleteList();
}

void ConnPoolImpl::ConnPoolImpl::drainConnections() {
  if (primary_client_ != nullptr) {
    movePrimaryClientToDraining();
  }
}

void ConnPoolImpl::addDrainedCallback(DrainedCb cb) {
  drained_callbacks_.push_back(cb);
  checkForDrained();
}

void ConnPoolImpl::checkForDrained() {
  if (drained_callbacks_.empty()) {
    return;
  }

  bool drained = true;
  if (primary_client_) {
    if (primary_client_->client_->numActiveRequests() == 0) {
      primary_client_->client_->close();
      ASSERT(!primary_client_);
    } else {
      drained = false;
    }
  }

  ASSERT(!draining_client_ || (draining_client_->client_->numActiveRequests() > 0));
  if (draining_client_ && draining_client_->client_->numActiveRequests() > 0) {
    drained = false;
  }

  if (drained) {
    ENVOY_LOG(debug, "invoking drained callbacks");
    for (const DrainedCb& cb : drained_callbacks_) {
      cb();
    }
  }
}

ConnectionPool::Cancellable* ConnPoolImpl::newStream(Http::StreamDecoder& response_decoder,
                                                     ConnectionPool::Callbacks& callbacks) {
  ASSERT(drained_callbacks_.empty());

  // First see if we need to handle max streams rollover.
  uint64_t max_streams = host_->cluster().maxRequestsPerConnection();
  if (max_streams == 0) {
    max_streams = maxTotalStreams();
  }

  if (primary_client_ && primary_client_->total_streams_ >= max_streams) {
    movePrimaryClientToDraining();
  }

  if (!primary_client_) {
    primary_client_.reset(new ActiveClient(*this));
  }

  if (!host_->cluster().resourceManager(priority_).requests().canCreate()) {
    ENVOY_LOG(debug, "max requests overflow");
    callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, nullptr);
    host_->cluster().stats().upstream_rq_pending_overflow_.inc();
  } else {
    ENVOY_CONN_LOG(debug, "creating stream", *primary_client_->client_);
    primary_client_->total_streams_++;
    host_->stats().rq_total_.inc();
    host_->stats().rq_active_.inc();
    host_->cluster().stats().upstream_rq_total_.inc();
    host_->cluster().stats().upstream_rq_active_.inc();
    host_->cluster().resourceManager(priority_).requests().inc();
    callbacks.onPoolReady(primary_client_->client_->newStream(response_decoder),
                          primary_client_->real_host_description_);
  }

  return nullptr;
}

void ConnPoolImpl::onConnectionEvent(ActiveClient& client, Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {

    if (client.closed_with_active_rq_) {
      host_->cluster().stats().upstream_cx_destroy_with_active_rq_.inc();
      if (event == Network::ConnectionEvent::RemoteClose) {
        host_->cluster().stats().upstream_cx_destroy_remote_with_active_rq_.inc();
      } else {
        host_->cluster().stats().upstream_cx_destroy_local_with_active_rq_.inc();
      }
    }

    if (&client == primary_client_.get()) {
      ENVOY_CONN_LOG(debug, "destroying primary client", *client.client_);
      dispatcher_.deferredDelete(std::move(primary_client_));
    } else {
      ENVOY_CONN_LOG(debug, "destroying draining client", *client.client_);
      dispatcher_.deferredDelete(std::move(draining_client_));
    }

    if (client.connect_timer_) {
      host_->cluster().stats().upstream_cx_connect_fail_.inc();
      host_->stats().cx_connect_fail_.inc();
    }

    if (client.closed_with_active_rq_) {
      checkForDrained();
    }
  }

  if (event == Network::ConnectionEvent::Connected) {
    conn_connect_ms_->complete();
  }

  if (client.connect_timer_) {
    client.connect_timer_->disableTimer();
    client.connect_timer_.reset();
  }
}

void ConnPoolImpl::movePrimaryClientToDraining() {
  ENVOY_CONN_LOG(debug, "moving primary to draining", *primary_client_->client_);
  if (draining_client_) {
    // This should pretty much never happen, but is possible if we start draining and then get
    // a goaway for example. In this case just kill the current draining connection. It's not
    // worth keeping a list.
    draining_client_->client_->close();
  }

  ASSERT(!draining_client_);
  if (primary_client_->client_->numActiveRequests() == 0) {
    // If we are making a new connection and the primary does not have any active requests just
    // close it now.
    primary_client_->client_->close();
  } else {
    draining_client_ = std::move(primary_client_);
  }

  ASSERT(!primary_client_);
}

void ConnPoolImpl::onConnectTimeout(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "connect timeout", *client.client_);
  host_->cluster().stats().upstream_cx_connect_timeout_.inc();
  client.client_->close();
}

void ConnPoolImpl::onGoAway(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "remote goaway", *client.client_);
  host_->cluster().stats().upstream_cx_close_notify_.inc();
  if (&client == primary_client_.get()) {
    movePrimaryClientToDraining();
  }
}

void ConnPoolImpl::onStreamDestroy(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "destroying stream: {} remaining", *client.client_,
                 client.client_->numActiveRequests());
  host_->stats().rq_active_.dec();
  host_->cluster().stats().upstream_rq_active_.dec();
  host_->cluster().resourceManager(priority_).requests().dec();
  if (&client == draining_client_.get() && client.client_->numActiveRequests() == 0) {
    // Close out the draining client if we no long have active requests.
    client.client_->close();
  }

  // If we are destroying this stream because of a disconnect, do not check for drain here. We will
  // wait until the connection has been fully drained of streams and then check in the connection
  // event callback.
  if (!client.closed_with_active_rq_) {
    checkForDrained();
  }
}

void ConnPoolImpl::onStreamReset(ActiveClient& client, Http::StreamResetReason reason) {
  if (reason == StreamResetReason::ConnectionTermination ||
      reason == StreamResetReason::ConnectionFailure) {
    host_->cluster().stats().upstream_rq_pending_failure_eject_.inc();
    client.closed_with_active_rq_ = true;
  } else if (reason == StreamResetReason::LocalReset) {
    host_->cluster().stats().upstream_rq_tx_reset_.inc();
  } else if (reason == StreamResetReason::RemoteReset) {
    host_->cluster().stats().upstream_rq_rx_reset_.inc();
  }
}

ConnPoolImpl::ActiveClient::ActiveClient(ConnPoolImpl& parent)
    : parent_(parent),
      connect_timer_(parent_.dispatcher_.createTimer([this]() -> void { onConnectTimeout(); })) {

  parent_.conn_connect_ms_.reset(new Stats::Timespan(
      parent_.host_->cluster().stats().upstream_cx_connect_ms_, parent_.dispatcher_.timeSystem()));
  Upstream::Host::CreateConnectionData data =
      parent_.host_->createConnection(parent_.dispatcher_, parent_.socket_options_);
  real_host_description_ = data.host_description_;
  client_ = parent_.createCodecClient(data);
  client_->addConnectionCallbacks(*this);
  client_->setCodecClientCallbacks(*this);
  client_->setCodecConnectionCallbacks(*this);
  connect_timer_->enableTimer(parent_.host_->cluster().connectTimeout());

  parent_.host_->stats().cx_total_.inc();
  parent_.host_->stats().cx_active_.inc();
  parent_.host_->cluster().stats().upstream_cx_total_.inc();
  parent_.host_->cluster().stats().upstream_cx_active_.inc();
  parent_.host_->cluster().stats().upstream_cx_http2_total_.inc();
  conn_length_.reset(new Stats::Timespan(parent_.host_->cluster().stats().upstream_cx_length_ms_,
                                         parent_.dispatcher_.timeSystem()));

  client_->setConnectionStats({parent_.host_->cluster().stats().upstream_cx_rx_bytes_total_,
                               parent_.host_->cluster().stats().upstream_cx_rx_bytes_buffered_,
                               parent_.host_->cluster().stats().upstream_cx_tx_bytes_total_,
                               parent_.host_->cluster().stats().upstream_cx_tx_bytes_buffered_,
                               &parent_.host_->cluster().stats().bind_errors_, nullptr});
}

ConnPoolImpl::ActiveClient::~ActiveClient() {
  parent_.host_->stats().cx_active_.dec();
  parent_.host_->cluster().stats().upstream_cx_active_.dec();
  conn_length_->complete();
}

CodecClientPtr ProdConnPoolImpl::createCodecClient(Upstream::Host::CreateConnectionData& data) {
  CodecClientPtr codec{new CodecClientProd(CodecClient::Type::HTTP2, std::move(data.connection_),
                                           data.host_description_, dispatcher_)};
  return codec;
}

uint32_t ProdConnPoolImpl::maxTotalStreams() { return MAX_STREAMS; }

} // namespace Http2
} // namespace Http
} // namespace Envoy
