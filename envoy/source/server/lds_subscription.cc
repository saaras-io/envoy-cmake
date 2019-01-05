#include "server/lds_subscription.h"

#include "envoy/api/v2/listener/listener.pb.h"

#include "common/common/fmt.h"
#include "common/config/lds_json.h"
#include "common/config/utility.h"
#include "common/http/headers.h"
#include "common/json/config_schemas.h"
#include "common/json/json_loader.h"

namespace Envoy {
namespace Server {

LdsSubscription::LdsSubscription(Config::SubscriptionStats stats,
                                 const envoy::api::v2::core::ConfigSource& lds_config,
                                 Upstream::ClusterManager& cm, Event::Dispatcher& dispatcher,
                                 Runtime::RandomGenerator& random,
                                 const LocalInfo::LocalInfo& local_info,
                                 const Stats::StatsOptions& stats_options)
    : RestApiFetcher(cm, lds_config.api_config_source(), dispatcher, random),
      local_info_(local_info), stats_(stats), stats_options_(stats_options) {
  Envoy::Config::Utility::checkClusterAndLocalInfo(
      "lds", lds_config.api_config_source().cluster_names()[0], cm, local_info);
}

void LdsSubscription::createRequest(Http::Message& request) {
  ENVOY_LOG(debug, "lds: starting request");
  stats_.update_attempt_.inc();
  request.headers().insertMethod().value().setReference(Http::Headers::get().MethodValues.Get);
  request.headers().insertPath().value(
      fmt::format("/v1/listeners/{}/{}", local_info_.clusterName(), local_info_.nodeName()));
  request.headers().insertContentType().value().setReference(
      Http::Headers::get().ContentTypeValues.Json);
  request.headers().insertContentLength().value(size_t(0));
}

void LdsSubscription::parseResponse(const Http::Message& response) {
  ENVOY_LOG(debug, "lds: parsing response");
  const std::string response_body = response.bodyAsString();
  Json::ObjectSharedPtr response_json = Json::Factory::loadFromString(response_body);
  response_json->validateSchema(Json::Schema::LDS_SCHEMA);
  std::vector<Json::ObjectSharedPtr> json_listeners = response_json->getObjectArray("listeners");

  Protobuf::RepeatedPtrField<envoy::api::v2::Listener> resources;
  for (const Json::ObjectSharedPtr& json_listener : json_listeners) {
    Config::LdsJson::translateListener(*json_listener, *resources.Add(), stats_options_);
  }

  std::pair<std::string, uint64_t> hash =
      Envoy::Config::Utility::computeHashedVersion(response_body);
  callbacks_->onConfigUpdate(resources, hash.first);
  stats_.version_.set(hash.second);
  stats_.update_success_.inc();
}

void LdsSubscription::onFetchComplete() {}

void LdsSubscription::onFetchFailure(const EnvoyException* e) {
  callbacks_->onConfigUpdateFailed(e);
  if (e) {
    stats_.update_rejected_.inc();
    ENVOY_LOG(warn, "lds: fetch failure: {}", e->what());
  } else {
    stats_.update_failure_.inc();
    ENVOY_LOG(info, "lds: fetch failure: network error");
  }
}

} // namespace Server
} // namespace Envoy
