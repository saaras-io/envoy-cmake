#include "extensions/filters/network/http_connection_manager/config.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "envoy/config/filter/network/http_connection_manager/v2/http_connection_manager.pb.validate.h"
#include "envoy/filesystem/filesystem.h"
#include "envoy/registry/registry.h"
#include "envoy/server/admin.h"

#include "common/access_log/access_log_impl.h"
#include "common/common/fmt.h"
#include "common/config/filter_json.h"
#include "common/config/utility.h"
#include "common/http/date_provider_impl.h"
#include "common/http/default_server_string.h"
#include "common/http/http1/codec_impl.h"
#include "common/http/http2/codec_impl.h"
#include "common/http/utility.h"
#include "common/json/config_schemas.h"
#include "common/protobuf/utility.h"
#include "common/router/rds_impl.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace HttpConnectionManager {
namespace {

typedef std::list<Http::FilterFactoryCb> FilterFactoriesList;
typedef std::map<std::string, std::unique_ptr<FilterFactoriesList>> FilterFactoryMap;

FilterFactoryMap::const_iterator findUpgradeCaseInsensitive(const FilterFactoryMap& upgrade_map,
                                                            absl::string_view upgrade_type) {
  for (auto it = upgrade_map.begin(); it != upgrade_map.end(); ++it) {
    if (StringUtil::CaseInsensitiveCompare()(it->first, upgrade_type)) {
      return it;
    }
  }
  return upgrade_map.end();
}

std::unique_ptr<Http::InternalAddressConfig> createInternalAddressConfig(
    const envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager&
        config) {
  if (config.has_internal_address_config()) {
    return std::make_unique<InternalAddressConfig>(config.internal_address_config());
  }

  return std::make_unique<Http::DefaultInternalAddressConfig>();
}

} // namespace

// Singleton registration via macro defined in envoy/singleton/manager.h
SINGLETON_MANAGER_REGISTRATION(date_provider);
SINGLETON_MANAGER_REGISTRATION(route_config_provider_manager);

Network::FilterFactoryCb
HttpConnectionManagerFilterConfigFactory::createFilterFactoryFromProtoTyped(
    const envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager&
        proto_config,
    Server::Configuration::FactoryContext& context) {
  std::shared_ptr<Http::TlsCachingDateProviderImpl> date_provider =
      context.singletonManager().getTyped<Http::TlsCachingDateProviderImpl>(
          SINGLETON_MANAGER_REGISTERED_NAME(date_provider), [&context] {
            return std::make_shared<Http::TlsCachingDateProviderImpl>(context.dispatcher(),
                                                                      context.threadLocal());
          });

  std::shared_ptr<Router::RouteConfigProviderManager> route_config_provider_manager =
      context.singletonManager().getTyped<Router::RouteConfigProviderManager>(
          SINGLETON_MANAGER_REGISTERED_NAME(route_config_provider_manager), [&context] {
            return std::make_shared<Router::RouteConfigProviderManagerImpl>(context.admin());
          });

  std::shared_ptr<HttpConnectionManagerConfig> filter_config(new HttpConnectionManagerConfig(
      proto_config, context, *date_provider, *route_config_provider_manager));

  // This lambda captures the shared_ptrs created above, thus preserving the
  // reference count. Moreover, keep in mind the capture list determines
  // destruction order.
  return [route_config_provider_manager, filter_config, &context,
          date_provider](Network::FilterManager& filter_manager) -> void {
    filter_manager.addReadFilter(Network::ReadFilterSharedPtr{new Http::ConnectionManagerImpl(
        *filter_config, context.drainDecision(), context.random(), context.httpTracer(),
        context.runtime(), context.localInfo(), context.clusterManager(),
        &context.overloadManager(), context.dispatcher().timeSystem())});
  };
}

Network::FilterFactoryCb HttpConnectionManagerFilterConfigFactory::createFilterFactory(
    const Json::Object& json_config, Server::Configuration::FactoryContext& context) {
  envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager proto_config;
  Config::FilterJson::translateHttpConnectionManager(json_config, proto_config,
                                                     context.scope().statsOptions());
  return createFilterFactoryFromProtoTyped(proto_config, context);
}

/**
 * Static registration for the HTTP connection manager filter.
 */
static Registry::RegisterFactory<HttpConnectionManagerFilterConfigFactory,
                                 Server::Configuration::NamedNetworkFilterConfigFactory>
    registered_;

std::string
HttpConnectionManagerConfigUtility::determineNextProtocol(Network::Connection& connection,
                                                          const Buffer::Instance& data) {
  if (!connection.nextProtocol().empty()) {
    return connection.nextProtocol();
  }

  // See if the data we have so far shows the HTTP/2 prefix. We ignore the case where someone sends
  // us the first few bytes of the HTTP/2 prefix since in all public cases we use SSL/ALPN. For
  // internal cases this should practically never happen.
  if (-1 != data.search(Http::Http2::CLIENT_MAGIC_PREFIX.c_str(),
                        Http::Http2::CLIENT_MAGIC_PREFIX.size(), 0)) {
    return Http::Http2::ALPN_STRING;
  }

  return "";
}

InternalAddressConfig::InternalAddressConfig(
    const envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
        InternalAddressConfig& config)
    : unix_sockets_(config.unix_sockets()) {}

HttpConnectionManagerConfig::HttpConnectionManagerConfig(
    const envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager&
        config,
    Server::Configuration::FactoryContext& context, Http::DateProvider& date_provider,
    Router::RouteConfigProviderManager& route_config_provider_manager)
    : context_(context), stats_prefix_(fmt::format("http.{}.", config.stat_prefix())),
      stats_(Http::ConnectionManagerImpl::generateStats(stats_prefix_, context_.scope())),
      tracing_stats_(
          Http::ConnectionManagerImpl::generateTracingStats(stats_prefix_, context_.scope())),
      use_remote_address_(PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, use_remote_address, false)),
      internal_address_config_(createInternalAddressConfig(config)),
      xff_num_trusted_hops_(config.xff_num_trusted_hops()),
      skip_xff_append_(config.skip_xff_append()), via_(config.via()),
      route_config_provider_manager_(route_config_provider_manager),
      http2_settings_(Http::Utility::parseHttp2Settings(config.http2_protocol_options())),
      http1_settings_(Http::Utility::parseHttp1Settings(config.http_protocol_options())),
      idle_timeout_(PROTOBUF_GET_OPTIONAL_MS(config, idle_timeout)),
      stream_idle_timeout_(
          PROTOBUF_GET_MS_OR_DEFAULT(config, stream_idle_timeout, StreamIdleTimeoutMs)),
      drain_timeout_(PROTOBUF_GET_MS_OR_DEFAULT(config, drain_timeout, 5000)),
      generate_request_id_(PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, generate_request_id, true)),
      date_provider_(date_provider),
      listener_stats_(Http::ConnectionManagerImpl::generateListenerStats(stats_prefix_,
                                                                         context_.listenerScope())),
      proxy_100_continue_(config.proxy_100_continue()),
      delayed_close_timeout_(PROTOBUF_GET_MS_OR_DEFAULT(config, delayed_close_timeout, 1000)) {

  route_config_provider_ = Router::RouteConfigProviderUtil::create(config, context_, stats_prefix_,
                                                                   route_config_provider_manager_);

  switch (config.forward_client_cert_details()) {
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::SANITIZE:
    forward_client_cert_ = Http::ForwardClientCertType::Sanitize;
    break;
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
      FORWARD_ONLY:
    forward_client_cert_ = Http::ForwardClientCertType::ForwardOnly;
    break;
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
      APPEND_FORWARD:
    forward_client_cert_ = Http::ForwardClientCertType::AppendForward;
    break;
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
      SANITIZE_SET:
    forward_client_cert_ = Http::ForwardClientCertType::SanitizeSet;
    break;
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
      ALWAYS_FORWARD_ONLY:
    forward_client_cert_ = Http::ForwardClientCertType::AlwaysForwardOnly;
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }

  const auto& set_current_client_cert_details = config.set_current_client_cert_details();
  if (set_current_client_cert_details.cert()) {
    set_current_client_cert_details_.push_back(Http::ClientCertDetailsType::Cert);
  }
  if (PROTOBUF_GET_WRAPPED_OR_DEFAULT(set_current_client_cert_details, subject, false)) {
    set_current_client_cert_details_.push_back(Http::ClientCertDetailsType::Subject);
  }
  if (set_current_client_cert_details.uri()) {
    set_current_client_cert_details_.push_back(Http::ClientCertDetailsType::URI);
  }
  if (set_current_client_cert_details.dns()) {
    set_current_client_cert_details_.push_back(Http::ClientCertDetailsType::DNS);
  }

  if (config.has_add_user_agent() && config.add_user_agent().value()) {
    user_agent_ = context_.localInfo().clusterName();
  }

  if (config.has_tracing()) {
    const auto& tracing_config = config.tracing();

    Tracing::OperationName tracing_operation_name;
    std::vector<Http::LowerCaseString> request_headers_for_tags;

    switch (tracing_config.operation_name()) {
    case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
        Tracing::INGRESS:
      tracing_operation_name = Tracing::OperationName::Ingress;
      break;
    case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::
        Tracing::EGRESS:
      tracing_operation_name = Tracing::OperationName::Egress;
      break;
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }

    for (const std::string& header : tracing_config.request_headers_for_tags()) {
      request_headers_for_tags.push_back(Http::LowerCaseString(header));
    }

    uint64_t client_sampling{
        PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(tracing_config, client_sampling, 100, 100)};
    uint64_t random_sampling{PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
        tracing_config, random_sampling, 10000, 10000)};
    uint64_t overall_sampling{
        PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(tracing_config, overall_sampling, 100, 100)};

    tracing_config_.reset(new Http::TracingConnectionManagerConfig(
        {tracing_operation_name, request_headers_for_tags, client_sampling, random_sampling,
         overall_sampling}));
  }

  for (const auto& access_log : config.access_log()) {
    AccessLog::InstanceSharedPtr current_access_log =
        AccessLog::AccessLogFactory::fromProto(access_log, context_);
    access_logs_.push_back(current_access_log);
  }

  if (!config.server_name().empty()) {
    server_name_ = config.server_name();
  } else {
    server_name_ = Http::DefaultServerString::get();
  }

  switch (config.codec_type()) {
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::AUTO:
    codec_type_ = CodecType::AUTO;
    break;
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::HTTP1:
    codec_type_ = CodecType::HTTP1;
    break;
  case envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager::HTTP2:
    codec_type_ = CodecType::HTTP2;
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }

  const auto& filters = config.http_filters();
  for (int32_t i = 0; i < filters.size(); i++) {
    processFilter(filters[i], i, "http", filter_factories_);
  }

  for (auto upgrade_config : config.upgrade_configs()) {
    const std::string& name = upgrade_config.upgrade_type();
    if (findUpgradeCaseInsensitive(upgrade_filter_factories_, name) !=
        upgrade_filter_factories_.end()) {
      throw EnvoyException(
          fmt::format("Error: multiple upgrade configs with the same name: '{}'", name));
    }
    if (upgrade_config.filters().size() > 0) {
      std::unique_ptr<FilterFactoriesList> factories = std::make_unique<FilterFactoriesList>();
      for (int32_t i = 0; i < upgrade_config.filters().size(); i++) {
        processFilter(upgrade_config.filters(i), i, name, *factories);
      }
      upgrade_filter_factories_.emplace(std::make_pair(name, std::move(factories)));
    } else {
      std::unique_ptr<FilterFactoriesList> factories(nullptr);
      upgrade_filter_factories_.emplace(std::make_pair(name, std::move(factories)));
    }
  }
}

void HttpConnectionManagerConfig::processFilter(
    const envoy::config::filter::network::http_connection_manager::v2::HttpFilter& proto_config,
    int i, absl::string_view prefix, std::list<Http::FilterFactoryCb>& filter_factories) {
  const ProtobufTypes::String& string_name = proto_config.name();

  ENVOY_LOG(debug, "    {} filter #{}", prefix, i);
  ENVOY_LOG(debug, "      name: {}", string_name);

  const Json::ObjectSharedPtr filter_config =
      MessageUtil::getJsonObjectFromMessage(proto_config.config());
  ENVOY_LOG(debug, "    config: {}", filter_config->asJsonString());

  // Now see if there is a factory that will accept the config.
  auto& factory =
      Config::Utility::getAndCheckFactory<Server::Configuration::NamedHttpFilterConfigFactory>(
          string_name);
  Http::FilterFactoryCb callback;
  if (filter_config->getBoolean("deprecated_v1", false)) {
    callback = factory.createFilterFactory(*filter_config->getObject("value", true), stats_prefix_,
                                           context_);
  } else {
    ProtobufTypes::MessagePtr message =
        Config::Utility::translateToFactoryConfig(proto_config, factory);
    callback = factory.createFilterFactoryFromProto(*message, stats_prefix_, context_);
  }
  filter_factories.push_back(callback);
}

Http::ServerConnectionPtr
HttpConnectionManagerConfig::createCodec(Network::Connection& connection,
                                         const Buffer::Instance& data,
                                         Http::ServerConnectionCallbacks& callbacks) {
  switch (codec_type_) {
  case CodecType::HTTP1:
    return Http::ServerConnectionPtr{
        new Http::Http1::ServerConnectionImpl(connection, callbacks, http1_settings_)};
  case CodecType::HTTP2:
    return Http::ServerConnectionPtr{new Http::Http2::ServerConnectionImpl(
        connection, callbacks, context_.scope(), http2_settings_)};
  case CodecType::AUTO:
    if (HttpConnectionManagerConfigUtility::determineNextProtocol(connection, data) ==
        Http::Http2::ALPN_STRING) {
      return Http::ServerConnectionPtr{new Http::Http2::ServerConnectionImpl(
          connection, callbacks, context_.scope(), http2_settings_)};
    } else {
      return Http::ServerConnectionPtr{
          new Http::Http1::ServerConnectionImpl(connection, callbacks, http1_settings_)};
    }
  }

  NOT_REACHED_GCOVR_EXCL_LINE;
}

void HttpConnectionManagerConfig::createFilterChain(Http::FilterChainFactoryCallbacks& callbacks) {
  for (const Http::FilterFactoryCb& factory : filter_factories_) {
    factory(callbacks);
  }
}

bool HttpConnectionManagerConfig::createUpgradeFilterChain(
    absl::string_view upgrade_type, Http::FilterChainFactoryCallbacks& callbacks) {
  auto it = findUpgradeCaseInsensitive(upgrade_filter_factories_, upgrade_type);
  if (it != upgrade_filter_factories_.end()) {
    FilterFactoriesList* filters_to_use = nullptr;
    if (it->second != nullptr) {
      filters_to_use = it->second.get();
    } else {
      filters_to_use = &filter_factories_;
    }
    for (const Http::FilterFactoryCb& factory : *filters_to_use) {
      factory(callbacks);
    }
    return true;
  }
  return false;
}

const Network::Address::Instance& HttpConnectionManagerConfig::localAddress() {
  return *context_.localInfo().address();
}

} // namespace HttpConnectionManager
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
