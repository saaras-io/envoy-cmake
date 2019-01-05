#pragma once

#include <memory>

#include "envoy/api/v2/listener/listener.pb.h"
#include "envoy/network/filter.h"
#include "envoy/server/filter_config.h"
#include "envoy/server/instance.h"
#include "envoy/server/listener_manager.h"
#include "envoy/server/transport_socket_config.h"
#include "envoy/server/worker.h"
#include "envoy/stats/scope.h"

#include "common/common/logger.h"
#include "common/network/cidr_range.h"
#include "common/network/lc_trie.h"

#include "server/init_manager_impl.h"
#include "server/lds_api.h"

namespace Envoy {
namespace Server {

/**
 * Prod implementation of ListenerComponentFactory that creates real sockets and attempts to fetch
 * sockets from the parent process via the hot restarter. The filter factory list is created from
 * statically registered filters.
 */
class ProdListenerComponentFactory : public ListenerComponentFactory,
                                     Logger::Loggable<Logger::Id::config> {
public:
  ProdListenerComponentFactory(Instance& server) : server_(server) {}

  /**
   * Static worker for createNetworkFilterFactoryList() that can be used directly in tests.
   */
  static std::vector<Network::FilterFactoryCb> createNetworkFilterFactoryList_(
      const Protobuf::RepeatedPtrField<envoy::api::v2::listener::Filter>& filters,
      Configuration::FactoryContext& context);
  /**
   * Static worker for createListenerFilterFactoryList() that can be used directly in tests.
   */
  static std::vector<Network::ListenerFilterFactoryCb> createListenerFilterFactoryList_(
      const Protobuf::RepeatedPtrField<envoy::api::v2::listener::ListenerFilter>& filters,
      Configuration::ListenerFactoryContext& context);

  // Server::ListenerComponentFactory
  LdsApiPtr createLdsApi(const envoy::api::v2::core::ConfigSource& lds_config) override {
    return std::make_unique<LdsApiImpl>(
        lds_config, server_.clusterManager(), server_.dispatcher(), server_.random(),
        server_.initManager(), server_.localInfo(), server_.stats(), server_.listenerManager());
  }
  std::vector<Network::FilterFactoryCb> createNetworkFilterFactoryList(
      const Protobuf::RepeatedPtrField<envoy::api::v2::listener::Filter>& filters,
      Configuration::FactoryContext& context) override {
    return createNetworkFilterFactoryList_(filters, context);
  }
  std::vector<Network::ListenerFilterFactoryCb> createListenerFilterFactoryList(
      const Protobuf::RepeatedPtrField<envoy::api::v2::listener::ListenerFilter>& filters,
      Configuration::ListenerFactoryContext& context) override {
    return createListenerFilterFactoryList_(filters, context);
  }
  Network::SocketSharedPtr createListenSocket(Network::Address::InstanceConstSharedPtr address,
                                              const Network::Socket::OptionsSharedPtr& options,
                                              bool bind_to_port) override;
  DrainManagerPtr createDrainManager(envoy::api::v2::Listener::DrainType drain_type) override;
  uint64_t nextListenerTag() override { return next_listener_tag_++; }

private:
  Instance& server_;
  uint64_t next_listener_tag_{1};
};

class ListenerImpl;
typedef std::unique_ptr<ListenerImpl> ListenerImplPtr;

/**
 * All listener manager stats. @see stats_macros.h
 */
// clang-format off
#define ALL_LISTENER_MANAGER_STATS(COUNTER, GAUGE)                                                 \
  COUNTER(listener_added)                                                                          \
  COUNTER(listener_modified)                                                                       \
  COUNTER(listener_removed)                                                                        \
  COUNTER(listener_create_success)                                                                 \
  COUNTER(listener_create_failure)                                                                 \
  GAUGE  (total_listeners_warming)                                                                 \
  GAUGE  (total_listeners_active)                                                                  \
  GAUGE  (total_listeners_draining)
// clang-format on

/**
 * Struct definition for all listener manager stats. @see stats_macros.h
 */
struct ListenerManagerStats {
  ALL_LISTENER_MANAGER_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

/**
 * Implementation of ListenerManager.
 */
class ListenerManagerImpl : public ListenerManager, Logger::Loggable<Logger::Id::config> {
public:
  ListenerManagerImpl(Instance& server, ListenerComponentFactory& listener_factory,
                      WorkerFactory& worker_factory, TimeSource& time_source);

  void onListenerWarmed(ListenerImpl& listener);

  // Server::ListenerManager
  bool addOrUpdateListener(const envoy::api::v2::Listener& config, const std::string& version_info,
                           bool modifiable) override;
  void createLdsApi(const envoy::api::v2::core::ConfigSource& lds_config) override {
    ASSERT(lds_api_ == nullptr);
    lds_api_ = factory_.createLdsApi(lds_config);
  }
  std::vector<std::reference_wrapper<Network::ListenerConfig>> listeners() override;
  uint64_t numConnections() override;
  bool removeListener(const std::string& listener_name) override;
  void startWorkers(GuardDog& guard_dog) override;
  void stopListeners() override;
  void stopWorkers() override;

  Instance& server_;
  TimeSource& time_source_;
  ListenerComponentFactory& factory_;

private:
  typedef std::list<ListenerImplPtr> ListenerList;

  struct DrainingListener {
    DrainingListener(ListenerImplPtr&& listener, uint64_t workers_pending_removal)
        : listener_(std::move(listener)), workers_pending_removal_(workers_pending_removal) {}

    ListenerImplPtr listener_;
    uint64_t workers_pending_removal_;
  };

  void addListenerToWorker(Worker& worker, ListenerImpl& listener);
  ProtobufTypes::MessagePtr dumpListenerConfigs();
  static ListenerManagerStats generateStats(Stats::Scope& scope);
  static bool hasListenerWithAddress(const ListenerList& list,
                                     const Network::Address::Instance& address);
  void updateWarmingActiveGauges() {
    // Using set() avoids a multiple modifiers problem during the multiple processes phase of hot
    // restart.
    stats_.total_listeners_warming_.set(warming_listeners_.size());
    stats_.total_listeners_active_.set(active_listeners_.size());
  }

  /**
   * Mark a listener for draining. The listener will no longer be considered active but will remain
   * present to allow connection draining.
   * @param listener supplies the listener to drain.
   */
  void drainListener(ListenerImplPtr&& listener);

  /**
   * Get a listener by name. This routine is used because listeners have inherent order in static
   * configuration and especially for tests. Thus, we can't use a map.
   * @param listeners supplies the listener list to look in.
   * @param name supplies the name to search for.
   */
  ListenerList::iterator getListenerByName(ListenerList& listeners, const std::string& name);

  // Active listeners are listeners that are currently accepting new connections on the workers.
  ListenerList active_listeners_;
  // Warming listeners are listeners that may need further initialization via the listener's init
  // manager. For example, RDS, or in the future KDS. Once a listener is done warming it will
  // be transitioned to active.
  ListenerList warming_listeners_;
  // Draining listeners are listeners that are in the process of being drained and removed. They
  // go through two phases where first the workers stop accepting new connections and existing
  // connections are drained. Then after that time period the listener is removed from all workers
  // and any remaining connections are closed.
  std::list<DrainingListener> draining_listeners_;
  std::list<WorkerPtr> workers_;
  bool workers_started_{};
  ListenerManagerStats stats_;
  ConfigTracker::EntryOwnerPtr config_tracker_entry_;
  LdsApiPtr lds_api_;
};

// TODO(mattklein123): Consider getting rid of pre-worker start and post-worker start code by
//                     initializing all listeners after workers are started.

/**
 * Maps proto config to runtime config for a listener with a network filter chain.
 */
class ListenerImpl : public Network::ListenerConfig,
                     public Configuration::ListenerFactoryContext,
                     public Network::DrainDecision,
                     public Network::FilterChainManager,
                     public Network::FilterChainFactory,
                     Logger::Loggable<Logger::Id::config> {
public:
  /**
   * Create a new listener.
   * @param config supplies the configuration proto.
   * @param version_info supplies the xDS version of the listener.
   * @param parent supplies the owning manager.
   * @param name supplies the listener name.
   * @param modifiable supplies whether the listener can be updated or removed.
   * @param workers_started supplies whether the listener is being added before or after workers
   *        have been started. This controls various behavior related to init management.
   * @param hash supplies the hash to use for duplicate checking.
   */
  ListenerImpl(const envoy::api::v2::Listener& config, const std::string& version_info,
               ListenerManagerImpl& parent, const std::string& name, bool modifiable,
               bool workers_started, uint64_t hash);
  ~ListenerImpl();

  /**
   * Helper functions to determine whether a listener is blocked for update or remove.
   */
  bool blockUpdate(uint64_t new_hash) { return new_hash == hash_ || !modifiable_; }
  bool blockRemove() { return !modifiable_; }

  /**
   * Called when a listener failed to be actually created on a worker.
   * @return TRUE if we have seen more than one worker failure.
   */
  bool onListenerCreateFailure() {
    bool ret = saw_listener_create_failure_;
    saw_listener_create_failure_ = true;
    return ret;
  }

  Network::Address::InstanceConstSharedPtr address() const { return address_; }
  const envoy::api::v2::Listener& config() { return config_; }
  const Network::SocketSharedPtr& getSocket() const { return socket_; }
  void debugLog(const std::string& message);
  void initialize();
  DrainManager& localDrainManager() const { return *local_drain_manager_; }
  void setSocket(const Network::SocketSharedPtr& socket);
  void setSocketAndOptions(const Network::SocketSharedPtr& socket);
  const Network::Socket::OptionsSharedPtr& listenSocketOptions() { return listen_socket_options_; }
  const std::string& versionInfo() { return version_info_; }

  // Network::ListenerConfig
  Network::FilterChainManager& filterChainManager() override { return *this; }
  Network::FilterChainFactory& filterChainFactory() override { return *this; }
  Network::Socket& socket() override { return *socket_; }
  bool bindToPort() override { return bind_to_port_; }
  bool handOffRestoredDestinationConnections() const override {
    return hand_off_restored_destination_connections_;
  }
  uint32_t perConnectionBufferLimitBytes() override { return per_connection_buffer_limit_bytes_; }
  Stats::Scope& listenerScope() override { return *listener_scope_; }
  uint64_t listenerTag() const override { return listener_tag_; }
  const std::string& name() const override { return name_; }

  // Server::Configuration::ListenerFactoryContext
  AccessLog::AccessLogManager& accessLogManager() override {
    return parent_.server_.accessLogManager();
  }
  Upstream::ClusterManager& clusterManager() override { return parent_.server_.clusterManager(); }
  Event::Dispatcher& dispatcher() override { return parent_.server_.dispatcher(); }
  Network::DrainDecision& drainDecision() override { return *this; }
  bool healthCheckFailed() override { return parent_.server_.healthCheckFailed(); }
  Tracing::HttpTracer& httpTracer() override { return parent_.server_.httpTracer(); }
  Init::Manager& initManager() override;
  const LocalInfo::LocalInfo& localInfo() const override { return parent_.server_.localInfo(); }
  Envoy::Runtime::RandomGenerator& random() override { return parent_.server_.random(); }
  RateLimit::ClientPtr
  rateLimitClient(const absl::optional<std::chrono::milliseconds>& timeout) override {
    return parent_.server_.rateLimitClient(timeout);
  }
  Envoy::Runtime::Loader& runtime() override { return parent_.server_.runtime(); }
  Stats::Scope& scope() override { return *global_scope_; }
  Singleton::Manager& singletonManager() override { return parent_.server_.singletonManager(); }
  OverloadManager& overloadManager() override { return parent_.server_.overloadManager(); }
  ThreadLocal::Instance& threadLocal() override { return parent_.server_.threadLocal(); }
  Admin& admin() override { return parent_.server_.admin(); }
  const envoy::api::v2::core::Metadata& listenerMetadata() const override {
    return config_.metadata();
  };
  TimeSource& timeSource() override { return parent_.time_source_; }
  void ensureSocketOptions() {
    if (!listen_socket_options_) {
      listen_socket_options_ =
          std::make_shared<std::vector<Network::Socket::OptionConstSharedPtr>>();
    }
  }
  void addListenSocketOption(const Network::Socket::OptionConstSharedPtr& option) override {
    ensureSocketOptions();
    listen_socket_options_->emplace_back(std::move(option));
  }
  void addListenSocketOptions(const Network::Socket::OptionsSharedPtr& options) override {
    ensureSocketOptions();
    Network::Socket::appendOptions(listen_socket_options_, options);
  }

  // Network::DrainDecision
  bool drainClose() const override;

  // Network::FilterChainManager
  const Network::FilterChain*
  findFilterChain(const Network::ConnectionSocket& socket) const override;

  // Network::FilterChainFactory
  bool createNetworkFilterChain(Network::Connection& connection,
                                const std::vector<Network::FilterFactoryCb>& factories) override;
  bool createListenerFilterChain(Network::ListenerFilterManager& manager) override;

  SystemTime last_updated_;

private:
  typedef std::unordered_map<std::string, Network::FilterChainSharedPtr> ApplicationProtocolsMap;
  typedef std::unordered_map<std::string, ApplicationProtocolsMap> TransportProtocolsMap;
  // Both exact server names and wildcard domains are part of the same map, in which wildcard
  // domains are prefixed with "." (i.e. ".example.com" for "*.example.com") to differentiate
  // between exact and wildcard entries.
  typedef std::unordered_map<std::string, TransportProtocolsMap> ServerNamesMap;
  typedef std::unordered_map<std::string, ServerNamesMap> DestinationIPsMap;
  typedef std::shared_ptr<ServerNamesMap> ServerNamesMapSharedPtr;
  typedef Network::LcTrie::LcTrie<ServerNamesMapSharedPtr> DestinationIPsTrie;
  typedef std::unique_ptr<DestinationIPsTrie> DestinationIPsTriePtr;
  typedef std::unordered_map<uint16_t, std::pair<DestinationIPsMap, DestinationIPsTriePtr>>
      DestinationPortsMap;

  void addFilterChain(uint16_t destination_port, const std::vector<std::string>& destination_ips,
                      const std::vector<std::string>& server_names,
                      const std::string& transport_protocol,
                      const std::vector<std::string>& application_protocols,
                      Network::TransportSocketFactoryPtr&& transport_socket_factory,
                      std::vector<Network::FilterFactoryCb> filters_factory);
  void addFilterChainForDestinationPorts(DestinationPortsMap& destination_ports_map,
                                         uint16_t destination_port,
                                         const std::vector<std::string>& destination_ips,
                                         const std::vector<std::string>& server_names,
                                         const std::string& transport_protocol,
                                         const std::vector<std::string>& application_protocols,
                                         const Network::FilterChainSharedPtr& filter_chain);
  void addFilterChainForDestinationIPs(DestinationIPsMap& destination_ips_map,
                                       const std::vector<std::string>& destination_ips,
                                       const std::vector<std::string>& server_names,
                                       const std::string& transport_protocol,
                                       const std::vector<std::string>& application_protocols,
                                       const Network::FilterChainSharedPtr& filter_chain);
  void addFilterChainForServerNames(ServerNamesMap& server_names_map,
                                    const std::vector<std::string>& server_names,
                                    const std::string& transport_protocol,
                                    const std::vector<std::string>& application_protocols,
                                    const Network::FilterChainSharedPtr& filter_chain);
  void addFilterChainForApplicationProtocols(ApplicationProtocolsMap& application_protocol_map,
                                             const std::vector<std::string>& application_protocols,
                                             const Network::FilterChainSharedPtr& filter_chain);

  void convertDestinationIPsMapToTrie();

  const Network::FilterChain*
  findFilterChainForDestinationIP(const DestinationIPsTrie& destination_ips_trie,
                                  const Network::ConnectionSocket& socket) const;
  const Network::FilterChain*
  findFilterChainForServerName(const ServerNamesMap& server_names_map,
                               const Network::ConnectionSocket& socket) const;
  const Network::FilterChain*
  findFilterChainForTransportProtocol(const TransportProtocolsMap& transport_protocols_map,
                                      const Network::ConnectionSocket& socket) const;
  const Network::FilterChain*
  findFilterChainForApplicationProtocols(const ApplicationProtocolsMap& application_protocols_map,
                                         const Network::ConnectionSocket& socket) const;

  static bool isWildcardServerName(const std::string& name);

  // Mapping of FilterChain's configured destination ports, IPs, server names, transport protocols
  // and application protocols, using structures defined above.
  DestinationPortsMap destination_ports_map_;

  ListenerManagerImpl& parent_;
  Network::Address::InstanceConstSharedPtr address_;
  Network::SocketSharedPtr socket_;
  Stats::ScopePtr global_scope_;   // Stats with global named scope, but needed for LDS cleanup.
  Stats::ScopePtr listener_scope_; // Stats with listener named scope.
  const bool bind_to_port_;
  const bool hand_off_restored_destination_connections_;
  const uint32_t per_connection_buffer_limit_bytes_;
  const uint64_t listener_tag_;
  const std::string name_;
  const bool modifiable_;
  const bool workers_started_;
  const uint64_t hash_;
  InitManagerImpl dynamic_init_manager_;
  bool initialize_canceled_{};
  std::vector<Network::ListenerFilterFactoryCb> listener_filter_factories_;
  DrainManagerPtr local_drain_manager_;
  bool saw_listener_create_failure_{};
  const envoy::api::v2::Listener config_;
  const std::string version_info_;
  Network::Socket::OptionsSharedPtr listen_socket_options_;
};

class FilterChainImpl : public Network::FilterChain {
public:
  FilterChainImpl(Network::TransportSocketFactoryPtr&& transport_socket_factory,
                  std::vector<Network::FilterFactoryCb> filters_factory)
      : transport_socket_factory_(std::move(transport_socket_factory)),
        filters_factory_(std::move(filters_factory)) {}

  // Network::FilterChain
  const Network::TransportSocketFactory& transportSocketFactory() const override {
    return *transport_socket_factory_;
  }

  const std::vector<Network::FilterFactoryCb>& networkFilterFactories() const override {
    return filters_factory_;
  }

private:
  const Network::TransportSocketFactoryPtr transport_socket_factory_;
  const std::vector<Network::FilterFactoryCb> filters_factory_;
};

} // namespace Server
} // namespace Envoy
