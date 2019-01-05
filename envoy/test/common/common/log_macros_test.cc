#include <iostream>
#include <string>

#include "common/common/logger.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {

class TestFilterLog : public Logger::Loggable<Logger::Id::filter> {
public:
  void logMessage() {
    ENVOY_LOG(trace, "fake message");
    ENVOY_LOG(debug, "fake message");
    ENVOY_LOG(warn, "fake message");
    ENVOY_LOG(error, "fake message");
    ENVOY_LOG(critical, "fake message");
    ENVOY_CONN_LOG(info, "fake message", connection_);
    ENVOY_STREAM_LOG(info, "fake message", stream_);
  }

private:
  NiceMock<Network::MockConnection> connection_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> stream_;
};

TEST(Logger, All) {
  // This test exists just to ensure all macros compile and run with the expected arguments provided

  TestFilterLog filter;
  filter.logMessage();

  // Misc logging with no facility.
  ENVOY_LOG_MISC(info, "fake message");
}

TEST(Logger, evaluateParams) {
  uint32_t i = 1;

  // Set logger's level to low level.
  // Log message with higher severity and make sure that params were evaluated.
  GET_MISC_LOGGER().set_level(spdlog::level::info);
  ENVOY_LOG_MISC(warn, "test message '{}'", i++);
  EXPECT_THAT(i, testing::Eq(2));
}

TEST(Logger, doNotEvaluateParams) {
  uint32_t i = 1;

  // Set logger's logging level high and log a message with lower severity
  // params should not be evaluated.
  GET_MISC_LOGGER().set_level(spdlog::level::critical);
  ENVOY_LOG_MISC(error, "test message '{}'", i++);
  EXPECT_THAT(i, testing::Eq(1));
}

TEST(Logger, logAsStatement) {
  // Just log as part of if ... statement
  uint32_t i = 1, j = 1;

  // Set logger's logging level to high
  GET_MISC_LOGGER().set_level(spdlog::level::critical);

  // Make sure that if statement inside of LOGGER macro does not catch trailing
  // else ....
  if (true)
    ENVOY_LOG_MISC(warn, "test message 1 '{}'", i++);
  else
    ENVOY_LOG_MISC(critical, "test message 2 '{}'", j++);

  EXPECT_THAT(i, testing::Eq(1));
  EXPECT_THAT(j, testing::Eq(1));

  // Do the same with curly brackets
  if (true) {
    ENVOY_LOG_MISC(warn, "test message 3 '{}'", i++);
  } else {
    ENVOY_LOG_MISC(critical, "test message 4 '{}'", j++);
  }

  EXPECT_THAT(i, testing::Eq(1));
  EXPECT_THAT(j, testing::Eq(1));
}

TEST(Logger, checkLoggerLevel) {
  class logTestClass : public Logger::Loggable<Logger::Id::misc> {
  public:
    void setLevel(const spdlog::level::level_enum level) { ENVOY_LOGGER().set_level(level); }
    uint32_t executeAtTraceLevel() {
      if (ENVOY_LOG_CHECK_LEVEL(trace)) {
        //  Logger's level was at least trace
        return 1;
      } else {
        // Logger's level was higher than trace
        return 2;
      };
    }
  };

  logTestClass testObj;

  // Set Loggers severity low
  testObj.setLevel(spdlog::level::trace);
  EXPECT_THAT(testObj.executeAtTraceLevel(), testing::Eq(1));

  testObj.setLevel(spdlog::level::info);
  EXPECT_THAT(testObj.executeAtTraceLevel(), testing::Eq(2));
}

TEST(RegistryTest, LoggerWithName) {
  EXPECT_EQ(nullptr, Logger::Registry::logger("blah"));
  EXPECT_EQ("upstream", Logger::Registry::logger("upstream")->name());
}

} // namespace Envoy
