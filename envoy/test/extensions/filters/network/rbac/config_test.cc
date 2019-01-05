#include "envoy/config/filter/network/rbac/v2/rbac.pb.validate.h"

#include "extensions/filters/network/rbac/config.h"

#include "test/mocks/server/mocks.h"

#include "fmt/printf.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RBACFilter {

namespace {
const std::string header = R"EOF(
{ "header": {"name": "key", "exact_match": "value"} }
)EOF";

const std::string metadata = R"EOF(
{
  "metadata": {
    "filter": "t", "path": [ { "key": "a" } ], "value": { "string_match": { "exact": "x" } }
  }
}
)EOF";
} // namespace

class RoleBasedAccessControlNetworkFilterConfigFactoryTest : public testing::Test {
public:
  void validateRule(const std::string& policy_json) {
    checkRule(fmt::sprintf(policy_json, header));
    checkRule(fmt::sprintf(policy_json, metadata));
  }

private:
  void checkRule(const std::string& policy_json) {
    envoy::config::rbac::v2alpha::Policy policy_proto{};
    MessageUtil::loadFromJson(policy_json, policy_proto);

    envoy::config::filter::network::rbac::v2::RBAC config{};
    config.set_stat_prefix("test");
    (*config.mutable_rules()->mutable_policies())["foo"] = policy_proto;

    NiceMock<Server::Configuration::MockFactoryContext> context;
    RoleBasedAccessControlNetworkFilterConfigFactory factory;
    EXPECT_THROW(factory.createFilterFactoryFromProto(config, context), Envoy::EnvoyException);

    config.clear_rules();
    (*config.mutable_shadow_rules()->mutable_policies())["foo"] = policy_proto;
    EXPECT_THROW(factory.createFilterFactoryFromProto(config, context), Envoy::EnvoyException);
  }
};

TEST_F(RoleBasedAccessControlNetworkFilterConfigFactoryTest, ValidProto) {
  envoy::config::rbac::v2alpha::Policy policy;
  policy.add_permissions()->set_any(true);
  policy.add_principals()->set_any(true);
  envoy::config::filter::network::rbac::v2::RBAC config;
  config.set_stat_prefix("stats");
  (*config.mutable_rules()->mutable_policies())["foo"] = policy;

  NiceMock<Server::Configuration::MockFactoryContext> context;
  RoleBasedAccessControlNetworkFilterConfigFactory factory;
  Network::FilterFactoryCb cb = factory.createFilterFactoryFromProto(config, context);
  Network::MockConnection connection;
  EXPECT_CALL(connection, addReadFilter(_));
  cb(connection);
}

TEST_F(RoleBasedAccessControlNetworkFilterConfigFactoryTest, EmptyProto) {
  RoleBasedAccessControlNetworkFilterConfigFactory factory;
  auto* config = dynamic_cast<envoy::config::filter::network::rbac::v2::RBAC*>(
      factory.createEmptyConfigProto().get());
  EXPECT_NE(nullptr, config);
}

TEST_F(RoleBasedAccessControlNetworkFilterConfigFactoryTest, InvalidPermission) {
  validateRule(R"EOF(
{
  "permissions": [ { "any": true }, { "and_rules": { "rules": [ { "any": true }, %s ] } } ],
  "principals": [ { "any": true } ]
}
)EOF");

  validateRule(R"EOF(
{
  "permissions": [ { "any": true }, { "or_rules": { "rules": [ { "any": true }, %s ] } } ],
  "principals": [ { "any": true } ]
}
)EOF");

  validateRule(R"EOF(
{
  "permissions": [ { "any": true }, { "not_rule": %s } ],
  "principals": [ { "any": true } ]
}
)EOF");

  validateRule(R"EOF(
{
  "permissions": [ { "any": true }, %s ],
  "principals": [ { "any": true } ]
}
)EOF");
}

TEST_F(RoleBasedAccessControlNetworkFilterConfigFactoryTest, InvalidPrincipal) {
  validateRule(R"EOF(
{
  "principals": [ { "any": true }, { "and_ids": { "ids": [ { "any": true }, %s ] } } ],
  "permissions": [ { "any": true } ]
}
)EOF");

  validateRule(R"EOF(
{
  "principals": [ { "any": true }, { "or_ids": { "ids": [ { "any": true }, %s ] } } ],
  "permissions": [ { "any": true } ]
}
)EOF");

  validateRule(R"EOF(
{
  "principals": [ { "any": true }, { "not_id": %s } ],
  "permissions": [ { "any": true } ]
}
)EOF");

  validateRule(R"EOF(
{
  "principals": [ { "any": true }, %s ],
  "permissions": [ { "any": true } ]
}
)EOF");
}

} // namespace RBACFilter
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
