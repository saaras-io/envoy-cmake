#pragma once

#include "envoy/server/filter_config.h"

#include "extensions/filters/http/common/empty_http_filter_config.h"
#include "extensions/filters/http/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Dynamo {

/**
 * Config registration for http dynamodb filter.
 */
class DynamoFilterConfig : public Common::EmptyHttpFilterConfig {
public:
  DynamoFilterConfig() : Common::EmptyHttpFilterConfig(HttpFilterNames::get().Dynamo) {}

  Http::FilterFactoryCb createFilter(const std::string& stat_prefix,
                                     Server::Configuration::FactoryContext& context) override;
};

} // namespace Dynamo
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
