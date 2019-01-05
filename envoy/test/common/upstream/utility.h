#pragma once

#include "envoy/upstream/upstream.h"

#include "common/common/utility.h"
#include "common/config/cds_json.h"
#include "common/json/json_loader.h"
#include "common/network/utility.h"
#include "common/stats/stats_options_impl.h"
#include "common/upstream/upstream_impl.h"

#include "fmt/printf.h"

namespace Envoy {
namespace Upstream {
namespace {

inline std::string defaultSdsClusterJson(const std::string& name) {
  return fmt::sprintf(R"EOF(
  {
    "name": "%s",
    "connect_timeout_ms": 250,
    "type": "sds",
    "lb_type": "round_robin"
  }
  )EOF",
                      name);
}

inline std::string defaultStaticClusterJson(const std::string& name) {
  return fmt::sprintf(R"EOF(
  {
    "name": "%s",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "round_robin",
    "hosts": [{"url": "tcp://127.0.0.1:11001"}]
  }
  )EOF",
                      name);
}

inline std::string clustersJson(const std::vector<std::string>& clusters) {
  return fmt::sprintf("\"clusters\": [%s]", StringUtil::join(clusters, ","));
}

inline envoy::api::v2::Cluster parseClusterFromJson(const std::string& json_string) {
  envoy::api::v2::Cluster cluster;
  auto json_object_ptr = Json::Factory::loadFromString(json_string);
  Stats::StatsOptionsImpl stats_options;
  Config::CdsJson::translateCluster(*json_object_ptr,
                                    absl::optional<envoy::api::v2::core::ConfigSource>(), cluster,
                                    stats_options);
  return cluster;
}

inline envoy::api::v2::Cluster parseClusterFromV2Yaml(const std::string& yaml) {
  envoy::api::v2::Cluster cluster;
  MessageUtil::loadFromYaml(yaml, cluster);
  return cluster;
}

inline envoy::api::v2::Cluster defaultStaticCluster(const std::string& name) {
  return parseClusterFromJson(defaultStaticClusterJson(name));
}

inline envoy::api::v2::Cluster
parseSdsClusterFromJson(const std::string& json_string,
                        const envoy::api::v2::core::ConfigSource eds_config) {
  envoy::api::v2::Cluster cluster;
  auto json_object_ptr = Json::Factory::loadFromString(json_string);
  Stats::StatsOptionsImpl stats_options;
  Config::CdsJson::translateCluster(*json_object_ptr, eds_config, cluster, stats_options);
  return cluster;
}

inline HostSharedPtr makeTestHost(ClusterInfoConstSharedPtr cluster, const std::string& url,
                                  uint32_t weight = 1) {
  return HostSharedPtr{new HostImpl(
      cluster, "", Network::Utility::resolveUrl(url),
      envoy::api::v2::core::Metadata::default_instance(), weight, envoy::api::v2::core::Locality(),
      envoy::api::v2::endpoint::Endpoint::HealthCheckConfig::default_instance(), 0)};
}

inline HostSharedPtr makeTestHost(ClusterInfoConstSharedPtr cluster, const std::string& url,
                                  const envoy::api::v2::core::Metadata& metadata,
                                  uint32_t weight = 1) {
  return HostSharedPtr{
      new HostImpl(cluster, "", Network::Utility::resolveUrl(url), metadata, weight,
                   envoy::api::v2::core::Locality(),
                   envoy::api::v2::endpoint::Endpoint::HealthCheckConfig::default_instance(), 0)};
}

inline HostSharedPtr
makeTestHost(ClusterInfoConstSharedPtr cluster, const std::string& url,
             const envoy::api::v2::endpoint::Endpoint::HealthCheckConfig& health_check_config,
             uint32_t weight = 1) {
  return HostSharedPtr{new HostImpl(cluster, "", Network::Utility::resolveUrl(url),
                                    envoy::api::v2::core::Metadata::default_instance(), weight,
                                    envoy::api::v2::core::Locality(), health_check_config, 0)};
}

inline HostDescriptionConstSharedPtr makeTestHostDescription(ClusterInfoConstSharedPtr cluster,
                                                             const std::string& url) {
  return HostDescriptionConstSharedPtr{new HostDescriptionImpl(
      cluster, "", Network::Utility::resolveUrl(url),
      envoy::api::v2::core::Metadata::default_instance(),
      envoy::api::v2::core::Locality().default_instance(),
      envoy::api::v2::endpoint::Endpoint::HealthCheckConfig::default_instance(), 0)};
}

inline HostsPerLocalitySharedPtr makeHostsPerLocality(std::vector<HostVector>&& locality_hosts,
                                                      bool force_no_local_locality = false) {
  return std::make_shared<HostsPerLocalityImpl>(
      std::move(locality_hosts), !force_no_local_locality && !locality_hosts.empty());
}

inline envoy::api::v2::core::HealthCheck
parseHealthCheckFromV2Yaml(const std::string& yaml_string) {
  envoy::api::v2::core::HealthCheck health_check;
  MessageUtil::loadFromYaml(yaml_string, health_check);
  return health_check;
}

inline envoy::api::v2::core::HealthCheck
parseHealthCheckFromV1Json(const std::string& json_string) {
  envoy::api::v2::core::HealthCheck health_check;
  auto json_object_ptr = Json::Factory::loadFromString(json_string);
  Config::CdsJson::translateHealthCheck(*json_object_ptr, health_check);
  return health_check;
}

} // namespace
} // namespace Upstream
} // namespace Envoy
