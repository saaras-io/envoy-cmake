#pragma once

#include <cstdint>
#include <string>

#include "envoy/config/filter/network/http_connection_manager/v2/http_connection_manager.pb.h"
#include "envoy/config/subscription.h"
#include "envoy/stats/scope.h"

#include "common/common/assert.h"
#include "common/http/rest_api_fetcher.h"

namespace Envoy {
namespace Router {

/**
 * Subscription implementation that reads host information from the v1 REST Route Discovery
 * Service.
 */
class RdsSubscription : public Http::RestApiFetcher,
                        public Envoy::Config::Subscription<envoy::api::v2::RouteConfiguration>,
                        Logger::Loggable<Logger::Id::upstream> {
public:
  RdsSubscription(Envoy::Config::SubscriptionStats stats,
                  const envoy::config::filter::network::http_connection_manager::v2::Rds& rds,
                  Upstream::ClusterManager& cm, Event::Dispatcher& dispatcher,
                  Runtime::RandomGenerator& random, const LocalInfo::LocalInfo& local_info,
                  const Stats::Scope& scope);

private:
  // Config::Subscription
  void start(const std::vector<std::string>& resources,
             Envoy::Config::SubscriptionCallbacks<envoy::api::v2::RouteConfiguration>& callbacks)
      override {
    // We can only handle a single cluster route configuration, it's a design error to ever use this
    // type of Subscription with more than a single cluster.
    ASSERT(resources.size() == 1);
    route_config_name_ = resources[0];
    callbacks_ = &callbacks;
    RestApiFetcher::initialize();
  }

  void updateResources(const std::vector<std::string>& resources) override {
    // We should never hit this at runtime, since this legacy adapter is only used by HTTP
    // connection manager that doesn't do dynamic modification of resources.
    UNREFERENCED_PARAMETER(resources);
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }

  // Http::RestApiFetcher
  void createRequest(Http::Message& request) override;
  void parseResponse(const Http::Message& response) override;
  void onFetchComplete() override;
  void onFetchFailure(const EnvoyException* e) override;

  std::string route_config_name_;
  const LocalInfo::LocalInfo& local_info_;
  Envoy::Config::SubscriptionCallbacks<envoy::api::v2::RouteConfiguration>* callbacks_ = nullptr;
  Envoy::Config::SubscriptionStats stats_;
  const Stats::Scope& scope_;
};

} // namespace Router
} // namespace Envoy
