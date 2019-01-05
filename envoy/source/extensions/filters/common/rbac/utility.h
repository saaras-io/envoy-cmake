#pragma once

#include "envoy/config/filter/http/rbac/v2/rbac.pb.h"
#include "envoy/config/filter/network/rbac/v2/rbac.pb.h"
#include "envoy/stats/stats_macros.h"

#include "extensions/filters/common/rbac/engine_impl.h"

namespace Envoy {
namespace Extensions {
namespace Filters {
namespace Common {
namespace RBAC {

/**
 * All stats for the RBAC filter. @see stats_macros.h
 */
// clang-format off
#define ALL_RBAC_FILTER_STATS(COUNTER)                                                             \
  COUNTER(allowed)                                                                                 \
  COUNTER(denied)                                                                                  \
  COUNTER(shadow_allowed)                                                                          \
  COUNTER(shadow_denied)
// clang-format on

/**
 * Wrapper struct for RBAC filter stats. @see stats_macros.h
 */
struct RoleBasedAccessControlFilterStats {
  ALL_RBAC_FILTER_STATS(GENERATE_COUNTER_STRUCT)
};

RoleBasedAccessControlFilterStats generateStats(const std::string& prefix, Stats::Scope& scope);

enum class EnforcementMode { Enforced, Shadow };

template <class ConfigType>
absl::optional<RoleBasedAccessControlEngineImpl> createEngine(const ConfigType& config) {
  return config.has_rules() ? absl::make_optional<RoleBasedAccessControlEngineImpl>(config.rules())
                            : absl::nullopt;
}

template <class ConfigType>
absl::optional<RoleBasedAccessControlEngineImpl> createShadowEngine(const ConfigType& config) {
  return config.has_shadow_rules()
             ? absl::make_optional<RoleBasedAccessControlEngineImpl>(config.shadow_rules())
             : absl::nullopt;
}

} // namespace RBAC
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy
