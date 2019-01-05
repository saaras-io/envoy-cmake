#include <cstdint>
#include <string>

#include "common/common/fmt.h"
#include "common/config/protocol_json.h"
#include "common/http/exception.h"
#include "common/http/header_map_impl.h"
#include "common/http/utility.h"
#include "common/network/address_impl.h"

#include "test/mocks/http/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::InvokeWithoutArgs;

namespace Envoy {
namespace Http {

TEST(HttpUtility, parseQueryString) {
  EXPECT_EQ(Utility::QueryParams(), Utility::parseQueryString("/hello"));
  EXPECT_EQ(Utility::QueryParams(), Utility::parseQueryString("/hello?"));
  EXPECT_EQ(Utility::QueryParams({{"hello", ""}}), Utility::parseQueryString("/hello?hello"));
  EXPECT_EQ(Utility::QueryParams({{"hello", "world"}}),
            Utility::parseQueryString("/hello?hello=world"));
  EXPECT_EQ(Utility::QueryParams({{"hello", ""}}), Utility::parseQueryString("/hello?hello="));
  EXPECT_EQ(Utility::QueryParams({{"hello", ""}}), Utility::parseQueryString("/hello?hello=&"));
  EXPECT_EQ(Utility::QueryParams({{"hello", ""}, {"hello2", "world2"}}),
            Utility::parseQueryString("/hello?hello=&hello2=world2"));
  EXPECT_EQ(Utility::QueryParams({{"name", "admin"}, {"level", "trace"}}),
            Utility::parseQueryString("/logging?name=admin&level=trace"));
}

TEST(HttpUtility, getResponseStatus) {
  EXPECT_THROW(Utility::getResponseStatus(TestHeaderMapImpl{}), CodecClientException);
  EXPECT_EQ(200U, Utility::getResponseStatus(TestHeaderMapImpl{{":status", "200"}}));
}

TEST(HttpUtility, isWebSocketUpgradeRequest) {
  EXPECT_FALSE(Utility::isWebSocketUpgradeRequest(TestHeaderMapImpl{}));
  EXPECT_FALSE(Utility::isWebSocketUpgradeRequest(TestHeaderMapImpl{{"connection", "upgrade"}}));
  EXPECT_FALSE(Utility::isWebSocketUpgradeRequest(TestHeaderMapImpl{{"upgrade", "websocket"}}));
  EXPECT_FALSE(Utility::isWebSocketUpgradeRequest(
      TestHeaderMapImpl{{"Connection", "close"}, {"Upgrade", "websocket"}}));
  EXPECT_FALSE(Utility::isUpgrade(
      TestHeaderMapImpl{{"Connection", "IsNotAnUpgrade"}, {"Upgrade", "websocket"}}));

  EXPECT_TRUE(Utility::isWebSocketUpgradeRequest(
      TestHeaderMapImpl{{"Connection", "upgrade"}, {"Upgrade", "websocket"}}));
  EXPECT_TRUE(Utility::isWebSocketUpgradeRequest(
      TestHeaderMapImpl{{"connection", "upgrade"}, {"upgrade", "websocket"}}));
  EXPECT_TRUE(Utility::isWebSocketUpgradeRequest(
      TestHeaderMapImpl{{"connection", "Upgrade"}, {"upgrade", "WebSocket"}}));
}

TEST(HttpUtility, isUpgrade) {
  EXPECT_FALSE(Utility::isUpgrade(TestHeaderMapImpl{}));
  EXPECT_FALSE(Utility::isUpgrade(TestHeaderMapImpl{{"connection", "upgrade"}}));
  EXPECT_FALSE(Utility::isUpgrade(TestHeaderMapImpl{{"upgrade", "foo"}}));
  EXPECT_FALSE(Utility::isUpgrade(TestHeaderMapImpl{{"Connection", "close"}, {"Upgrade", "foo"}}));
  EXPECT_FALSE(
      Utility::isUpgrade(TestHeaderMapImpl{{"Connection", "IsNotAnUpgrade"}, {"Upgrade", "foo"}}));
  EXPECT_FALSE(Utility::isUpgrade(
      TestHeaderMapImpl{{"Connection", "Is Not An Upgrade"}, {"Upgrade", "foo"}}));

  EXPECT_TRUE(Utility::isUpgrade(TestHeaderMapImpl{{"Connection", "upgrade"}, {"Upgrade", "foo"}}));
  EXPECT_TRUE(Utility::isUpgrade(TestHeaderMapImpl{{"connection", "upgrade"}, {"upgrade", "foo"}}));
  EXPECT_TRUE(Utility::isUpgrade(TestHeaderMapImpl{{"connection", "Upgrade"}, {"upgrade", "FoO"}}));
  EXPECT_TRUE(Utility::isUpgrade(
      TestHeaderMapImpl{{"connection", "keep-alive, Upgrade"}, {"upgrade", "FOO"}}));
}

// Start with H1 style websocket request headers. Transform to H2 and back.
TEST(HttpUtility, H1H2H1Request) {
  TestHeaderMapImpl converted_headers = {
      {":method", "GET"}, {"content-length", "0"}, {"Upgrade", "foo"}, {"Connection", "upgrade"}};
  const TestHeaderMapImpl original_headers(converted_headers);

  ASSERT_TRUE(Utility::isUpgrade(converted_headers));
  ASSERT_FALSE(Utility::isH2UpgradeRequest(converted_headers));
  Utility::transformUpgradeRequestFromH1toH2(converted_headers);

  ASSERT_FALSE(Utility::isUpgrade(converted_headers));
  ASSERT_TRUE(Utility::isH2UpgradeRequest(converted_headers));
  Utility::transformUpgradeRequestFromH2toH1(converted_headers);

  ASSERT_TRUE(Utility::isUpgrade(converted_headers));
  ASSERT_FALSE(Utility::isH2UpgradeRequest(converted_headers));
  ASSERT_EQ(converted_headers, original_headers);
}

// Start with H2 style websocket request headers. Transform to H1 and back.
TEST(HttpUtility, H2H1H2Request) {
  TestHeaderMapImpl converted_headers = {
      {":method", "CONNECT"}, {"content-length", "0"}, {":protocol", "websocket"}};
  const TestHeaderMapImpl original_headers(converted_headers);

  ASSERT_FALSE(Utility::isUpgrade(converted_headers));
  ASSERT_TRUE(Utility::isH2UpgradeRequest(converted_headers));
  Utility::transformUpgradeRequestFromH2toH1(converted_headers);

  ASSERT_TRUE(Utility::isUpgrade(converted_headers));
  ASSERT_FALSE(Utility::isH2UpgradeRequest(converted_headers));
  Utility::transformUpgradeRequestFromH1toH2(converted_headers);

  ASSERT_FALSE(Utility::isUpgrade(converted_headers));
  ASSERT_TRUE(Utility::isH2UpgradeRequest(converted_headers));
  ASSERT_EQ(converted_headers, original_headers);
}

// Start with H1 style websocket response headers. Transform to H2 and back.
TEST(HttpUtility, H1H2H1Response) {
  TestHeaderMapImpl converted_headers = {{":status", "101"},
                                         {"content-length", "0"},
                                         {"upgrade", "websocket"},
                                         {"connection", "upgrade"}};
  const TestHeaderMapImpl original_headers(converted_headers);

  ASSERT_TRUE(Utility::isUpgrade(converted_headers));
  Utility::transformUpgradeResponseFromH1toH2(converted_headers);

  ASSERT_FALSE(Utility::isUpgrade(converted_headers));
  Utility::transformUpgradeResponseFromH2toH1(converted_headers, "websocket");

  ASSERT_TRUE(Utility::isUpgrade(converted_headers));
  ASSERT_EQ(converted_headers, original_headers);
}

// Users of the transformation functions should not expect the results to be
// identical. Because the headers are always added in a set order, the original
// header order may not be preserved.
TEST(HttpUtility, OrderNotPreserved) {
  TestHeaderMapImpl expected_headers = {
      {":method", "GET"}, {"content-length", "0"}, {"Upgrade", "foo"}, {"Connection", "upgrade"}};

  TestHeaderMapImpl converted_headers = {
      {":method", "GET"}, {"content-length", "0"}, {"Connection", "upgrade"}, {"Upgrade", "foo"}};

  Utility::transformUpgradeRequestFromH1toH2(converted_headers);
  Utility::transformUpgradeRequestFromH2toH1(converted_headers);
  EXPECT_EQ(converted_headers, expected_headers);
}

// A more serious problem with using WebSocket help for general Upgrades, is that method for
// WebSocket is always GET but the method for other upgrades is allowed to be a
// POST. This is a documented weakness in Envoy docs and can be addressed with
// a custom x-envoy-original-method header if it is ever needed.
TEST(HttpUtility, MethodNotPreserved) {
  TestHeaderMapImpl expected_headers = {
      {":method", "GET"}, {"content-length", "0"}, {"Upgrade", "foo"}, {"Connection", "upgrade"}};

  TestHeaderMapImpl converted_headers = {
      {":method", "POST"}, {"content-length", "0"}, {"Upgrade", "foo"}, {"Connection", "upgrade"}};

  Utility::transformUpgradeRequestFromH1toH2(converted_headers);
  Utility::transformUpgradeRequestFromH2toH1(converted_headers);
  EXPECT_EQ(converted_headers, expected_headers);
}

TEST(HttpUtility, appendXff) {
  {
    TestHeaderMapImpl headers;
    Network::Address::Ipv4Instance address("127.0.0.1");
    Utility::appendXff(headers, address);
    EXPECT_EQ("127.0.0.1", headers.get_("x-forwarded-for"));
  }

  {
    TestHeaderMapImpl headers{{"x-forwarded-for", "10.0.0.1"}};
    Network::Address::Ipv4Instance address("127.0.0.1");
    Utility::appendXff(headers, address);
    EXPECT_EQ("10.0.0.1, 127.0.0.1", headers.get_("x-forwarded-for"));
  }

  {
    TestHeaderMapImpl headers{{"x-forwarded-for", "10.0.0.1"}};
    Network::Address::PipeInstance address("/foo");
    Utility::appendXff(headers, address);
    EXPECT_EQ("10.0.0.1", headers.get_("x-forwarded-for"));
  }
}

TEST(HttpUtility, appendVia) {
  {
    TestHeaderMapImpl headers;
    Utility::appendVia(headers, "foo");
    EXPECT_EQ("foo", headers.get_("via"));
  }

  {
    TestHeaderMapImpl headers{{"via", "foo"}};
    Utility::appendVia(headers, "bar");
    EXPECT_EQ("foo, bar", headers.get_("via"));
  }
}

TEST(HttpUtility, createSslRedirectPath) {
  {
    TestHeaderMapImpl headers{{":authority", "www.lyft.com"}, {":path", "/hello"}};
    EXPECT_EQ("https://www.lyft.com/hello", Utility::createSslRedirectPath(headers));
  }
}

namespace {

Http2Settings parseHttp2SettingsFromJson(const std::string& json_string) {
  envoy::api::v2::core::Http2ProtocolOptions http2_protocol_options;
  auto json_object_ptr = Json::Factory::loadFromString(json_string);
  Config::ProtocolJson::translateHttp2ProtocolOptions(
      *json_object_ptr->getObject("http2_settings", true), http2_protocol_options);
  return Utility::parseHttp2Settings(http2_protocol_options);
}

} // namespace

TEST(HttpUtility, parseHttp2Settings) {
  {
    auto http2_settings = parseHttp2SettingsFromJson("{}");
    EXPECT_EQ(Http2Settings::DEFAULT_HPACK_TABLE_SIZE, http2_settings.hpack_table_size_);
    EXPECT_EQ(Http2Settings::DEFAULT_MAX_CONCURRENT_STREAMS,
              http2_settings.max_concurrent_streams_);
    EXPECT_EQ(Http2Settings::DEFAULT_INITIAL_STREAM_WINDOW_SIZE,
              http2_settings.initial_stream_window_size_);
    EXPECT_EQ(Http2Settings::DEFAULT_INITIAL_CONNECTION_WINDOW_SIZE,
              http2_settings.initial_connection_window_size_);
  }

  {
    auto http2_settings = parseHttp2SettingsFromJson(R"raw({
                                          "http2_settings" : {
                                            "hpack_table_size": 1,
                                            "max_concurrent_streams": 2,
                                            "initial_stream_window_size": 3,
                                            "initial_connection_window_size": 4
                                          }
                                        })raw");
    EXPECT_EQ(1U, http2_settings.hpack_table_size_);
    EXPECT_EQ(2U, http2_settings.max_concurrent_streams_);
    EXPECT_EQ(3U, http2_settings.initial_stream_window_size_);
    EXPECT_EQ(4U, http2_settings.initial_connection_window_size_);
  }
}

TEST(HttpUtility, getLastAddressFromXFF) {
  {
    const std::string first_address = "192.0.2.10";
    const std::string second_address = "192.0.2.1";
    const std::string third_address = "10.0.0.1";
    TestHeaderMapImpl request_headers{{"x-forwarded-for", "192.0.2.10, 192.0.2.1, 10.0.0.1"}};
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(third_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);
    ret = Utility::getLastAddressFromXFF(request_headers, 1);
    EXPECT_EQ(second_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);
    ret = Utility::getLastAddressFromXFF(request_headers, 2);
    EXPECT_EQ(first_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);
    ret = Utility::getLastAddressFromXFF(request_headers, 3);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    const std::string first_address = "192.0.2.10";
    const std::string second_address = "192.0.2.1";
    const std::string third_address = "10.0.0.1";
    const std::string fourth_address = "10.0.0.2";
    TestHeaderMapImpl request_headers{
        {"x-forwarded-for", "192.0.2.10, 192.0.2.1 ,10.0.0.1,10.0.0.2"}};

    // No space on the left.
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(fourth_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);

    // No space on either side.
    ret = Utility::getLastAddressFromXFF(request_headers, 1);
    EXPECT_EQ(third_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);

    // Exercise rtrim() and ltrim().
    ret = Utility::getLastAddressFromXFF(request_headers, 2);
    EXPECT_EQ(second_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);

    // No space trimming.
    ret = Utility::getLastAddressFromXFF(request_headers, 3);
    EXPECT_EQ(first_address, ret.address_->ip()->addressAsString());
    EXPECT_FALSE(ret.single_address_);

    // No address found.
    ret = Utility::getLastAddressFromXFF(request_headers, 4);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    TestHeaderMapImpl request_headers{{"x-forwarded-for", ""}};
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    TestHeaderMapImpl request_headers{{"x-forwarded-for", ","}};
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    TestHeaderMapImpl request_headers{{"x-forwarded-for", ", "}};
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    TestHeaderMapImpl request_headers{{"x-forwarded-for", ", bad"}};
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    TestHeaderMapImpl request_headers;
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(nullptr, ret.address_);
    EXPECT_FALSE(ret.single_address_);
  }
  {
    const std::string first_address = "34.0.0.1";
    TestHeaderMapImpl request_headers{{"x-forwarded-for", first_address}};
    auto ret = Utility::getLastAddressFromXFF(request_headers);
    EXPECT_EQ(first_address, ret.address_->ip()->addressAsString());
    EXPECT_TRUE(ret.single_address_);
  }
}

TEST(HttpUtility, TestParseCookie) {
  TestHeaderMapImpl headers{
      {"someheader", "10.0.0.1"},
      {"cookie", "somekey=somevalue; someotherkey=someothervalue"},
      {"cookie", "abc=def; token=abc123; Expires=Wed, 09 Jun 2021 10:18:14 GMT"},
      {"cookie", "key2=value2; key3=value3"}};

  std::string key{"token"};
  std::string value = Utility::parseCookieValue(headers, key);
  EXPECT_EQ(value, "abc123");
}

TEST(HttpUtility, TestParseCookieBadValues) {
  TestHeaderMapImpl headers{{"cookie", "token1=abc123; = "},
                            {"cookie", "token2=abc123;   "},
                            {"cookie", "; token3=abc123;"},
                            {"cookie", "=; token4=\"abc123\""}};

  EXPECT_EQ(Utility::parseCookieValue(headers, "token1"), "abc123");
  EXPECT_EQ(Utility::parseCookieValue(headers, "token2"), "abc123");
  EXPECT_EQ(Utility::parseCookieValue(headers, "token3"), "abc123");
  EXPECT_EQ(Utility::parseCookieValue(headers, "token4"), "abc123");
}

TEST(HttpUtility, TestParseCookieWithQuotes) {
  TestHeaderMapImpl headers{
      {"someheader", "10.0.0.1"},
      {"cookie", "dquote=\"; quoteddquote=\"\"\""},
      {"cookie", "leadingdquote=\"foobar;"},
      {"cookie", "abc=def; token=\"abc123\"; Expires=Wed, 09 Jun 2021 10:18:14 GMT"}};

  EXPECT_EQ(Utility::parseCookieValue(headers, "token"), "abc123");
  EXPECT_EQ(Utility::parseCookieValue(headers, "dquote"), "\"");
  EXPECT_EQ(Utility::parseCookieValue(headers, "quoteddquote"), "\"");
  EXPECT_EQ(Utility::parseCookieValue(headers, "leadingdquote"), "\"foobar");
}

TEST(HttpUtility, TestHasSetCookie) {
  TestHeaderMapImpl headers{{"someheader", "10.0.0.1"},
                            {"set-cookie", "somekey=somevalue"},
                            {"set-cookie", "abc=def; Expires=Wed, 09 Jun 2021 10:18:14 GMT"},
                            {"set-cookie", "key2=value2; Secure"}};

  EXPECT_TRUE(Utility::hasSetCookie(headers, "abc"));
  EXPECT_TRUE(Utility::hasSetCookie(headers, "somekey"));
  EXPECT_FALSE(Utility::hasSetCookie(headers, "ghi"));
}

TEST(HttpUtility, TestHasSetCookieBadValues) {
  TestHeaderMapImpl headers{{"someheader", "10.0.0.1"},
                            {"set-cookie", "somekey =somevalue"},
                            {"set-cookie", "abc"},
                            {"set-cookie", "key2=value2; Secure"}};

  EXPECT_FALSE(Utility::hasSetCookie(headers, "abc"));
  EXPECT_TRUE(Utility::hasSetCookie(headers, "key2"));
}

TEST(HttpUtility, TestMakeSetCookieValue) {
  EXPECT_EQ("name=\"value\"; Max-Age=10",
            Utility::makeSetCookieValue("name", "value", "", std::chrono::seconds(10), false));
  EXPECT_EQ("name=\"value\"",
            Utility::makeSetCookieValue("name", "value", "", std::chrono::seconds::zero(), false));
  EXPECT_EQ("name=\"value\"; Max-Age=10; HttpOnly",
            Utility::makeSetCookieValue("name", "value", "", std::chrono::seconds(10), true));
  EXPECT_EQ("name=\"value\"; HttpOnly",
            Utility::makeSetCookieValue("name", "value", "", std::chrono::seconds::zero(), true));

  EXPECT_EQ("name=\"value\"; Max-Age=10; Path=/",
            Utility::makeSetCookieValue("name", "value", "/", std::chrono::seconds(10), false));
  EXPECT_EQ("name=\"value\"; Path=/",
            Utility::makeSetCookieValue("name", "value", "/", std::chrono::seconds::zero(), false));
  EXPECT_EQ("name=\"value\"; Max-Age=10; Path=/; HttpOnly",
            Utility::makeSetCookieValue("name", "value", "/", std::chrono::seconds(10), true));
  EXPECT_EQ("name=\"value\"; Path=/; HttpOnly",
            Utility::makeSetCookieValue("name", "value", "/", std::chrono::seconds::zero(), true));
}

TEST(HttpUtility, SendLocalReply) {
  MockStreamDecoderFilterCallbacks callbacks;
  bool is_reset = false;

  EXPECT_CALL(callbacks, encodeHeaders_(_, false));
  EXPECT_CALL(callbacks, encodeData(_, true));
  Utility::sendLocalReply(false, callbacks, is_reset, Http::Code::PayloadTooLarge, "large", false);
}

TEST(HttpUtility, SendLocalGrpcReply) {
  MockStreamDecoderFilterCallbacks callbacks;
  bool is_reset = false;

  EXPECT_CALL(callbacks, encodeHeaders_(_, true))
      .WillOnce(Invoke([&](const HeaderMap& headers, bool) -> void {
        EXPECT_STREQ(headers.Status()->value().c_str(), "200");
        EXPECT_NE(headers.GrpcStatus(), nullptr);
        EXPECT_STREQ(headers.GrpcStatus()->value().c_str(), "2"); // Unknown gRPC error.
        EXPECT_NE(headers.GrpcMessage(), nullptr);
        EXPECT_STREQ(headers.GrpcMessage()->value().c_str(), "large");
      }));
  Utility::sendLocalReply(true, callbacks, is_reset, Http::Code::PayloadTooLarge, "large", false);
}

TEST(HttpUtility, SendLocalReplyDestroyedEarly) {
  MockStreamDecoderFilterCallbacks callbacks;
  bool is_reset = false;

  EXPECT_CALL(callbacks, encodeHeaders_(_, false)).WillOnce(InvokeWithoutArgs([&]() -> void {
    is_reset = true;
  }));
  EXPECT_CALL(callbacks, encodeData(_, true)).Times(0);
  Utility::sendLocalReply(false, callbacks, is_reset, Http::Code::PayloadTooLarge, "large", false);
}

TEST(HttpUtility, SendLocalReplyHeadRequest) {
  MockStreamDecoderFilterCallbacks callbacks;
  bool is_reset = false;
  EXPECT_CALL(callbacks, encodeHeaders_(_, true))
      .WillOnce(Invoke([&](const HeaderMap& headers, bool) -> void {
        EXPECT_STREQ(headers.ContentLength()->value().c_str(),
                     fmt::format("{}", strlen("large")).c_str());
      }));
  Utility::sendLocalReply(false, callbacks, is_reset, Http::Code::PayloadTooLarge, "large", true);
}

TEST(HttpUtility, TestExtractHostPathFromUri) {
  absl::string_view host, path;

  // FQDN
  Utility::extractHostPathFromUri("scheme://dns.name/x/y/z", host, path);
  EXPECT_EQ(host, "dns.name");
  EXPECT_EQ(path, "/x/y/z");

  // Just the host part
  Utility::extractHostPathFromUri("dns.name", host, path);
  EXPECT_EQ(host, "dns.name");
  EXPECT_EQ(path, "/");

  // Just host and path
  Utility::extractHostPathFromUri("dns.name/x/y/z", host, path);
  EXPECT_EQ(host, "dns.name");
  EXPECT_EQ(path, "/x/y/z");

  // Just the path
  Utility::extractHostPathFromUri("/x/y/z", host, path);
  EXPECT_EQ(host, "");
  EXPECT_EQ(path, "/x/y/z");

  // Some invalid URI
  Utility::extractHostPathFromUri("scheme://adf-scheme://adf", host, path);
  EXPECT_EQ(host, "adf-scheme:");
  EXPECT_EQ(path, "//adf");

  Utility::extractHostPathFromUri("://", host, path);
  EXPECT_EQ(host, "");
  EXPECT_EQ(path, "/");

  Utility::extractHostPathFromUri("/:/adsf", host, path);
  EXPECT_EQ(host, "");
  EXPECT_EQ(path, "/:/adsf");
}

TEST(HttpUtility, TestPrepareHeaders) {
  envoy::api::v2::core::HttpUri http_uri;
  http_uri.set_uri("scheme://dns.name/x/y/z");

  Http::MessagePtr message = Utility::prepareHeaders(http_uri);

  EXPECT_STREQ("/x/y/z", message->headers().Path()->value().c_str());
  EXPECT_STREQ("dns.name", message->headers().Host()->value().c_str());
}

TEST(HttpUtility, QueryParamsToString) {
  EXPECT_EQ("", Utility::queryParamsToString(Utility::QueryParams({})));
  EXPECT_EQ("?a=1", Utility::queryParamsToString(Utility::QueryParams({{"a", "1"}})));
  EXPECT_EQ("?a=1&b=2",
            Utility::queryParamsToString(Utility::QueryParams({{"a", "1"}, {"b", "2"}})));
}

} // namespace Http
} // namespace Envoy
