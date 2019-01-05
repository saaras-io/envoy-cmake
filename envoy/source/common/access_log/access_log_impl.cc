#include "common/access_log/access_log_impl.h"

#include <cstdint>
#include <string>

#include "envoy/common/time.h"
#include "envoy/config/filter/accesslog/v2/accesslog.pb.validate.h"
#include "envoy/filesystem/filesystem.h"
#include "envoy/http/header_map.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/upstream.h"

#include "common/access_log/access_log_formatter.h"
#include "common/common/assert.h"
#include "common/common/utility.h"
#include "common/config/utility.h"
#include "common/http/header_map_impl.h"
#include "common/http/header_utility.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "common/protobuf/utility.h"
#include "common/request_info/utility.h"
#include "common/runtime/uuid_util.h"
#include "common/tracing/http_tracer_impl.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace AccessLog {

ComparisonFilter::ComparisonFilter(
    const envoy::config::filter::accesslog::v2::ComparisonFilter& config, Runtime::Loader& runtime)
    : config_(config), runtime_(runtime) {}

bool ComparisonFilter::compareAgainstValue(uint64_t lhs) {
  uint64_t value = config_.value().default_value();

  if (!config_.value().runtime_key().empty()) {
    value = runtime_.snapshot().getInteger(config_.value().runtime_key(), value);
  }

  switch (config_.op()) {
  case envoy::config::filter::accesslog::v2::ComparisonFilter::GE:
    return lhs >= value;
  case envoy::config::filter::accesslog::v2::ComparisonFilter::EQ:
    return lhs == value;
  case envoy::config::filter::accesslog::v2::ComparisonFilter::LE:
    return lhs <= value;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

FilterPtr
FilterFactory::fromProto(const envoy::config::filter::accesslog::v2::AccessLogFilter& config,
                         Runtime::Loader& runtime, Runtime::RandomGenerator& random) {
  switch (config.filter_specifier_case()) {
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kStatusCodeFilter:
    return FilterPtr{new StatusCodeFilter(config.status_code_filter(), runtime)};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kDurationFilter:
    return FilterPtr{new DurationFilter(config.duration_filter(), runtime)};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kNotHealthCheckFilter:
    return FilterPtr{new NotHealthCheckFilter()};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kTraceableFilter:
    return FilterPtr{new TraceableRequestFilter()};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kRuntimeFilter:
    return FilterPtr{new RuntimeFilter(config.runtime_filter(), runtime, random)};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kAndFilter:
    return FilterPtr{new AndFilter(config.and_filter(), runtime, random)};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kOrFilter:
    return FilterPtr{new OrFilter(config.or_filter(), runtime, random)};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kHeaderFilter:
    return FilterPtr{new HeaderFilter(config.header_filter())};
  case envoy::config::filter::accesslog::v2::AccessLogFilter::kResponseFlagFilter:
    MessageUtil::validate(config);
    return FilterPtr{new ResponseFlagFilter(config.response_flag_filter())};
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

bool TraceableRequestFilter::evaluate(const RequestInfo::RequestInfo& info,
                                      const Http::HeaderMap& request_headers) {
  Tracing::Decision decision = Tracing::HttpTracerUtility::isTracing(info, request_headers);

  return decision.traced && decision.reason == Tracing::Reason::ServiceForced;
}

bool StatusCodeFilter::evaluate(const RequestInfo::RequestInfo& info, const Http::HeaderMap&) {
  if (!info.responseCode()) {
    return compareAgainstValue(0ULL);
  }

  return compareAgainstValue(info.responseCode().value());
}

bool DurationFilter::evaluate(const RequestInfo::RequestInfo& info, const Http::HeaderMap&) {
  absl::optional<std::chrono::nanoseconds> final = info.requestComplete();
  ASSERT(final);

  return compareAgainstValue(
      std::chrono::duration_cast<std::chrono::milliseconds>(final.value()).count());
}

RuntimeFilter::RuntimeFilter(const envoy::config::filter::accesslog::v2::RuntimeFilter& config,
                             Runtime::Loader& runtime, Runtime::RandomGenerator& random)
    : runtime_(runtime), random_(random), runtime_key_(config.runtime_key()),
      percent_(config.percent_sampled()),
      use_independent_randomness_(config.use_independent_randomness()) {}

bool RuntimeFilter::evaluate(const RequestInfo::RequestInfo&,
                             const Http::HeaderMap& request_header) {
  const Http::HeaderEntry* uuid = request_header.RequestId();
  uint64_t random_value;
  if (use_independent_randomness_ || uuid == nullptr ||
      !UuidUtils::uuidModBy(
          uuid->value().c_str(), random_value,
          ProtobufPercentHelper::fractionalPercentDenominatorToInt(percent_.denominator()))) {
    random_value = random_.random();
  }

  return runtime_.snapshot().featureEnabled(
      runtime_key_, percent_.numerator(), random_value,
      ProtobufPercentHelper::fractionalPercentDenominatorToInt(percent_.denominator()));
}

OperatorFilter::OperatorFilter(const Protobuf::RepeatedPtrField<
                                   envoy::config::filter::accesslog::v2::AccessLogFilter>& configs,
                               Runtime::Loader& runtime, Runtime::RandomGenerator& random) {
  for (const auto& config : configs) {
    filters_.emplace_back(FilterFactory::fromProto(config, runtime, random));
  }
}

OrFilter::OrFilter(const envoy::config::filter::accesslog::v2::OrFilter& config,
                   Runtime::Loader& runtime, Runtime::RandomGenerator& random)
    : OperatorFilter(config.filters(), runtime, random) {}

AndFilter::AndFilter(const envoy::config::filter::accesslog::v2::AndFilter& config,
                     Runtime::Loader& runtime, Runtime::RandomGenerator& random)
    : OperatorFilter(config.filters(), runtime, random) {}

bool OrFilter::evaluate(const RequestInfo::RequestInfo& info,
                        const Http::HeaderMap& request_headers) {
  bool result = false;
  for (auto& filter : filters_) {
    result |= filter->evaluate(info, request_headers);

    if (result) {
      break;
    }
  }

  return result;
}

bool AndFilter::evaluate(const RequestInfo::RequestInfo& info,
                         const Http::HeaderMap& request_headers) {
  bool result = true;
  for (auto& filter : filters_) {
    result &= filter->evaluate(info, request_headers);

    if (!result) {
      break;
    }
  }

  return result;
}

bool NotHealthCheckFilter::evaluate(const RequestInfo::RequestInfo& info, const Http::HeaderMap&) {
  return !info.healthCheck();
}

HeaderFilter::HeaderFilter(const envoy::config::filter::accesslog::v2::HeaderFilter& config) {
  header_data_.push_back(Http::HeaderUtility::HeaderData(config.header()));
}

bool HeaderFilter::evaluate(const RequestInfo::RequestInfo&,
                            const Http::HeaderMap& request_headers) {
  return Http::HeaderUtility::matchHeaders(request_headers, header_data_);
}

ResponseFlagFilter::ResponseFlagFilter(
    const envoy::config::filter::accesslog::v2::ResponseFlagFilter& config) {
  for (int i = 0; i < config.flags_size(); i++) {
    absl::optional<RequestInfo::ResponseFlag> response_flag =
        RequestInfo::ResponseFlagUtils::toResponseFlag(config.flags(i));
    // The config has been validated. Therefore, every flag in the config will have a mapping.
    ASSERT(response_flag.has_value());
    configured_flags_ |= response_flag.value();
  }
}

bool ResponseFlagFilter::evaluate(const RequestInfo::RequestInfo& info, const Http::HeaderMap&) {
  if (configured_flags_ != 0) {
    return info.intersectResponseFlags(configured_flags_);
  }
  return info.hasAnyResponseFlag();
}

InstanceSharedPtr
AccessLogFactory::fromProto(const envoy::config::filter::accesslog::v2::AccessLog& config,
                            Server::Configuration::FactoryContext& context) {
  FilterPtr filter;
  if (config.has_filter()) {
    filter = FilterFactory::fromProto(config.filter(), context.runtime(), context.random());
  }

  auto& factory =
      Config::Utility::getAndCheckFactory<Server::Configuration::AccessLogInstanceFactory>(
          config.name());
  ProtobufTypes::MessagePtr message = Config::Utility::translateToFactoryConfig(config, factory);

  return factory.createAccessLogInstance(*message, std::move(filter), context);
}

} // namespace AccessLog
} // namespace Envoy
