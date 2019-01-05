#include "envoy/admin/v2alpha/config_dump.pb.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "common/api/os_sys_calls_impl.h"
#include "common/config/metadata.h"
#include "common/network/address_impl.h"
#include "common/network/listen_socket_impl.h"
#include "common/network/socket_option_impl.h"
#include "common/network/utility.h"
#include "common/ssl/ssl_socket.h"

#include "server/configuration_impl.h"
#include "server/listener_manager_impl.h"

#include "extensions/filters/listener/original_dst/original_dst.h"

#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/server/utility.h"
#include "test/test_common/environment.h"
#include "test/test_common/registry.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/threadsafe_singleton_injector.h"
#include "test/test_common/utility.h"

#include "absl/strings/match.h"
#include "gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::Throw;

namespace Envoy {
namespace Server {

class ListenerHandle {
public:
  ListenerHandle() { EXPECT_CALL(*drain_manager_, startParentShutdownSequence()).Times(0); }
  ~ListenerHandle() { onDestroy(); }

  MOCK_METHOD0(onDestroy, void());

  Init::MockTarget target_;
  MockDrainManager* drain_manager_ = new MockDrainManager();
  Configuration::FactoryContext* context_{};
};

class ListenerManagerImplTest : public testing::Test {
public:
  ListenerManagerImplTest() {
    EXPECT_CALL(worker_factory_, createWorker_()).WillOnce(Return(worker_));
    manager_.reset(
        new ListenerManagerImpl(server_, listener_factory_, worker_factory_, time_system_));
  }

  /**
   * This routing sets up an expectation that does various things:
   * 1) Allows us to track listener destruction via filter factory destruction.
   * 2) Allows us to register for init manager handling much like RDS, etc. would do.
   * 3) Stores the factory context for later use.
   * 4) Creates a mock local drain manager for the listener.
   */
  ListenerHandle* expectListenerCreate(
      bool need_init,
      envoy::api::v2::Listener::DrainType drain_type = envoy::api::v2::Listener_DrainType_DEFAULT) {
    ListenerHandle* raw_listener = new ListenerHandle();
    EXPECT_CALL(listener_factory_, createDrainManager_(drain_type))
        .WillOnce(Return(raw_listener->drain_manager_));
    EXPECT_CALL(listener_factory_, createNetworkFilterFactoryList(_, _))
        .WillOnce(Invoke(
            [raw_listener, need_init](
                const Protobuf::RepeatedPtrField<envoy::api::v2::listener::Filter>&,
                Configuration::FactoryContext& context) -> std::vector<Network::FilterFactoryCb> {
              std::shared_ptr<ListenerHandle> notifier(raw_listener);
              raw_listener->context_ = &context;
              if (need_init) {
                context.initManager().registerTarget(notifier->target_);
              }
              return {[notifier](Network::FilterManager&) -> void {}};
            }));

    return raw_listener;
  }

  void checkStats(uint64_t added, uint64_t modified, uint64_t removed, uint64_t warming,
                  uint64_t active, uint64_t draining) {
    EXPECT_EQ(added, server_.stats_store_.counter("listener_manager.listener_added").value());
    EXPECT_EQ(modified, server_.stats_store_.counter("listener_manager.listener_modified").value());
    EXPECT_EQ(removed, server_.stats_store_.counter("listener_manager.listener_removed").value());
    EXPECT_EQ(warming,
              server_.stats_store_.gauge("listener_manager.total_listeners_warming").value());
    EXPECT_EQ(active,
              server_.stats_store_.gauge("listener_manager.total_listeners_active").value());
    EXPECT_EQ(draining,
              server_.stats_store_.gauge("listener_manager.total_listeners_draining").value());
  }

  void checkConfigDump(const std::string& expected_dump_yaml) {
    auto message_ptr = server_.admin_.config_tracker_.config_tracker_callbacks_["listeners"]();
    const auto& listeners_config_dump =
        dynamic_cast<const envoy::admin::v2alpha::ListenersConfigDump&>(*message_ptr);

    envoy::admin::v2alpha::ListenersConfigDump expected_listeners_config_dump;
    MessageUtil::loadFromYaml(expected_dump_yaml, expected_listeners_config_dump);
    EXPECT_EQ(expected_listeners_config_dump.DebugString(), listeners_config_dump.DebugString());
  }

  NiceMock<MockInstance> server_;
  NiceMock<MockListenerComponentFactory> listener_factory_;
  MockWorker* worker_ = new MockWorker();
  NiceMock<MockWorkerFactory> worker_factory_;
  std::unique_ptr<ListenerManagerImpl> manager_;
  NiceMock<MockGuardDog> guard_dog_;
  Event::SimulatedTimeSystem time_system_;
};

class ListenerManagerImplWithRealFiltersTest : public ListenerManagerImplTest {
public:
  ListenerManagerImplWithRealFiltersTest() {
    // Use real filter loading by default.
    ON_CALL(listener_factory_, createNetworkFilterFactoryList(_, _))
        .WillByDefault(Invoke(
            [](const Protobuf::RepeatedPtrField<envoy::api::v2::listener::Filter>& filters,
               Configuration::FactoryContext& context) -> std::vector<Network::FilterFactoryCb> {
              return ProdListenerComponentFactory::createNetworkFilterFactoryList_(filters,
                                                                                   context);
            }));
    ON_CALL(listener_factory_, createListenerFilterFactoryList(_, _))
        .WillByDefault(Invoke(
            [](const Protobuf::RepeatedPtrField<envoy::api::v2::listener::ListenerFilter>& filters,
               Configuration::ListenerFactoryContext& context)
                -> std::vector<Network::ListenerFilterFactoryCb> {
              return ProdListenerComponentFactory::createListenerFilterFactoryList_(filters,
                                                                                    context);
            }));
    socket_.reset(new NiceMock<Network::MockConnectionSocket>());
    address_.reset(new Network::Address::Ipv4Instance("127.0.0.1", 1234));
  }

  const Network::FilterChain*
  findFilterChain(uint16_t destination_port, bool expect_destination_port_match,
                  const std::string& destination_address, bool expect_destination_address_match,
                  const std::string& server_name, bool expect_server_name_match,
                  const std::string& transport_protocol, bool expect_transport_protocol_match,
                  const std::vector<std::string>& application_protocols) {
    const int times = expect_destination_port_match ? 2 : 1;
    if (absl::StartsWith(destination_address, "/")) {
      address_.reset(new Network::Address::PipeInstance(destination_address));
    } else {
      address_.reset(new Network::Address::Ipv4Instance(destination_address, destination_port));
    }
    EXPECT_CALL(*socket_, localAddress()).Times(times).WillRepeatedly(ReturnRef(address_));

    if (expect_destination_address_match) {
      EXPECT_CALL(*socket_, requestedServerName()).WillOnce(Return(absl::string_view(server_name)));
    } else {
      EXPECT_CALL(*socket_, requestedServerName()).Times(0);
    }

    if (expect_server_name_match) {
      EXPECT_CALL(*socket_, detectedTransportProtocol())
          .WillOnce(Return(absl::string_view(transport_protocol)));
    } else {
      EXPECT_CALL(*socket_, detectedTransportProtocol()).Times(0);
    }

    if (expect_transport_protocol_match) {
      EXPECT_CALL(*socket_, requestedApplicationProtocols())
          .WillOnce(ReturnRef(application_protocols));
    } else {
      EXPECT_CALL(*socket_, requestedApplicationProtocols()).Times(0);
    }

    return manager_->listeners().back().get().filterChainManager().findFilterChain(*socket_);
  }

private:
  std::unique_ptr<Network::MockConnectionSocket> socket_;
  Network::Address::InstanceConstSharedPtr address_;
};

class MockLdsApi : public LdsApi {
public:
  MOCK_CONST_METHOD0(versionInfo, std::string());
};

TEST_F(ListenerManagerImplWithRealFiltersTest, EmptyFilter) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, DefaultListenerPerConnectionBufferLimit) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(1024 * 1024U, manager_->listeners().back().get().perConnectionBufferLimitBytes());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SetListenerPerConnectionBufferLimit) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "per_connection_buffer_limit_bytes": 8192
  }
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(8192U, manager_->listeners().back().get().perConnectionBufferLimitBytes());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SslContext) {
  const std::string json = TestEnvironment::substitute(R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters" : [],
    "ssl_context" : {
      "cert_chain_file" : "{{ test_rundir }}/test/common/ssl/test_data/san_uri_cert.pem",
      "private_key_file" : "{{ test_rundir }}/test/common/ssl/test_data/san_uri_key.pem",
      "ca_cert_file" : "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.pem",
      "verify_subject_alt_name" : [
        "localhost",
        "127.0.0.1"
      ]
    }
  }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, BadListenerConfig) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "test": "a"
  }
  )EOF";

  EXPECT_THROW(manager_->addOrUpdateListener(parseListenerFromJson(json), "", true),
               Json::Exception);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, BadFilterConfig) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      {
        "type" : "type",
        "name" : "name",
        "config" : {}
      }
    ]
  }
  )EOF";

  EXPECT_THROW(manager_->addOrUpdateListener(parseListenerFromJson(json), "", true),
               Json::Exception);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, BadFilterName) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      {
        "type" : "write",
        "name" : "invalid",
        "config" : {}
      }
    ]
  }
  )EOF";

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromJson(json), "", true),
                            EnvoyException,
                            "Didn't find a registered implementation for name: 'invalid'");
}

class TestStatsConfigFactory : public Configuration::NamedNetworkFilterConfigFactory {
public:
  // Configuration::NamedNetworkFilterConfigFactory
  Network::FilterFactoryCb createFilterFactory(const Json::Object&,
                                               Configuration::FactoryContext& context) override {
    context.scope().counter("bar").inc();
    return [](Network::FilterManager&) -> void {};
  }
  std::string name() override { return "stats_test"; }
};

TEST_F(ListenerManagerImplWithRealFiltersTest, StatsScopeTest) {
  Registry::RegisterFactory<TestStatsConfigFactory, Configuration::NamedNetworkFilterConfigFactory>
      registered;

  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "bind_to_port": false,
    "filters": [
      {
        "type" : "read",
        "name" : "stats_test",
        "config" : {}
      }
    ]
  }
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, false));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  manager_->listeners().front().get().listenerScope().counter("foo").inc();

  EXPECT_EQ(1UL, server_.stats_store_.counter("bar").value());
  EXPECT_EQ(1UL, server_.stats_store_.counter("listener.127.0.0.1_1234.foo").value());
}

TEST_F(ListenerManagerImplTest, ModifyOnlyDrainType) {
  InSequence s;

  // Add foo listener.
  const std::string listener_foo_yaml = R"EOF(
    name: "foo"
    address:
      socket_address: { address: 127.0.0.1, port_value: 10000 }
    filter_chains:
    - filters:
    drain_type: MODIFY_ONLY
  )EOF";

  ListenerHandle* listener_foo =
      expectListenerCreate(false, envoy::api::v2::Listener_DrainType_MODIFY_ONLY);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "", true));
  checkStats(1, 0, 0, 0, 1, 0);

  EXPECT_CALL(*listener_foo, onDestroy());
}

TEST_F(ListenerManagerImplTest, AddListenerAddressNotMatching) {
  InSequence s;

  // Add foo listener.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "drain_type": "default"
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(1, 0, 0, 0, 1, 0);

  // Update foo listener, but with a different address. Should throw.
  const std::string listener_foo_different_address_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1235",
    "filters": [],
    "drain_type": "modify_only"
  }
  )EOF";

  ListenerHandle* listener_foo_different_address =
      expectListenerCreate(false, envoy::api::v2::Listener_DrainType_MODIFY_ONLY);
  EXPECT_CALL(*listener_foo_different_address, onDestroy());
  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_different_address_json), "",
                                    true),
      EnvoyException,
      "error updating listener: 'foo' has a different address "
      "'127.0.0.1:1235' from existing listener");

  EXPECT_CALL(*listener_foo, onDestroy());
}

// Make sure that a listener creation does not fail on IPv4 only setups when FilterChainMatch is not
// specified and we try to create default CidrRange. See convertDestinationIPsMapToTrie function for
// more details.
TEST_F(ListenerManagerImplTest, AddListenerOnIpv4OnlySetups) {
  InSequence s;

  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "drain_type": "default"
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);

  EXPECT_CALL(os_sys_calls, socket(AF_INET, _, 0)).WillOnce(Return(Api::SysCallIntResult{5, 0}));
  EXPECT_CALL(os_sys_calls, socket(AF_INET6, _, 0)).WillOnce(Return(Api::SysCallIntResult{-1, 0}));

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));

  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(1, 0, 0, 0, 1, 0);
  EXPECT_CALL(*listener_foo, onDestroy());
}

// Make sure that a listener creation does not fail on IPv6 only setups when FilterChainMatch is not
// specified and we try to create default CidrRange. See convertDestinationIPsMapToTrie function for
// more details.
TEST_F(ListenerManagerImplTest, AddListenerOnIpv6OnlySetups) {
  InSequence s;

  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://[::0001]:1234",
    "filters": [],
    "drain_type": "default"
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);

  EXPECT_CALL(os_sys_calls, socket(AF_INET, _, 0)).WillOnce(Return(Api::SysCallIntResult{-1, 0}));
  EXPECT_CALL(os_sys_calls, socket(AF_INET6, _, 0)).WillOnce(Return(Api::SysCallIntResult{5, 0}));

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));

  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(1, 0, 0, 0, 1, 0);
  EXPECT_CALL(*listener_foo, onDestroy());
}

// Make sure that a listener that is not modifiable cannot be updated or removed.
TEST_F(ListenerManagerImplTest, UpdateRemoveNotModifiableListener) {
  time_system_.setSystemTime(std::chrono::milliseconds(1001001001001));

  InSequence s;

  // Add foo listener.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", false));
  checkStats(1, 0, 0, 0, 1, 0);
  checkConfigDump(R"EOF(
static_listeners:
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
  last_updated:
    seconds: 1001001001
    nanos: 1000000
dynamic_active_listeners:
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Update foo listener. Should be blocked.
  const std::string listener_foo_update1_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      { "type" : "read", "name" : "fake", "config" : {} }
    ]
  }
  )EOF";

  EXPECT_FALSE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_update1_json), "", false));
  checkStats(1, 0, 0, 0, 1, 0);

  // Remove foo listener. Should be blocked.
  EXPECT_FALSE(manager_->removeListener("foo"));
  checkStats(1, 0, 0, 0, 1, 0);

  EXPECT_CALL(*listener_foo, onDestroy());
}

TEST_F(ListenerManagerImplTest, AddOrUpdateListener) {
  time_system_.setSystemTime(std::chrono::milliseconds(1001001001001));

  InSequence s;

  MockLdsApi* lds_api = new MockLdsApi();
  EXPECT_CALL(listener_factory_, createLdsApi_(_)).WillOnce(Return(lds_api));
  envoy::api::v2::core::ConfigSource lds_config;
  manager_->createLdsApi(lds_config);

  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return(""));
  checkConfigDump(R"EOF(
static_listeners:
dynamic_active_listeners:
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Add foo listener.
  const std::string listener_foo_yaml = R"EOF(
name: "foo"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1234
filter_chains: {}
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "version1", true));
  checkStats(1, 0, 0, 0, 1, 0);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version1"));
  checkConfigDump(R"EOF(
version_info: version1
static_listeners:
dynamic_active_listeners:
  version_info: "version1"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
  last_updated:
    seconds: 1001001001
    nanos: 1000000
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Update duplicate should be a NOP.
  EXPECT_FALSE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "", true));
  checkStats(1, 0, 0, 0, 1, 0);

  // Update foo listener. Should share socket.
  const std::string listener_foo_update1_yaml = R"EOF(
name: "foo"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1234
filter_chains: {}
per_connection_buffer_limit_bytes: 10
  )EOF";

  time_system_.setSystemTime(std::chrono::milliseconds(2002002002002));

  ListenerHandle* listener_foo_update1 = expectListenerCreate(false);
  EXPECT_CALL(*listener_foo, onDestroy());
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_update1_yaml),
                                            "version2", true));
  checkStats(1, 1, 0, 0, 1, 0);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version2"));
  checkConfigDump(R"EOF(
version_info: version2
static_listeners:
dynamic_active_listeners:
  version_info: "version2"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
    per_connection_buffer_limit_bytes: 10
  last_updated:
    seconds: 2002002002
    nanos: 2000000
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Start workers.
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);
  worker_->callAddCompletion(true);

  // Update duplicate should be a NOP.
  EXPECT_FALSE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_update1_yaml), "", true));
  checkStats(1, 1, 0, 0, 1, 0);

  time_system_.setSystemTime(std::chrono::milliseconds(3003003003003));

  // Update foo. Should go into warming, have an immediate warming callback, and start immediate
  // removal.
  ListenerHandle* listener_foo_update2 = expectListenerCreate(false);
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo_update1->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "version3", true));
  worker_->callAddCompletion(true);
  checkStats(1, 2, 0, 0, 1, 1);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version3"));
  checkConfigDump(R"EOF(
version_info: version3
static_listeners:
dynamic_active_listeners:
  version_info: "version3"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
  last_updated:
    seconds: 3003003003
    nanos: 3000000
dynamic_warming_listeners:
dynamic_draining_listeners:
  version_info: "version2"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
    per_connection_buffer_limit_bytes: 10
  last_updated:
    seconds: 2002002002
    nanos: 2000000
)EOF");

  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo_update1->drain_manager_->drain_sequence_completion_();
  checkStats(1, 2, 0, 0, 1, 1);
  EXPECT_CALL(*listener_foo_update1, onDestroy());
  worker_->callRemovalCompletion();
  checkStats(1, 2, 0, 0, 1, 0);

  time_system_.setSystemTime(std::chrono::milliseconds(4004004004004));

  // Add bar listener.
  const std::string listener_bar_yaml = R"EOF(
name: "bar"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1235
filter_chains: {}
  )EOF";

  ListenerHandle* listener_bar = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_bar_yaml), "version4", true));
  EXPECT_EQ(2UL, manager_->listeners().size());
  worker_->callAddCompletion(true);
  checkStats(2, 2, 0, 0, 2, 0);

  time_system_.setSystemTime(std::chrono::milliseconds(5005005005005));

  // Add baz listener, this time requiring initializing.
  const std::string listener_baz_yaml = R"EOF(
name: "baz"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1236
filter_chains: {}
  )EOF";

  ListenerHandle* listener_baz = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(listener_baz->target_, initialize(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_baz_yaml), "version5", true));
  EXPECT_EQ(2UL, manager_->listeners().size());
  checkStats(3, 2, 0, 1, 2, 0);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version5"));
  checkConfigDump(R"EOF(
version_info: version5
static_listeners:
dynamic_active_listeners:
  - version_info: "version3"
    listener:
      name: "foo"
      address:
        socket_address:
          address: "127.0.0.1"
          port_value: 1234
      filter_chains: {}
    last_updated:
      seconds: 3003003003
      nanos: 3000000
  - version_info: "version4"
    listener:
      name: "bar"
      address:
        socket_address:
          address: "127.0.0.1"
          port_value: 1235
      filter_chains: {}
    last_updated:
      seconds: 4004004004
      nanos: 4000000
dynamic_warming_listeners:
  - version_info: "version5"
    listener:
      name: "baz"
      address:
        socket_address:
          address: "127.0.0.1"
          port_value: 1236
      filter_chains: {}
    last_updated:
      seconds: 5005005005
      nanos: 5000000
dynamic_draining_listeners:
)EOF");

  // Update a duplicate baz that is currently warming.
  EXPECT_FALSE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_baz_yaml), "", true));
  checkStats(3, 2, 0, 1, 2, 0);

  // Update baz while it is warming.
  const std::string listener_baz_update1_json = R"EOF(
  {
    "name": "baz",
    "address": "tcp://127.0.0.1:1236",
    "filters": [
      { "type" : "read", "name" : "fake", "config" : {} }
    ]
  }
  )EOF";

  ListenerHandle* listener_baz_update1 = expectListenerCreate(true);
  EXPECT_CALL(*listener_baz, onDestroy()).WillOnce(Invoke([listener_baz]() -> void {
    // Call the initialize callback during destruction like RDS will.
    listener_baz->target_.callback_();
  }));
  EXPECT_CALL(listener_baz_update1->target_, initialize(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_baz_update1_json), "", true));
  EXPECT_EQ(2UL, manager_->listeners().size());
  checkStats(3, 3, 0, 1, 2, 0);

  // Finish initialization for baz which should make it active.
  EXPECT_CALL(*worker_, addListener(_, _));
  listener_baz_update1->target_.callback_();
  EXPECT_EQ(3UL, manager_->listeners().size());
  worker_->callAddCompletion(true);
  checkStats(3, 3, 0, 0, 3, 0);

  EXPECT_CALL(*listener_foo_update2, onDestroy());
  EXPECT_CALL(*listener_bar, onDestroy());
  EXPECT_CALL(*listener_baz_update1, onDestroy());
}

TEST_F(ListenerManagerImplTest, AddDrainingListener) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Add foo listener directly into active.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  Network::Address::InstanceConstSharedPtr local_address(
      new Network::Address::Ipv4Instance("127.0.0.1", 1234));
  ON_CALL(*listener_factory_.socket_, localAddress()).WillByDefault(ReturnRef(local_address));

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  worker_->callAddCompletion(true);
  checkStats(1, 0, 0, 0, 1, 0);

  // Remove foo into draining.
  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(manager_->removeListener("foo"));
  checkStats(1, 0, 1, 0, 0, 1);
  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();
  checkStats(1, 0, 1, 0, 0, 1);

  // Add foo again. We should use the socket from draining.
  ListenerHandle* listener_foo2 = expectListenerCreate(false);
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  worker_->callAddCompletion(true);
  checkStats(2, 0, 1, 0, 1, 1);

  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();
  checkStats(2, 0, 1, 0, 1, 0);

  EXPECT_CALL(*listener_foo2, onDestroy());
}

TEST_F(ListenerManagerImplTest, CantBindSocket) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true))
      .WillOnce(Throw(EnvoyException("can't bind")));
  EXPECT_CALL(*listener_foo, onDestroy());
  EXPECT_THROW(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true),
               EnvoyException);
}

TEST_F(ListenerManagerImplTest, ListenerDraining) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  worker_->callAddCompletion(true);
  checkStats(1, 0, 0, 0, 1, 0);

  EXPECT_CALL(*listener_foo->drain_manager_, drainClose()).WillOnce(Return(false));
  EXPECT_CALL(server_.drain_manager_, drainClose()).WillOnce(Return(false));
  EXPECT_FALSE(listener_foo->context_->drainDecision().drainClose());

  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(manager_->removeListener("foo"));
  checkStats(1, 0, 1, 0, 0, 1);

  // NOTE: || short circuit here prevents the server drain manager from getting called.
  EXPECT_CALL(*listener_foo->drain_manager_, drainClose()).WillOnce(Return(true));
  EXPECT_TRUE(listener_foo->context_->drainDecision().drainClose());

  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();
  checkStats(1, 0, 1, 0, 0, 1);

  EXPECT_CALL(*listener_foo->drain_manager_, drainClose()).WillOnce(Return(false));
  EXPECT_CALL(server_.drain_manager_, drainClose()).WillOnce(Return(true));
  EXPECT_TRUE(listener_foo->context_->drainDecision().drainClose());

  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(1, 0, 1, 0, 0, 0);
}

TEST_F(ListenerManagerImplTest, RemoveListener) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Remove an unknown listener.
  EXPECT_FALSE(manager_->removeListener("unknown"));

  // Add foo listener into warming.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(listener_foo->target_, initialize(_));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(1, 0, 0, 1, 0, 0);

  // Remove foo.
  EXPECT_CALL(*listener_foo, onDestroy());
  EXPECT_TRUE(manager_->removeListener("foo"));
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(1, 0, 1, 0, 0, 0);

  // Add foo again and initialize it.
  listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(listener_foo->target_, initialize(_));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(2, 0, 1, 1, 0, 0);
  EXPECT_CALL(*worker_, addListener(_, _));
  listener_foo->target_.callback_();
  worker_->callAddCompletion(true);
  EXPECT_EQ(1UL, manager_->listeners().size());
  checkStats(2, 0, 1, 0, 1, 0);

  // Update foo into warming.
  const std::string listener_foo_update1_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      { "type" : "read", "name" : "fake", "config" : {} }
    ]
  }
  )EOF";

  ListenerHandle* listener_foo_update1 = expectListenerCreate(true);
  EXPECT_CALL(listener_foo_update1->target_, initialize(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_update1_json), "", true));
  EXPECT_EQ(1UL, manager_->listeners().size());
  checkStats(2, 1, 1, 1, 1, 0);

  // Remove foo which should remove both warming and active.
  EXPECT_CALL(*listener_foo_update1, onDestroy());
  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(manager_->removeListener("foo"));
  checkStats(2, 1, 2, 0, 0, 1);
  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();
  checkStats(2, 1, 2, 0, 0, 1);
  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(2, 1, 2, 0, 0, 0);
}

TEST_F(ListenerManagerImplTest, AddListenerFailure) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Add foo listener into active.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://0.0.0.0:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));

  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  worker_->callAddCompletion(false);

  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();

  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();

  EXPECT_EQ(1UL, server_.stats_store_.counter("listener_manager.listener_create_failure").value());
}

TEST_F(ListenerManagerImplTest, StatsNameValidCharacterTest) {
  const std::string json = R"EOF(
  {
    "address": "tcp://[::1]:10000",
    "filters": [],
    "bind_to_port": false
  }
  )EOF";

  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  manager_->listeners().front().get().listenerScope().counter("foo").inc();

  EXPECT_EQ(1UL, server_.stats_store_.counter("listener.[__1]_10000.foo").value());
}

TEST_F(ListenerManagerImplTest, DuplicateAddressDontBind) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Add foo listener into warming.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://0.0.0.0:1234",
    "filters": [],
    "bind_to_port": false
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, false));
  EXPECT_CALL(listener_foo->target_, initialize(_));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));

  // Add bar with same non-binding address. Should fail.
  const std::string listener_bar_json = R"EOF(
  {
    "name": "bar",
    "address": "tcp://0.0.0.0:1234",
    "filters": [],
    "bind_to_port": false
  }
  )EOF";

  ListenerHandle* listener_bar = expectListenerCreate(true);
  EXPECT_CALL(*listener_bar, onDestroy());
  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_bar_json), "", true),
      EnvoyException,
      "error adding listener: 'bar' has duplicate address '0.0.0.0:1234' as existing listener");

  // Move foo to active and then try to add again. This should still fail.
  EXPECT_CALL(*worker_, addListener(_, _));
  listener_foo->target_.callback_();
  worker_->callAddCompletion(true);

  listener_bar = expectListenerCreate(true);
  EXPECT_CALL(*listener_bar, onDestroy());
  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_bar_json), "", true),
      EnvoyException,
      "error adding listener: 'bar' has duplicate address '0.0.0.0:1234' as existing listener");

  EXPECT_CALL(*listener_foo, onDestroy());
}

TEST_F(ListenerManagerImplTest, EarlyShutdown) {
  // If stopWorkers is called before the workers are started, it should be a no-op: they should be
  // neither started nor stopped.
  EXPECT_CALL(*worker_, start(_)).Times(0);
  EXPECT_CALL(*worker_, stop()).Times(0);
  manager_->stopWorkers();
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithDestinationPortMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        destination_port: 8080
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to unknown port - no match.
  auto filter_chain = findFilterChain(1234, false, "127.0.0.1", false, "", false, "tls", false, {});
  EXPECT_EQ(filter_chain, nullptr);

  // IPv4 client connects to valid port - using 1st filter chain.
  filter_chain = findFilterChain(8080, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // UDS client - no match.
  filter_chain = findFilterChain(0, false, "/tmp/test.sock", false, "", false, "tls", false, {});
  EXPECT_EQ(filter_chain, nullptr);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithDestinationIPMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        prefix_ranges: { address_prefix: 127.0.0.0, prefix_len: 8 }
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to unknown IP - no match.
  auto filter_chain = findFilterChain(1234, true, "1.2.3.4", false, "", false, "tls", false, {});
  EXPECT_EQ(filter_chain, nullptr);

  // IPv4 client connects to valid IP - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // UDS client - no match.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", false, "", false, "tls", false, {});
  EXPECT_EQ(filter_chain, nullptr);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithServerNamesMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "server1.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without SNI - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", false, "tls", false, {});
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client without matching SNI - no match.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "www.example.com", false, "tls", false, {});
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client with matching SNI - using 1st filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithTransportProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        transport_protocol: "tls"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TCP client - no match.
  auto filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "", true, "raw_buffer", false, {});
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithApplicationProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        application_protocols: "http/1.1"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without ALPN - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client with "http/1.1" ALPN - using 1st filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {"h2", "http/1.1"});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithDestinationPortMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_uri_key.pem" }
    - filter_chain_match:
        destination_port: 8080
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
    - filter_chain_match:
        destination_port: 8081
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_multiple_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to default port - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // IPv4 client connects to port 8080 - using 2nd filter chain.
  filter_chain = findFilterChain(8080, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // IPv4 client connects to port 8081 - using 3nd filter chain.
  filter_chain = findFilterChain(8081, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");

  // UDS client - using 1st filter chain.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithDestinationIPMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_uri_key.pem" }
    - filter_chain_match:
        prefix_ranges: { address_prefix: 192.168.0.1, prefix_len: 32 }
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
    - filter_chain_match:
        prefix_ranges: { address_prefix: 192.168.0.0, prefix_len: 16 }
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_multiple_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to default IP - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // IPv4 client connects to exact IP match - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "192.168.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // IPv4 client connects to wildcard IP match - using 3nd filter chain.
  filter_chain = findFilterChain(1234, true, "192.168.1.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");

  // UDS client - using 1st filter chain.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithServerNamesMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_uri_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/common/ssl/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "server1.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/common/ssl/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "*.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_multiple_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/common/ssl/test_data/ticket_key_a"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without SNI - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // TLS client with exact SNI match - using 2nd filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // TLS client with wildcard SNI match - using 3nd filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "server2.example.com", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");

  // TLS client with wildcard SNI match - using 3nd filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "www.wildcard.com", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithTransportProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
    - filter_chain_match:
        transport_protocol: "tls"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TCP client - using 1st filter chain.
  auto filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "", true, "raw_buffer", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithApplicationProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
    - filter_chain_match:
        application_protocols: ["dummy", "h2"]
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without ALPN - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client with "h2,http/1.1" ALPN - using 2nd filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {"h2", "http/1.1"});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithMultipleRequirementsMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
    - filter_chain_match:
        server_names: ["www.example.com", "server1.example.com"]
        transport_protocol: "tls"
        application_protocols: ["dummy", "h2"]
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without SNI and ALPN - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client with exact SNI match but without ALPN - no match (SNI blackholed by configuration).
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls", true, {});
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client with ALPN match but without SNI - using 1st filter chain.
  filter_chain =
      findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {"h2", "http/1.1"});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client with exact SNI match and ALPN match - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls",
                                 true, {"h2", "http/1.1"});
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket();
  auto ssl_socket = dynamic_cast<Ssl::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithDifferentSessionTicketKeys) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/common/ssl/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "www.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/common/ssl/test_data/ticket_key_b"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest,
       MultipleFilterChainsWithMixedUseOfSessionTicketKeys) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/common/ssl/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "www.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithInvalidDestinationIPMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        prefix_ranges: { address_prefix: a.b.c.d, prefix_len: 32 }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "malformed IP address: a.b.c.d");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithInvalidServerNamesMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "*w.example.com"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "error adding listener '127.0.0.1:1234': partial wildcards are not "
                            "supported in \"server_names\"");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithSameMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        transport_protocol: "tls"
    - filter_chain_match:
        transport_protocol: "tls"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "error adding listener '127.0.0.1:1234': multiple filter chains with "
                            "the same matching rules are defined");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsFilterChainWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        transport_protocol: "tls"
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS Inspector is automatically injected for filter chains with TLS requirements,
  // so make sure there is exactly 1 listener filter (and assume it's TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr&) -> void {}));
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SniFilterChainWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS Inspector is automatically injected for filter chains with SNI requirements,
  // so make sure there is exactly 1 listener filter (and assume it's TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr&) -> void {}));
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, AlpnFilterChainWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        application_protocols: ["h2", "http/1.1"]
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS Inspector is automatically injected for filter chains with ALPN requirements,
  // so make sure there is exactly 1 listener filter (and assume it's TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr&) -> void {}));
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CustomTransportProtocolWithSniWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
        transport_protocol: "custom"
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // Make sure there are no listener filters (i.e. no automatically injected TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_)).Times(0);
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInline) {
  const std::string yaml = R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: "-----BEGIN CERTIFICATE-----\nMIIDGjCCAoOgAwIBAgIJALE/9j8tvBGNMA0GCSqGSIb3DQEBCwUAMIGDMQswCQYD\nVQQGEwJVUzETMBEGA1UECAwKQ2FsaWZvcm5pYTEWMBQGA1UEBwwNU2FuIEZyYW5j\naXNjbzENMAsGA1UECgwETHlmdDEZMBcGA1UECwwQTHlmdCBFbmdpbmVlcmluZzEd\nMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwHhcNMTgwMTE1MjI0MDI3WhcN\nMjAwMTE1MjI0MDI3WjB6MQswCQYDVQQGEwJVUzETMBEGA1UECAwKQ2FsaWZvcm5p\nYTEWMBQGA1UEBwwNU2FuIEZyYW5jaXNjbzENMAsGA1UECgwETHlmdDEZMBcGA1UE\nCwwQTHlmdCBFbmdpbmVlcmluZzEUMBIGA1UEAwwLVGVzdCBTZXJ2ZXIwgZ8wDQYJ\nKoZIhvcNAQEBBQADgY0AMIGJAoGBALGG70n/nfIB64LH6jraqxpJ3EUO+gL/KkHG\n4+/hQMMZpehPdcHa7vj1efBgaaddtjRZ3GLSSF968O19EbMwjQl1Azwn3Ql8SddQ\nhyW30/Q/jgY54MnDBGgb5xhb7tdfjGvZ+lKapu9FypTcrre/wXSwBSsmm2me0CCN\nAZKddyMzAgMBAAGjgZ0wgZowDAYDVR0TAQH/BAIwADALBgNVHQ8EBAMCBeAwHQYD\nVR0lBBYwFAYIKwYBBQUHAwIGCCsGAQUFBwMBMB4GA1UdEQQXMBWCE3NlcnZlcjEu\nZXhhbXBsZS5jb20wHQYDVR0OBBYEFBZcRXhGXlFghYQ9/R6yF7XTHzkxMB8GA1Ud\nIwQYMBaAFIuDOLnI5iZPpH230VFAuWeNYuWdMA0GCSqGSIb3DQEBCwUAA4GBAFZg\n6ZznS3KuEus6ZJsLJH7J0BMKmpdj5hM0M++TBnP8LVy73ETc95Y2sxvB/B2c7f2v\nwjz4nGd5O3kkiVlMrQ44GPUKrO3/Ltix+MT3ixuF7Z7vLFKwkIG2d0RXJVVb1MOM\nw1bmtABHf8sVAFF6RZtjshWSaZYvIhiG1FedV4Vw\n-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\nMIIC3jCCAkegAwIBAgIJAJvUixVO/5pTMA0GCSqGSIb3DQEBCwUAMHYxCzAJBgNV\nBAYTAlVTMRMwEQYDVQQIEwpDYWxpZm9ybmlhMRYwFAYDVQQHEw1TYW4gRnJhbmNp\nc2NvMQ0wCwYDVQQKEwRMeWZ0MRkwFwYDVQQLExBMeWZ0IEVuZ2luZWVyaW5nMRAw\nDgYDVQQDEwdUZXN0IENBMB4XDTE4MDExNTIyNDAyN1oXDTI4MDExMzIyNDAyN1ow\ngYMxCzAJBgNVBAYTAlVTMRMwEQYDVQQIDApDYWxpZm9ybmlhMRYwFAYDVQQHDA1T\nYW4gRnJhbmNpc2NvMQ0wCwYDVQQKDARMeWZ0MRkwFwYDVQQLDBBMeWZ0IEVuZ2lu\nZWVyaW5nMR0wGwYDVQQDDBRUZXN0IEludGVybWVkaWF0ZSBDQTCBnzANBgkqhkiG\n9w0BAQEFAAOBjQAwgYkCgYEAxd1kOhV+/2n+rJWKvrtkyyqkWgBXXhH15G9cusaR\nzJtwxsvtPRYZ9nTc+A6GDFgZ0TS1sq/WJXfs3guMpFObXU+tSlezxHVRpWPTXKff\nhblqtZMPKW5q5LmOHxKi8GUxwDnEeAiZmzstGCYkRKn+GmLYe26vFGBw4MvM89Vm\necMCAwEAAaNmMGQwEgYDVR0TAQH/BAgwBgEB/wIBADAOBgNVHQ8BAf8EBAMCAQYw\nHQYDVR0OBBYEFIuDOLnI5iZPpH230VFAuWeNYuWdMB8GA1UdIwQYMBaAFDt4pFFP\nFoSTHEgoegytK5ZByn15MA0GCSqGSIb3DQEBCwUAA4GBAGO77sBFzn63pM2Oy4XS\n+FEcFj/lB4vBh4r8jtzdf5EMaKUeXY9i57MTryPTJZRFXW6BuQ/B3hiOW2fwkCdL\neHd0MwGv95cn1PCWZh1IidVrtrDeu0oLxhRk5mcflaaLBuGADUD3Y1ms2sl90Ean\n6C+0EHH2O6Emesx9IhPZRx4H\n-----END CERTIFICATE-----" }
              private_key: { inline_string: "-----BEGIN RSA PRIVATE KEY-----\nMIICXgIBAAKBgQCxhu9J/53yAeuCx+o62qsaSdxFDvoC/ypBxuPv4UDDGaXoT3XB\n2u749XnwYGmnXbY0Wdxi0khfevDtfRGzMI0JdQM8J90JfEnXUIclt9P0P44GOeDJ\nwwRoG+cYW+7XX4xr2fpSmqbvRcqU3K63v8F0sAUrJptpntAgjQGSnXcjMwIDAQAB\nAoGAJk4kQcZLEVYCuDRkwRA/zStUwP3rSkw+lPTSaAcljzNwjgDfOtX/rG5jQk+7\nXGanEwK0wAn5nciMReIvuIdoVt7zEtFygZ7D3yKh+Ywu0AYC8TaiszAaL0efMnAW\nMtpgyrAoMB4bFON3oyNVZM161+cdrU0PrEDqh+GHl8abpokCQQDfn2jE3WmS/a36\n2Dt/e3yjNE/4xDshAcHkNmF+jpcB66bls5ZU5Yv07RUVExQ30fw3v+ADMxmNYg2f\nqpAF2getAkEAyzr9GwgdKeruErwlLhMOG327zTcZlHZXU5Q378/S3bKi3c/voGCb\nexKjxRBZTzSw5hfrOzbCEpMLcwZaoBiyXwJBAJAH2XAq98vQDpXpXfEPNUjc8cFV\niowI2LxHdmYQKxz2jemW0PXfX1Siuxh20GffnOa/c+Y7rHKOvB2hut+5/YUCQQCF\nnBx2vxjdTBSEwKj455IoxLrJKeZpUnwK+LDluo35Ls4gYeo6WAkgGpsMnbj5d7yt\nKSB/Z3qj14R5dL3z7wilAkEAp78JPrvWP5zc3YOmmr9s8IQ3almspwnlh1mdcgoX\n4rHe3U4jRZwE6fw8W9G4QsFRlez1Re6g8T7yoFXiZETGqQ==\n-----END RSA PRIVATE KEY-----" }
          validation_context:
              trusted_ca: { inline_string: "-----BEGIN CERTIFICATE-----\nMIICzTCCAjagAwIBAgIJAOrzsOodDleaMA0GCSqGSIb3DQEBCwUAMHYxCzAJBgNV\nBAYTAlVTMRMwEQYDVQQIEwpDYWxpZm9ybmlhMRYwFAYDVQQHEw1TYW4gRnJhbmNp\nc2NvMQ0wCwYDVQQKEwRMeWZ0MRkwFwYDVQQLExBMeWZ0IEVuZ2luZWVyaW5nMRAw\nDgYDVQQDEwdUZXN0IENBMB4XDTE3MDcwOTAxMzkzMloXDTI3MDcwNzAxMzkzMlow\ndjELMAkGA1UEBhMCVVMxEzARBgNVBAgTCkNhbGlmb3JuaWExFjAUBgNVBAcTDVNh\nbiBGcmFuY2lzY28xDTALBgNVBAoTBEx5ZnQxGTAXBgNVBAsTEEx5ZnQgRW5naW5l\nZXJpbmcxEDAOBgNVBAMTB1Rlc3QgQ0EwgZ8wDQYJKoZIhvcNAQEBBQADgY0AMIGJ\nAoGBAJuJh8N5TheTHLKOxsLSAfiIu9VDeKPsV98KRJJaYCMoaof3j9wBs65HzIat\nAunuV4DVZZ2c/x7/v741oWadYd3yqL7XSzQaeBvhXi+wv3g17FYrdxaowG7cfmsh\ngCp7/9TRW0bRGL6Qp6od/u62L8dprdHXxnck/+sZMupam9YrAgMBAAGjYzBhMA8G\nA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMB0GA1UdDgQWBBQ7eKRRTxaE\nkxxIKHoMrSuWQcp9eTAfBgNVHSMEGDAWgBQ7eKRRTxaEkxxIKHoMrSuWQcp9eTAN\nBgkqhkiG9w0BAQsFAAOBgQCN00/2k9k8HNeJ8eYuFH10jnc+td7+OaYWpRSEKCS7\nux3KAu0UFt90mojEMClt4Y6uP4oXTWbRzMzAgQHldHU8Gkj8tYnv7mToX7Bh/xdc\n19epzjCmo/4Q6+16GZZvltiFjkkHSZEVI5ggljy1QdMIPRegsKKmX9mjZSCSSXD6\nSA==\n-----END CERTIFICATE-----" }
  )EOF";

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateChainInlinePrivateKeyFilename) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: "-----BEGIN CERTIFICATE-----\nMIIDGjCCAoOgAwIBAgIJALE/9j8tvBGNMA0GCSqGSIb3DQEBCwUAMIGDMQswCQYD\nVQQGEwJVUzETMBEGA1UECAwKQ2FsaWZvcm5pYTEWMBQGA1UEBwwNU2FuIEZyYW5j\naXNjbzENMAsGA1UECgwETHlmdDEZMBcGA1UECwwQTHlmdCBFbmdpbmVlcmluZzEd\nMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwHhcNMTgwMTE1MjI0MDI3WhcN\nMjAwMTE1MjI0MDI3WjB6MQswCQYDVQQGEwJVUzETMBEGA1UECAwKQ2FsaWZvcm5p\nYTEWMBQGA1UEBwwNU2FuIEZyYW5jaXNjbzENMAsGA1UECgwETHlmdDEZMBcGA1UE\nCwwQTHlmdCBFbmdpbmVlcmluZzEUMBIGA1UEAwwLVGVzdCBTZXJ2ZXIwgZ8wDQYJ\nKoZIhvcNAQEBBQADgY0AMIGJAoGBALGG70n/nfIB64LH6jraqxpJ3EUO+gL/KkHG\n4+/hQMMZpehPdcHa7vj1efBgaaddtjRZ3GLSSF968O19EbMwjQl1Azwn3Ql8SddQ\nhyW30/Q/jgY54MnDBGgb5xhb7tdfjGvZ+lKapu9FypTcrre/wXSwBSsmm2me0CCN\nAZKddyMzAgMBAAGjgZ0wgZowDAYDVR0TAQH/BAIwADALBgNVHQ8EBAMCBeAwHQYD\nVR0lBBYwFAYIKwYBBQUHAwIGCCsGAQUFBwMBMB4GA1UdEQQXMBWCE3NlcnZlcjEu\nZXhhbXBsZS5jb20wHQYDVR0OBBYEFBZcRXhGXlFghYQ9/R6yF7XTHzkxMB8GA1Ud\nIwQYMBaAFIuDOLnI5iZPpH230VFAuWeNYuWdMA0GCSqGSIb3DQEBCwUAA4GBAFZg\n6ZznS3KuEus6ZJsLJH7J0BMKmpdj5hM0M++TBnP8LVy73ETc95Y2sxvB/B2c7f2v\nwjz4nGd5O3kkiVlMrQ44GPUKrO3/Ltix+MT3ixuF7Z7vLFKwkIG2d0RXJVVb1MOM\nw1bmtABHf8sVAFF6RZtjshWSaZYvIhiG1FedV4Vw\n-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\nMIIC3jCCAkegAwIBAgIJAJvUixVO/5pTMA0GCSqGSIb3DQEBCwUAMHYxCzAJBgNV\nBAYTAlVTMRMwEQYDVQQIEwpDYWxpZm9ybmlhMRYwFAYDVQQHEw1TYW4gRnJhbmNp\nc2NvMQ0wCwYDVQQKEwRMeWZ0MRkwFwYDVQQLExBMeWZ0IEVuZ2luZWVyaW5nMRAw\nDgYDVQQDEwdUZXN0IENBMB4XDTE4MDExNTIyNDAyN1oXDTI4MDExMzIyNDAyN1ow\ngYMxCzAJBgNVBAYTAlVTMRMwEQYDVQQIDApDYWxpZm9ybmlhMRYwFAYDVQQHDA1T\nYW4gRnJhbmNpc2NvMQ0wCwYDVQQKDARMeWZ0MRkwFwYDVQQLDBBMeWZ0IEVuZ2lu\nZWVyaW5nMR0wGwYDVQQDDBRUZXN0IEludGVybWVkaWF0ZSBDQTCBnzANBgkqhkiG\n9w0BAQEFAAOBjQAwgYkCgYEAxd1kOhV+/2n+rJWKvrtkyyqkWgBXXhH15G9cusaR\nzJtwxsvtPRYZ9nTc+A6GDFgZ0TS1sq/WJXfs3guMpFObXU+tSlezxHVRpWPTXKff\nhblqtZMPKW5q5LmOHxKi8GUxwDnEeAiZmzstGCYkRKn+GmLYe26vFGBw4MvM89Vm\necMCAwEAAaNmMGQwEgYDVR0TAQH/BAgwBgEB/wIBADAOBgNVHQ8BAf8EBAMCAQYw\nHQYDVR0OBBYEFIuDOLnI5iZPpH230VFAuWeNYuWdMB8GA1UdIwQYMBaAFDt4pFFP\nFoSTHEgoegytK5ZByn15MA0GCSqGSIb3DQEBCwUAA4GBAGO77sBFzn63pM2Oy4XS\n+FEcFj/lB4vBh4r8jtzdf5EMaKUeXY9i57MTryPTJZRFXW6BuQ/B3hiOW2fwkCdL\neHd0MwGv95cn1PCWZh1IidVrtrDeu0oLxhRk5mcflaaLBuGADUD3Y1ms2sl90Ean\n6C+0EHH2O6Emesx9IhPZRx4H\n-----END CERTIFICATE-----" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key3.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateIncomplete) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_chain3.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true), EnvoyException,
      TestEnvironment::substitute("Failed to load incomplete certificate from {{ test_rundir }}"
                                  "/test/common/ssl/test_data/san_dns_chain3.pem, ",
                                  Network::Address::IpVersion::v4));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidCertificateChain) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: "invalid" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key3.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load certificate chain from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidIntermediateCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: "-----BEGIN CERTIFICATE-----\nMIIDGjCCAoOgAwIBAgIJALE/9j8tvBGNMA0GCSqGSIb3DQEBCwUAMIGDMQswCQYD\nVQQGEwJVUzETMBEGA1UECAwKQ2FsaWZvcm5pYTEWMBQGA1UEBwwNU2FuIEZyYW5j\naXNjbzENMAsGA1UECgwETHlmdDEZMBcGA1UECwwQTHlmdCBFbmdpbmVlcmluZzEd\nMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwHhcNMTgwMTE1MjI0MDI3WhcN\nMjAwMTE1MjI0MDI3WjB6MQswCQYDVQQGEwJVUzETMBEGA1UECAwKQ2FsaWZvcm5p\nYTEWMBQGA1UEBwwNU2FuIEZyYW5jaXNjbzENMAsGA1UECgwETHlmdDEZMBcGA1UE\nCwwQTHlmdCBFbmdpbmVlcmluZzEUMBIGA1UEAwwLVGVzdCBTZXJ2ZXIwgZ8wDQYJ\nKoZIhvcNAQEBBQADgY0AMIGJAoGBALGG70n/nfIB64LH6jraqxpJ3EUO+gL/KkHG\n4+/hQMMZpehPdcHa7vj1efBgaaddtjRZ3GLSSF968O19EbMwjQl1Azwn3Ql8SddQ\nhyW30/Q/jgY54MnDBGgb5xhb7tdfjGvZ+lKapu9FypTcrre/wXSwBSsmm2me0CCN\nAZKddyMzAgMBAAGjgZ0wgZowDAYDVR0TAQH/BAIwADALBgNVHQ8EBAMCBeAwHQYD\nVR0lBBYwFAYIKwYBBQUHAwIGCCsGAQUFBwMBMB4GA1UdEQQXMBWCE3NlcnZlcjEu\nZXhhbXBsZS5jb20wHQYDVR0OBBYEFBZcRXhGXlFghYQ9/R6yF7XTHzkxMB8GA1Ud\nIwQYMBaAFIuDOLnI5iZPpH230VFAuWeNYuWdMA0GCSqGSIb3DQEBCwUAA4GBAFZg\n6ZznS3KuEus6ZJsLJH7J0BMKmpdj5hM0M++TBnP8LVy73ETc95Y2sxvB/B2c7f2v\nwjz4nGd5O3kkiVlMrQ44GPUKrO3/Ltix+MT3ixuF7Z7vLFKwkIG2d0RXJVVb1MOM\nw1bmtABHf8sVAFF6RZtjshWSaZYvIhiG1FedV4Vw\n-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\ninvalid" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key3.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load certificate chain from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidPrivateKey) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_chain3.pem" }
              private_key: { inline_string: "invalid" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load private key from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidTrustedCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_chain3.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key3.pem" }
          validation_context:
              trusted_ca: { inline_string: "invalid" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load trusted CA certificates from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, Metadata) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    metadata: { filter_metadata: { com.bar.foo: { baz: test_value } } }
    filter_chains:
    - filter_chain_match:
      filters:
      - name: envoy.http_connection_manager
        config:
          stat_prefix: metadata_test
          route_config:
            virtual_hosts:
            - name: "some_virtual_host"
              domains: ["some.domain"]
              routes:
              - match: { prefix: "/" }
                route: { cluster: service_foo }
  )EOF",
                                                       Network::Address::IpVersion::v4);
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  auto context = dynamic_cast<Configuration::FactoryContext*>(&manager_->listeners().front().get());
  ASSERT_NE(nullptr, context);
  EXPECT_EQ("test_value",
            Config::Metadata::metadataValue(context->listenerMetadata(), "com.bar.foo", "baz")
                .string_value());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstFilter) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "envoy.listener.original_dst"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  Network::ListenerConfig& listener = manager_->listeners().back().get();

  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;

  NiceMock<Network::MockListenerFilterCallbacks> callbacks;
  Network::AcceptedSocketImpl socket(-1,
                                     Network::Address::InstanceConstSharedPtr{
                                         new Network::Address::Ipv4Instance("127.0.0.1", 1234)},
                                     Network::Address::InstanceConstSharedPtr{
                                         new Network::Address::Ipv4Instance("127.0.0.1", 5678)});

  EXPECT_CALL(callbacks, socket()).WillOnce(Invoke([&]() -> Network::ConnectionSocket& {
    return socket;
  }));

  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr& filter) -> void {
        EXPECT_EQ(Network::FilterStatus::Continue, filter->onAccept(callbacks));
      }));

  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

class OriginalDstTestFilter : public Extensions::ListenerFilters::OriginalDst::OriginalDstFilter {
  Network::Address::InstanceConstSharedPtr getOriginalDst(int) override {
    return Network::Address::InstanceConstSharedPtr{
        new Network::Address::Ipv4Instance("127.0.0.2", 2345)};
  }
};

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilter) {
  static int fd;
  fd = -1;
  EXPECT_CALL(*listener_factory_.socket_, fd()).WillOnce(Return(0));

  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(true));
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_BOUND))
          .WillOnce(Invoke(
              [](Network::Socket& socket, envoy::api::v2::core::SocketOption::SocketState) -> bool {
                fd = socket.fd();
                return true;
              }));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilter>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "test.listener.original_dst"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "test.listener.original_dst"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  Network::ListenerConfig& listener = manager_->listeners().back().get();

  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;

  NiceMock<Network::MockListenerFilterCallbacks> callbacks;
  Network::AcceptedSocketImpl socket(
      -1, std::make_unique<Network::Address::Ipv4Instance>("127.0.0.1", 1234),
      std::make_unique<Network::Address::Ipv4Instance>("127.0.0.1", 5678));

  EXPECT_CALL(callbacks, socket()).WillOnce(Invoke([&]() -> Network::ConnectionSocket& {
    return socket;
  }));

  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr& filter) -> void {
        EXPECT_EQ(Network::FilterStatus::Continue, filter->onAccept(callbacks));
      }));

  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
  EXPECT_TRUE(socket.localAddressRestored());
  EXPECT_EQ("127.0.0.2:2345", socket.localAddress()->asString());
  EXPECT_NE(fd, -1);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilterOptionFail) {
  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(false));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilter>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "testfail.listener.original_dst"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: "socketOptionFailListener"
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "testfail.listener.original_dst"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "MockListenerComponentFactory: Setting socket options failed");
  EXPECT_EQ(0U, manager_->listeners().size());
}

class OriginalDstTestFilterIPv6
    : public Extensions::ListenerFilters::OriginalDst::OriginalDstFilter {
  Network::Address::InstanceConstSharedPtr getOriginalDst(int) override {
    return Network::Address::InstanceConstSharedPtr{
        new Network::Address::Ipv6Instance("1::2", 2345)};
  }
};

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilterIPv6) {
  static int fd;
  fd = -1;
  EXPECT_CALL(*listener_factory_.socket_, fd()).WillOnce(Return(0));

  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(true));
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_BOUND))
          .WillOnce(Invoke(
              [](Network::Socket& socket, envoy::api::v2::core::SocketOption::SocketState) -> bool {
                fd = socket.fd();
                return true;
              }));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilterIPv6>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "test.listener.original_dstipv6"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: ::0001, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "test.listener.original_dstipv6"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v6);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  Network::ListenerConfig& listener = manager_->listeners().back().get();

  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;

  NiceMock<Network::MockListenerFilterCallbacks> callbacks;
  Network::AcceptedSocketImpl socket(
      -1, std::make_unique<Network::Address::Ipv6Instance>("::0001", 1234),
      std::make_unique<Network::Address::Ipv6Instance>("::0001", 5678));

  EXPECT_CALL(callbacks, socket()).WillOnce(Invoke([&]() -> Network::ConnectionSocket& {
    return socket;
  }));

  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr& filter) -> void {
        EXPECT_EQ(Network::FilterStatus::Continue, filter->onAccept(callbacks));
      }));

  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
  EXPECT_TRUE(socket.localAddressRestored());
  EXPECT_EQ("[1::2]:2345", socket.localAddress()->asString());
  EXPECT_NE(fd, -1);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilterOptionFailIPv6) {
  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(false));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilterIPv6>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "testfail.listener.original_dstipv6"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: "socketOptionFailListener"
    address:
      socket_address: { address: ::0001, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "testfail.listener.original_dstipv6"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v6);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "MockListenerComponentFactory: Setting socket options failed");
  EXPECT_EQ(0U, manager_->listeners().size());
}

// Validate that when neither transparent nor freebind is not set in the
// Listener, we see no socket option set.
TEST_F(ListenerManagerImplWithRealFiltersTest, TransparentFreebindListenerDisabled) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: "TestListener"
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains:
    - filters:
  )EOF",
                                                       Network::Address::IpVersion::v4);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true))
      .WillOnce(Invoke([&](Network::Address::InstanceConstSharedPtr,
                           const Network::Socket::OptionsSharedPtr& options,
                           bool) -> Network::SocketSharedPtr {
        EXPECT_EQ(options, nullptr);
        return listener_factory_.socket_;
      }));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

// Validate that when transparent is set in the Listener, we see the socket option
// propagated to setsockopt(). This is as close to an end-to-end test as we have
// for this feature, due to the complexity of creating an integration test
// involving the network stack. We only test the IPv4 case here, as the logic
// around IPv4/IPv6 handling is tested generically in
// socket_option_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, TransparentListenerEnabled) {
  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: TransparentListener
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains:
    - filters:
    transparent: true
  )EOF",
                                                       Network::Address::IpVersion::v4);
  if (ENVOY_SOCKET_IP_TRANSPARENT.has_value()) {
    EXPECT_CALL(listener_factory_, createListenSocket(_, _, true))
        .WillOnce(Invoke([this](Network::Address::InstanceConstSharedPtr,
                                const Network::Socket::OptionsSharedPtr& options,
                                bool) -> Network::SocketSharedPtr {
          EXPECT_NE(options.get(), nullptr);
          EXPECT_EQ(options->size(), 2);
          EXPECT_TRUE(
              Network::Socket::applyOptions(options, *listener_factory_.socket_,
                                            envoy::api::v2::core::SocketOption::STATE_PREBIND));
          return listener_factory_.socket_;
        }));
    // Expecting the socket option to bet set twice, once pre-bind, once post-bind.
    EXPECT_CALL(os_sys_calls,
                setsockopt_(_, ENVOY_SOCKET_IP_TRANSPARENT.value().first,
                            ENVOY_SOCKET_IP_TRANSPARENT.value().second, _, sizeof(int)))
        .Times(2)
        .WillRepeatedly(Invoke([](int, int, int, const void* optval, socklen_t) -> int {
          EXPECT_EQ(1, *static_cast<const int*>(optval));
          return 0;
        }));
    manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
    EXPECT_EQ(1U, manager_->listeners().size());
  } else {
    // MockListenerSocket is not a real socket, so this always fails in testing.
    EXPECT_THROW_WITH_MESSAGE(
        manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true), EnvoyException,
        "MockListenerComponentFactory: Setting socket options failed");
    EXPECT_EQ(0U, manager_->listeners().size());
  }
}

// Validate that when freebind is set in the Listener, we see the socket option
// propagated to setsockopt(). This is as close to an end-to-end test as we have
// for this feature, due to the complexity of creating an integration test
// involving the network stack. We only test the IPv4 case here, as the logic
// around IPv4/IPv6 handling is tested generically in
// socket_option_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, FreebindListenerEnabled) {
  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: FreebindListener
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains:
    - filters:
    freebind: true
  )EOF",
                                                       Network::Address::IpVersion::v4);
  if (ENVOY_SOCKET_IP_FREEBIND.has_value()) {
    EXPECT_CALL(listener_factory_, createListenSocket(_, _, true))
        .WillOnce(Invoke([this](Network::Address::InstanceConstSharedPtr,
                                const Network::Socket::OptionsSharedPtr& options,
                                bool) -> Network::SocketSharedPtr {
          EXPECT_NE(options.get(), nullptr);
          EXPECT_EQ(options->size(), 1);
          EXPECT_TRUE(
              Network::Socket::applyOptions(options, *listener_factory_.socket_,
                                            envoy::api::v2::core::SocketOption::STATE_PREBIND));
          return listener_factory_.socket_;
        }));
    EXPECT_CALL(os_sys_calls, setsockopt_(_, ENVOY_SOCKET_IP_FREEBIND.value().first,
                                          ENVOY_SOCKET_IP_FREEBIND.value().second, _, sizeof(int)))
        .WillOnce(Invoke([](int, int, int, const void* optval, socklen_t) -> int {
          EXPECT_EQ(1, *static_cast<const int*>(optval));
          return 0;
        }));
    manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
    EXPECT_EQ(1U, manager_->listeners().size());
  } else {
    // MockListenerSocket is not a real socket, so this always fails in testing.
    EXPECT_THROW_WITH_MESSAGE(
        manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true), EnvoyException,
        "MockListenerComponentFactory: Setting socket options failed");
    EXPECT_EQ(0U, manager_->listeners().size());
  }
}

TEST_F(ListenerManagerImplWithRealFiltersTest, LiteralSockoptListenerEnabled) {
  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: SockoptsListener
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains:
    - filters:
    socket_options: [
      # The socket goes through socket() and bind() but never listen(), so if we
      # ever saw (7, 8, 9) being applied it would cause a EXPECT_CALL failure.
      { level: 1, name: 2, int_value: 3, state: STATE_PREBIND },
      { level: 4, name: 5, int_value: 6, state: STATE_BOUND },
      { level: 7, name: 8, int_value: 9, state: STATE_LISTENING },
    ]
  )EOF",
                                                       Network::Address::IpVersion::v4);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true))
      .WillOnce(Invoke([this](Network::Address::InstanceConstSharedPtr,
                              const Network::Socket::OptionsSharedPtr& options,
                              bool) -> Network::SocketSharedPtr {
        EXPECT_NE(options.get(), nullptr);
        EXPECT_EQ(options->size(), 3);
        EXPECT_TRUE(
            Network::Socket::applyOptions(options, *listener_factory_.socket_,
                                          envoy::api::v2::core::SocketOption::STATE_PREBIND));
        return listener_factory_.socket_;
      }));
  EXPECT_CALL(os_sys_calls, setsockopt_(_, 1, 2, _, sizeof(int)))
      .WillOnce(Invoke([](int, int, int, const void* optval, socklen_t) -> int {
        EXPECT_EQ(3, *static_cast<const int*>(optval));
        return 0;
      }));
  EXPECT_CALL(os_sys_calls, setsockopt_(_, 4, 5, _, sizeof(int)))
      .WillOnce(Invoke([](int, int, int, const void* optval, socklen_t) -> int {
        EXPECT_EQ(6, *static_cast<const int*>(optval));
        return 0;
      }));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

// Set the resolver to the default IP resolver. The address resolver logic is unit tested in
// resolver_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, AddressResolver) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: AddressResolverdListener
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111, resolver_name: envoy.mock.resolver }
    filter_chains:
    - filters:
  )EOF",
                                                       Network::Address::IpVersion::v4);

  NiceMock<Network::MockAddressResolver> mock_resolver;
  EXPECT_CALL(mock_resolver, resolve(_))
      .WillOnce(Return(Network::Utility::parseInternetAddress("127.0.0.1", 1111, false)));

  Registry::InjectFactory<Network::Address::Resolver> register_resolver(mock_resolver);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CRLFilename) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.pem" }
            crl: { filename: "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.crl" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CRLInline) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.pem" }
            crl: { inline_string: "-----BEGIN X509 CRL-----\nMIIBbDCB1gIBATANBgkqhkiG9w0BAQsFADB2MQswCQYDVQQGEwJVUzETMBEGA1UE\nCBMKQ2FsaWZvcm5pYTEWMBQGA1UEBxMNU2FuIEZyYW5jaXNjbzENMAsGA1UEChME\nTHlmdDEZMBcGA1UECxMQTHlmdCBFbmdpbmVlcmluZzEQMA4GA1UEAxMHVGVzdCBD\nQRcNMTcxMjIwMTcxNDA4WhcNMjcxMjE4MTcxNDA4WjAcMBoCCQDZy/Qp7iAfHxcN\nMTcxMjIwMTcxMjU0WqAOMAwwCgYDVR0UBAMCAQAwDQYJKoZIhvcNAQELBQADgYEA\nOTn5Fgb44xtFd9QGtbTElZ3iwdlcOxRHjgQMd+ydzEEZRMzMgb4/NmEsgXAsxbrx\ntKmpgll8TblscitkglvGk8s4obi/OtgxNIvn+7pOBTjmrgJkcktBUDEWRbLZjsZx\nyH+5teBZ0tH0tVy914QeGitZFV8awK1hlJwlAz9g/jo=\n-----END X509 CRL-----" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, InvalidCRLInline) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.pem" }
            crl: { inline_string: "-----BEGIN X509 CRL-----\nTOTALLY_NOT_A_CRL_HERE\n-----END X509 CRL-----\n" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load CRL from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CRLWithNoCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
          validation_context:
            crl: { filename: "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.crl" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_REGEX(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                          EnvoyException, "^Failed to load CRL from .* without trusted CA$");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, VerifySanWithNoCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
          validation_context:
            verify_subject_alt_name: "spiffe://lyft.com/testclient"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "SAN-based verification of peer certificates without trusted CA "
                            "is insecure and not allowed");
}

// Disabling certificate expiration checks only makes sense with a trusted CA.
TEST_F(ListenerManagerImplWithRealFiltersTest, VerifyIgnoreExpirationWithNoCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }
          validation_context:
            allow_expired_certificate: true
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "Certificate validity period is always ignored without trusted CA");
}

// Verify that with a CA, expired certificates are allowed.
TEST_F(ListenerManagerImplWithRealFiltersTest, VerifyIgnoreExpirationWithCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/common/ssl/test_data/san_dns_key.pem" }

          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/common/ssl/test_data/ca_cert.pem" }
            allow_expired_certificate: true
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_NO_THROW(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true));
}

} // namespace Server
} // namespace Envoy
