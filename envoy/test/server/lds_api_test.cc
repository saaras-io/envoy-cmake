#include "envoy/api/v2/lds.pb.h"

#include "common/config/utility.h"
#include "common/http/message_impl.h"

#include "server/lds_api.h"

#include "test/mocks/server/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::Return;
using testing::Throw;

namespace Envoy {
namespace Server {

class LdsApiTest : public testing::Test {
public:
  LdsApiTest() : request_(&cluster_manager_.async_client_) {}

  void setup(bool v2_rest = false) {
    v2_rest_ = v2_rest;
    const std::string config_json = R"EOF(
    {
      "cluster": "foo_cluster",
      "refresh_delay_ms": 1000
    }
    )EOF";

    Json::ObjectSharedPtr config = Json::Factory::loadFromString(config_json);
    envoy::api::v2::core::ConfigSource lds_config;
    Config::Utility::translateLdsConfig(*config, lds_config);
    if (v2_rest) {
      lds_config.mutable_api_config_source()->set_api_type(
          envoy::api::v2::core::ApiConfigSource::REST);
    }
    Upstream::ClusterManager::ClusterInfoMap cluster_map;
    Upstream::MockCluster cluster;
    cluster_map.emplace("foo_cluster", cluster);
    EXPECT_CALL(cluster_manager_, clusters()).WillOnce(Return(cluster_map));
    EXPECT_CALL(cluster, info());
    EXPECT_CALL(*cluster.info_, addedViaApi());
    EXPECT_CALL(cluster, info());
    EXPECT_CALL(*cluster.info_, type());
    interval_timer_ = new Event::MockTimer(&dispatcher_);
    EXPECT_CALL(init_, registerTarget(_));
    lds_.reset(new LdsApiImpl(lds_config, cluster_manager_, dispatcher_, random_, init_,
                              local_info_, store_, listener_manager_));

    expectRequest();
    init_.initialize();
  }

  void expectAdd(const std::string& listener_name, absl::optional<std::string> version,
                 bool updated) {
    if (!version) {
      EXPECT_CALL(listener_manager_, addOrUpdateListener(_, _, true))
          .WillOnce(Invoke([listener_name, updated](const envoy::api::v2::Listener& config,
                                                    const std::string&, bool) -> bool {
            EXPECT_EQ(listener_name, config.name());
            return updated;
          }));
    } else {
      EXPECT_CALL(listener_manager_, addOrUpdateListener(_, version.value(), true))
          .WillOnce(Invoke([listener_name, updated](const envoy::api::v2::Listener& config,
                                                    const std::string&, bool) -> bool {
            EXPECT_EQ(listener_name, config.name());
            return updated;
          }));
    }
  }

  void expectRequest() {
    EXPECT_CALL(cluster_manager_, httpAsyncClientForCluster("foo_cluster"));
    EXPECT_CALL(cluster_manager_.async_client_, send_(_, _, _))
        .WillOnce(Invoke(
            [&](Http::MessagePtr& request, Http::AsyncClient::Callbacks& callbacks,
                const absl::optional<std::chrono::milliseconds>&) -> Http::AsyncClient::Request* {
              EXPECT_EQ(
                  (Http::TestHeaderMapImpl{
                      {":method", v2_rest_ ? "POST" : "GET"},
                      {":path", v2_rest_ ? "/v2/discovery:listeners"
                                         : "/v1/listeners/cluster_name/node_name"},
                      {":authority", "foo_cluster"},
                      {"content-type", "application/json"},
                      {"content-length",
                       request->body() ? fmt::FormatInt(request->body()->length()).str() : "0"}}),
                  request->headers());
              callbacks_ = &callbacks;
              return &request_;
            }));
  }

  void makeListenersAndExpectCall(const std::vector<std::string>& listener_names) {
    std::vector<std::reference_wrapper<Network::ListenerConfig>> refs;
    listeners_.clear();
    for (const auto& name : listener_names) {
      listeners_.emplace_back();
      listeners_.back().name_ = name;
      refs.push_back(listeners_.back());
    }
    EXPECT_CALL(listener_manager_, listeners()).WillOnce(Return(refs));
  }

  bool v2_rest_{};
  NiceMock<Upstream::MockClusterManager> cluster_manager_;
  Event::MockDispatcher dispatcher_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  Init::MockManager init_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  Stats::IsolatedStoreImpl store_;
  MockListenerManager listener_manager_;
  Http::MockAsyncClientRequest request_;
  std::unique_ptr<LdsApiImpl> lds_;
  Event::MockTimer* interval_timer_{};
  Http::AsyncClient::Callbacks* callbacks_{};

private:
  std::list<NiceMock<Network::MockListenerConfig>> listeners_;
};

// Negative test for protoc-gen-validate constraints.
TEST_F(LdsApiTest, ValidateFail) {
  InSequence s;

  setup(true);

  Protobuf::RepeatedPtrField<envoy::api::v2::Listener> listeners;
  listeners.Add();

  EXPECT_THROW(lds_->onConfigUpdate(listeners, ""), ProtoValidationException);
  EXPECT_CALL(request_, cancel());
}

TEST_F(LdsApiTest, UnknownCluster) {
  const std::string config_json = R"EOF(
  {
    "cluster": "foo_cluster",
    "refresh_delay_ms": 1000
  }
  )EOF";

  Json::ObjectSharedPtr config = Json::Factory::loadFromString(config_json);
  envoy::api::v2::core::ConfigSource lds_config;
  Config::Utility::translateLdsConfig(*config, lds_config);
  Upstream::ClusterManager::ClusterInfoMap cluster_map;
  EXPECT_CALL(cluster_manager_, clusters()).WillOnce(Return(cluster_map));
  EXPECT_THROW_WITH_MESSAGE(
      LdsApiImpl(lds_config, cluster_manager_, dispatcher_, random_, init_, local_info_, store_,
                 listener_manager_),
      EnvoyException,
      "envoy::api::v2::core::ConfigSource must have a statically defined non-EDS "
      "cluster: 'foo_cluster' does not exist, was added via api, or is an "
      "EDS cluster");
}

TEST_F(LdsApiTest, MisconfiguredListenerNameIsPresentInException) {
  InSequence s;

  setup(true);

  Protobuf::RepeatedPtrField<envoy::api::v2::Listener> listeners;
  std::vector<std::reference_wrapper<Network::ListenerConfig>> existing_listeners;

  // Construct a minimal listener that would pass proto validation.
  auto listener = listeners.Add();
  listener->set_name("invalid-listener");
  auto socket_address = listener->mutable_address()->mutable_socket_address();
  socket_address->set_address("invalid-address");
  socket_address->set_port_value(1);
  listener->add_filter_chains();

  EXPECT_CALL(listener_manager_, listeners()).WillOnce(Return(existing_listeners));

  EXPECT_CALL(listener_manager_, addOrUpdateListener(_, _, true))
      .WillOnce(Throw(EnvoyException("something is wrong")));

  EXPECT_THROW_WITH_MESSAGE(lds_->onConfigUpdate(listeners, ""), EnvoyException,
                            "Error adding/updating listener invalid-listener: something is wrong");
  EXPECT_CALL(request_, cancel());
}

// Validate onConfigUpadte throws EnvoyException with duplicate listeners.
TEST_F(LdsApiTest, ValidateDuplicateListeners) {
  InSequence s;

  setup(true);

  Protobuf::RepeatedPtrField<envoy::api::v2::Listener> listeners;
  auto* listener_1 = listeners.Add();
  listener_1->set_name("duplicate_listener");

  auto* listener_2 = listeners.Add();
  listener_2->set_name("duplicate_listener");

  EXPECT_THROW_WITH_MESSAGE(lds_->onConfigUpdate(listeners, ""), EnvoyException,
                            "duplicate listener duplicate_listener found");
  EXPECT_CALL(request_, cancel());
}

TEST_F(LdsApiTest, BadLocalInfo) {
  interval_timer_ = new Event::MockTimer(&dispatcher_);
  const std::string config_json = R"EOF(
  {
    "cluster": "foo_cluster",
    "refresh_delay_ms": 1000
  }
  )EOF";

  Json::ObjectSharedPtr config = Json::Factory::loadFromString(config_json);
  envoy::api::v2::core::ConfigSource lds_config;
  Config::Utility::translateLdsConfig(*config, lds_config);
  Upstream::ClusterManager::ClusterInfoMap cluster_map;
  Upstream::MockCluster cluster;
  cluster_map.emplace("foo_cluster", cluster);
  EXPECT_CALL(cluster_manager_, clusters()).WillOnce(Return(cluster_map));
  EXPECT_CALL(cluster, info()).Times(2);
  EXPECT_CALL(*cluster.info_, addedViaApi());
  EXPECT_CALL(*cluster.info_, type());
  ON_CALL(local_info_, clusterName()).WillByDefault(Return(std::string()));
  EXPECT_THROW_WITH_MESSAGE(
      LdsApiImpl(lds_config, cluster_manager_, dispatcher_, random_, init_, local_info_, store_,
                 listener_manager_),
      EnvoyException,
      "lds: node 'id' and 'cluster' are required. Set it either in 'node' config or via "
      "--service-node and --service-cluster options.");
}

TEST_F(LdsApiTest, Basic) {
  InSequence s;

  setup();

  const std::string response1_json = R"EOF(
  {
    "listeners": [
    {
      "name": "listener1",
      "address": "tcp://0.0.0.0:1",
      "filters": []
    },
    {
      "name": "listener2",
      "address": "tcp://0.0.0.0:2",
      "filters": []
    }
    ]
  }
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response1_json));

  makeListenersAndExpectCall({});
  expectAdd("listener1", "hash_d5b83398260abbbc", true);
  expectAdd("listener2", "hash_d5b83398260abbbc", true);
  EXPECT_CALL(init_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  EXPECT_EQ(Config::Utility::computeHashedVersion(response1_json).first, lds_->versionInfo());
  EXPECT_EQ(15400115654359694268U, store_.gauge("listener_manager.lds.version").value());
  expectRequest();
  interval_timer_->callback_();

  const std::string response2_json = R"EOF(
  {
    "listeners": [
    {
      "name": "listener1",
      "address": "tcp://0.0.0.0:1",
      "filters": []
    },
    {
      "name": "listener3",
      "address": "tcp://0.0.0.0:3",
      "filters": []
    }
    ]
  }
  )EOF";

  message.reset(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response2_json));

  makeListenersAndExpectCall({"listener1", "listener2"});
  EXPECT_CALL(listener_manager_, removeListener("listener2")).WillOnce(Return(true));
  expectAdd("listener1", "hash_fabfe23d041792d3", false);
  expectAdd("listener3", "hash_fabfe23d041792d3", true);
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));
  EXPECT_EQ(Config::Utility::computeHashedVersion(response2_json).first, lds_->versionInfo());

  EXPECT_EQ(2UL, store_.counter("listener_manager.lds.update_attempt").value());
  EXPECT_EQ(2UL, store_.counter("listener_manager.lds.update_success").value());
  EXPECT_EQ(18068408981723255507U, store_.gauge("listener_manager.lds.version").value());
}

// Regression test issue #2188 where an empty ca_cert_file field was created and caused the LDS
// update to fail validation.
TEST_F(LdsApiTest, TlsConfigWithoutCaCert) {
  InSequence s;

  setup();

  std::string response1_json = R"EOF(
  {
    "listeners": [
    ]
  }
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response1_json));

  makeListenersAndExpectCall({});
  EXPECT_CALL(init_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  EXPECT_EQ(Config::Utility::computeHashedVersion(response1_json).first, lds_->versionInfo());
  expectRequest();
  interval_timer_->callback_();

  std::string response2_basic = R"EOF(
  {{
   "listeners" : [
         {{
         "ssl_context" : {{
            "cipher_suites" : "[ECDHE-RSA-AES256-GCM-SHA384|ECDHE-RSA-AES128-GCM-SHA256]",
            "cert_chain_file" : "{}",
            "private_key_file" : "{}"
         }},
         "address" : "tcp://0.0.0.0:61001",
         "name" : "listener-8080",
         "filters" : [
            {{
               "config" : {{
                  "stat_prefix" : "ingress_tcp-9534127d-306e-49b5-5158-9688cf1cd33b",
                  "route_config" : {{
                     "routes" : [
                        {{
                           "cluster" : "0-service-cluster"
                        }}
                     ]
                  }}
               }},
               "type" : "read",
               "name" : "tcp_proxy"
            }}
         ]
      }}
   ]
  }}
  )EOF";
  std::string response2_json =
      fmt::format(response2_basic,
                  TestEnvironment::runfilesPath("/test/config/integration/certs/servercert.pem"),
                  TestEnvironment::runfilesPath("/test/config/integration/certs/serverkey.pem"));

  message.reset(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response2_json));
  makeListenersAndExpectCall({
      "listener-8080",
  });
  // Can't check version here because of bazel sandbox paths for the certs.
  expectAdd("listener-8080", {}, true);
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  EXPECT_NO_THROW(callbacks_->onSuccess(std::move(message)));
  EXPECT_EQ(Config::Utility::computeHashedVersion(response2_json).first, lds_->versionInfo());
}

TEST_F(LdsApiTest, Failure) {
  InSequence s;

  setup();

  const std::string response_json = R"EOF(
  {
    "listeners" : {}
  }
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response_json));

  EXPECT_CALL(init_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  expectRequest();
  interval_timer_->callback_();

  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onFailure(Http::AsyncClient::FailureReason::Reset);
  EXPECT_EQ("", lds_->versionInfo());

  EXPECT_EQ(2UL, store_.counter("listener_manager.lds.update_attempt").value());
  EXPECT_EQ(1UL, store_.counter("listener_manager.lds.update_failure").value());
  // Validate that the schema error increments update_rejected stat.
  EXPECT_EQ(1UL, store_.counter("listener_manager.lds.update_failure").value());
  EXPECT_EQ(0UL, store_.gauge("listener_manager.lds.version").value());
}

TEST_F(LdsApiTest, ReplacingListenerWithSameAddress) {
  InSequence s;

  setup();

  const std::string response1_json = R"EOF(
  {
    "listeners": [
    {
      "name": "listener1",
      "address": "tcp://0.0.0.0:1",
      "filters": []
    },
    {
      "name": "listener2",
      "address": "tcp://0.0.0.0:2",
      "filters": []
    }
    ]
  }
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response1_json));

  makeListenersAndExpectCall({});
  expectAdd("listener1", "hash_d5b83398260abbbc", true);
  expectAdd("listener2", "hash_d5b83398260abbbc", true);
  EXPECT_CALL(init_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  EXPECT_EQ(Config::Utility::computeHashedVersion(response1_json).first, lds_->versionInfo());
  EXPECT_EQ(15400115654359694268U, store_.gauge("listener_manager.lds.version").value());
  expectRequest();
  interval_timer_->callback_();

  const std::string response2_json = R"EOF(
  {
    "listeners": [
    {
      "name": "listener1",
      "address": "tcp://0.0.0.0:1",
      "filters": []
    },
    {
      "name": "listener3",
      "address": "tcp://0.0.0.0:2",
      "filters": []
    }
    ]
  }
  )EOF";

  message.reset(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response2_json));

  makeListenersAndExpectCall({"listener1", "listener2"});
  EXPECT_CALL(listener_manager_, removeListener("listener2")).WillOnce(Return(true));
  expectAdd("listener1", "hash_16e261d4c65402a2", false);
  expectAdd("listener3", "hash_16e261d4c65402a2", true);
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  EXPECT_EQ(Config::Utility::computeHashedVersion(response2_json).first, lds_->versionInfo());
  EXPECT_EQ(2UL, store_.counter("listener_manager.lds.update_attempt").value());
  EXPECT_EQ(2UL, store_.counter("listener_manager.lds.update_success").value());
  EXPECT_EQ(1648987980059378338U, store_.gauge("listener_manager.lds.version").value());
}

} // namespace Server
} // namespace Envoy
