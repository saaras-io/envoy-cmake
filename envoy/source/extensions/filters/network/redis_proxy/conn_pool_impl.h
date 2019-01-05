#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "envoy/config/filter/network/redis_proxy/v2/redis_proxy.pb.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/buffer/buffer_impl.h"
#include "common/network/filter_impl.h"
#include "common/protobuf/utility.h"
#include "common/upstream/load_balancer_impl.h"

#include "extensions/filters/network/redis_proxy/codec_impl.h"
#include "extensions/filters/network/redis_proxy/conn_pool.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RedisProxy {
namespace ConnPool {

// TODO(mattklein123): Circuit breaking
// TODO(rshriram): Fault injection

class ConfigImpl : public Config {
public:
  ConfigImpl(
      const envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings& config);

  bool disableOutlierEvents() const override { return false; }
  std::chrono::milliseconds opTimeout() const override { return op_timeout_; }

private:
  const std::chrono::milliseconds op_timeout_;
};

class ClientImpl : public Client, public DecoderCallbacks, public Network::ConnectionCallbacks {
public:
  static ClientPtr create(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                          EncoderPtr&& encoder, DecoderFactory& decoder_factory,
                          const Config& config);

  ~ClientImpl();

  // RedisProxy::ConnPool::Client
  void addConnectionCallbacks(Network::ConnectionCallbacks& callbacks) override {
    connection_->addConnectionCallbacks(callbacks);
  }
  void close() override;
  PoolRequest* makeRequest(const RespValue& request, PoolCallbacks& callbacks) override;

private:
  struct UpstreamReadFilter : public Network::ReadFilterBaseImpl {
    UpstreamReadFilter(ClientImpl& parent) : parent_(parent) {}

    // Network::ReadFilter
    Network::FilterStatus onData(Buffer::Instance& data, bool) override {
      parent_.onData(data);
      return Network::FilterStatus::Continue;
    }

    ClientImpl& parent_;
  };

  struct PendingRequest : public PoolRequest {
    PendingRequest(ClientImpl& parent, PoolCallbacks& callbacks);
    ~PendingRequest();

    // RedisProxy::ConnPool::PoolRequest
    void cancel() override;

    ClientImpl& parent_;
    PoolCallbacks& callbacks_;
    bool canceled_{};
  };

  ClientImpl(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher, EncoderPtr&& encoder,
             DecoderFactory& decoder_factory, const Config& config);
  void onConnectOrOpTimeout();
  void onData(Buffer::Instance& data);
  void putOutlierEvent(Upstream::Outlier::Result result);

  // RedisProxy::DecoderCallbacks
  void onRespValue(RespValuePtr&& value) override;

  // Network::ConnectionCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  Upstream::HostConstSharedPtr host_;
  Network::ClientConnectionPtr connection_;
  EncoderPtr encoder_;
  Buffer::OwnedImpl encoder_buffer_;
  DecoderPtr decoder_;
  const Config& config_;
  std::list<PendingRequest> pending_requests_;
  Event::TimerPtr connect_or_op_timer_;
  bool connected_{};
};

class ClientFactoryImpl : public ClientFactory {
public:
  // RedisProxy::ConnPool::ClientFactoryImpl
  ClientPtr create(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                   const Config& config) override;

  static ClientFactoryImpl instance_;

private:
  DecoderFactoryImpl decoder_factory_;
};

class InstanceImpl : public Instance {
public:
  InstanceImpl(
      const std::string& cluster_name, Upstream::ClusterManager& cm, ClientFactory& client_factory,
      ThreadLocal::SlotAllocator& tls,
      const envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings& config);

  // RedisProxy::ConnPool::Instance
  PoolRequest* makeRequest(const std::string& hash_key, const RespValue& request,
                           PoolCallbacks& callbacks) override;

private:
  struct ThreadLocalPool;

  struct ThreadLocalActiveClient : public Network::ConnectionCallbacks {
    ThreadLocalActiveClient(ThreadLocalPool& parent) : parent_(parent) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    ThreadLocalPool& parent_;
    Upstream::HostConstSharedPtr host_;
    ClientPtr redis_client_;
  };

  typedef std::unique_ptr<ThreadLocalActiveClient> ThreadLocalActiveClientPtr;

  struct ThreadLocalPool : public ThreadLocal::ThreadLocalObject {
    ThreadLocalPool(InstanceImpl& parent, Event::Dispatcher& dispatcher,
                    const std::string& cluster_name);
    ~ThreadLocalPool();
    PoolRequest* makeRequest(const std::string& hash_key, const RespValue& request,
                             PoolCallbacks& callbacks);
    void onHostsRemoved(const std::vector<Upstream::HostSharedPtr>& hosts_removed);

    InstanceImpl& parent_;
    Event::Dispatcher& dispatcher_;
    Upstream::ThreadLocalCluster* cluster_;
    std::unordered_map<Upstream::HostConstSharedPtr, ThreadLocalActiveClientPtr> client_map_;
    Envoy::Common::CallbackHandle* local_host_set_member_update_cb_handle_;
  };

  struct LbContextImpl : public Upstream::LoadBalancerContextBase {
    LbContextImpl(const std::string& hash_key) : hash_key_(std::hash<std::string>()(hash_key)) {}
    // TODO(danielhochman): convert to HashUtil::xxHash64 when we have a migration strategy.
    // Upstream::LoadBalancerContext
    absl::optional<uint64_t> computeHashKey() override { return hash_key_; }

    const absl::optional<uint64_t> hash_key_;
  };

  Upstream::ClusterManager& cm_;
  ClientFactory& client_factory_;
  ThreadLocal::SlotPtr tls_;
  ConfigImpl config_;
};

} // namespace ConnPool
} // namespace RedisProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
