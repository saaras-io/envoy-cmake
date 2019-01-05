#include "mocks.h"

#include <cstdint>

#include "common/common/assert.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RedisProxy {

void PrintTo(const RespValue& value, std::ostream* os) { *os << value.toString(); }

void PrintTo(const RespValuePtr& value, std::ostream* os) { *os << value->toString(); }

bool operator==(const RespValue& lhs, const RespValue& rhs) {
  if (lhs.type() != rhs.type()) {
    return false;
  }

  switch (lhs.type()) {
  case RespType::Array: {
    if (lhs.asArray().size() != rhs.asArray().size()) {
      return false;
    }

    bool equal = true;
    for (uint64_t i = 0; i < lhs.asArray().size(); i++) {
      equal &= (lhs.asArray()[i] == rhs.asArray()[i]);
    }

    return equal;
  }
  case RespType::SimpleString:
  case RespType::BulkString:
  case RespType::Error: {
    return lhs.asString() == rhs.asString();
  }
  case RespType::Null: {
    return true;
  }
  case RespType::Integer: {
    return lhs.asInteger() == rhs.asInteger();
  }
  }

  NOT_REACHED_GCOVR_EXCL_LINE;
}

MockEncoder::MockEncoder() {
  ON_CALL(*this, encode(_, _))
      .WillByDefault(Invoke([this](const RespValue& value, Buffer::Instance& out) -> void {
        real_encoder_.encode(value, out);
      }));
}

MockEncoder::~MockEncoder() {}

MockDecoder::MockDecoder() {}
MockDecoder::~MockDecoder() {}

namespace ConnPool {

MockClient::MockClient() {
  ON_CALL(*this, addConnectionCallbacks(_))
      .WillByDefault(Invoke([this](Network::ConnectionCallbacks& callbacks) -> void {
        callbacks_.push_back(&callbacks);
      }));
  ON_CALL(*this, close()).WillByDefault(Invoke([this]() -> void {
    raiseEvent(Network::ConnectionEvent::LocalClose);
  }));
}

MockClient::~MockClient() {}

MockPoolRequest::MockPoolRequest() {}
MockPoolRequest::~MockPoolRequest() {}

MockPoolCallbacks::MockPoolCallbacks() {}
MockPoolCallbacks::~MockPoolCallbacks() {}

MockInstance::MockInstance() {}
MockInstance::~MockInstance() {}

} // namespace ConnPool

namespace CommandSplitter {

MockSplitRequest::MockSplitRequest() {}
MockSplitRequest::~MockSplitRequest() {}

MockSplitCallbacks::MockSplitCallbacks() {}
MockSplitCallbacks::~MockSplitCallbacks() {}

MockInstance::MockInstance() {}
MockInstance::~MockInstance() {}

} // namespace CommandSplitter
} // namespace RedisProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
