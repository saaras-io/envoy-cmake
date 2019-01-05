#pragma once

#include "test/integration/http_integration.h"

#include "gtest/gtest.h"

namespace Envoy {
class Http2UpstreamIntegrationTest : public HttpIntegrationTest,
                                     public testing::TestWithParam<Network::Address::IpVersion> {
public:
  Http2UpstreamIntegrationTest()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP2, GetParam(), realTime()) {}

  void SetUp() override {
    setDownstreamProtocol(Http::CodecClient::Type::HTTP2);
    setUpstreamProtocol(FakeHttpConnection::Type::HTTP2);
  }

  void bidirectionalStreaming(uint32_t bytes);
  void simultaneousRequest(uint32_t request1_bytes, uint32_t request2_bytes,
                           uint32_t response1_bytes, uint32_t response2_bytes);
  void manySimultaneousRequests(uint32_t request_bytes, uint32_t response_bytes);
};
} // namespace Envoy
