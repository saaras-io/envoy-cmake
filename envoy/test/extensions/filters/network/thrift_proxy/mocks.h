#pragma once

#include "envoy/router/router.h"

#include "extensions/filters/network/thrift_proxy/conn_manager.h"
#include "extensions/filters/network/thrift_proxy/conn_state.h"
#include "extensions/filters/network/thrift_proxy/filters/factory_base.h"
#include "extensions/filters/network/thrift_proxy/filters/filter.h"
#include "extensions/filters/network/thrift_proxy/metadata.h"
#include "extensions/filters/network/thrift_proxy/protocol.h"
#include "extensions/filters/network/thrift_proxy/router/router.h"
#include "extensions/filters/network/thrift_proxy/transport.h"

#include "test/mocks/network/mocks.h"
#include "test/test_common/printers.h"

#include "gmock/gmock.h"

using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ThriftProxy {

class MockConfig : public Config {
public:
  MockConfig();
  ~MockConfig();

  // ThriftProxy::Config
  MOCK_METHOD0(filterFactory, ThriftFilters::FilterChainFactory&());
  MOCK_METHOD0(stats, ThriftFilterStats&());
  MOCK_METHOD1(createDecoder, DecoderPtr(DecoderCallbacks&));
  MOCK_METHOD0(routerConfig, Router::Config&());
};

class MockTransport : public Transport {
public:
  MockTransport();
  ~MockTransport();

  // ThriftProxy::Transport
  MOCK_CONST_METHOD0(name, const std::string&());
  MOCK_CONST_METHOD0(type, TransportType());
  MOCK_METHOD2(decodeFrameStart, bool(Buffer::Instance&, MessageMetadata&));
  MOCK_METHOD1(decodeFrameEnd, bool(Buffer::Instance&));
  MOCK_METHOD3(encodeFrame, void(Buffer::Instance&, const MessageMetadata&, Buffer::Instance&));

  std::string name_{"mock"};
  TransportType type_{TransportType::Auto};
};

class MockProtocol : public Protocol {
public:
  MockProtocol();
  ~MockProtocol();

  // ThriftProxy::Protocol
  MOCK_CONST_METHOD0(name, const std::string&());
  MOCK_CONST_METHOD0(type, ProtocolType());
  MOCK_METHOD1(setType, void(ProtocolType));
  MOCK_METHOD2(readMessageBegin, bool(Buffer::Instance& buffer, MessageMetadata& metadata));
  MOCK_METHOD1(readMessageEnd, bool(Buffer::Instance& buffer));
  MOCK_METHOD2(readStructBegin, bool(Buffer::Instance& buffer, std::string& name));
  MOCK_METHOD1(readStructEnd, bool(Buffer::Instance& buffer));
  MOCK_METHOD4(readFieldBegin, bool(Buffer::Instance& buffer, std::string& name,
                                    FieldType& field_type, int16_t& field_id));
  MOCK_METHOD1(readFieldEnd, bool(Buffer::Instance& buffer));
  MOCK_METHOD4(readMapBegin, bool(Buffer::Instance& buffer, FieldType& key_type,
                                  FieldType& value_type, uint32_t& size));
  MOCK_METHOD1(readMapEnd, bool(Buffer::Instance& buffer));
  MOCK_METHOD3(readListBegin, bool(Buffer::Instance& buffer, FieldType& elem_type, uint32_t& size));
  MOCK_METHOD1(readListEnd, bool(Buffer::Instance& buffer));
  MOCK_METHOD3(readSetBegin, bool(Buffer::Instance& buffer, FieldType& elem_type, uint32_t& size));
  MOCK_METHOD1(readSetEnd, bool(Buffer::Instance& buffer));
  MOCK_METHOD2(readBool, bool(Buffer::Instance& buffer, bool& value));
  MOCK_METHOD2(readByte, bool(Buffer::Instance& buffer, uint8_t& value));
  MOCK_METHOD2(readInt16, bool(Buffer::Instance& buffer, int16_t& value));
  MOCK_METHOD2(readInt32, bool(Buffer::Instance& buffer, int32_t& value));
  MOCK_METHOD2(readInt64, bool(Buffer::Instance& buffer, int64_t& value));
  MOCK_METHOD2(readDouble, bool(Buffer::Instance& buffer, double& value));
  MOCK_METHOD2(readString, bool(Buffer::Instance& buffer, std::string& value));
  MOCK_METHOD2(readBinary, bool(Buffer::Instance& buffer, std::string& value));

  MOCK_METHOD2(writeMessageBegin, void(Buffer::Instance& buffer, const MessageMetadata& metadata));
  MOCK_METHOD1(writeMessageEnd, void(Buffer::Instance& buffer));
  MOCK_METHOD2(writeStructBegin, void(Buffer::Instance& buffer, const std::string& name));
  MOCK_METHOD1(writeStructEnd, void(Buffer::Instance& buffer));
  MOCK_METHOD4(writeFieldBegin, void(Buffer::Instance& buffer, const std::string& name,
                                     FieldType field_type, int16_t field_id));
  MOCK_METHOD1(writeFieldEnd, void(Buffer::Instance& buffer));
  MOCK_METHOD4(writeMapBegin, void(Buffer::Instance& buffer, FieldType key_type,
                                   FieldType value_type, uint32_t size));
  MOCK_METHOD1(writeMapEnd, void(Buffer::Instance& buffer));
  MOCK_METHOD3(writeListBegin, void(Buffer::Instance& buffer, FieldType elem_type, uint32_t size));
  MOCK_METHOD1(writeListEnd, void(Buffer::Instance& buffer));
  MOCK_METHOD3(writeSetBegin, void(Buffer::Instance& buffer, FieldType elem_type, uint32_t size));
  MOCK_METHOD1(writeSetEnd, void(Buffer::Instance& buffer));
  MOCK_METHOD2(writeBool, void(Buffer::Instance& buffer, bool value));
  MOCK_METHOD2(writeByte, void(Buffer::Instance& buffer, uint8_t value));
  MOCK_METHOD2(writeInt16, void(Buffer::Instance& buffer, int16_t value));
  MOCK_METHOD2(writeInt32, void(Buffer::Instance& buffer, int32_t value));
  MOCK_METHOD2(writeInt64, void(Buffer::Instance& buffer, int64_t value));
  MOCK_METHOD2(writeDouble, void(Buffer::Instance& buffer, double value));
  MOCK_METHOD2(writeString, void(Buffer::Instance& buffer, const std::string& value));
  MOCK_METHOD2(writeBinary, void(Buffer::Instance& buffer, const std::string& value));
  MOCK_METHOD0(supportsUpgrade, bool());
  MOCK_METHOD0(upgradeRequestDecoder, DecoderEventHandlerSharedPtr());
  MOCK_METHOD1(upgradeResponse, DirectResponsePtr(const DecoderEventHandler&));
  MOCK_METHOD3(attemptUpgrade,
               ThriftObjectPtr(Transport&, ThriftConnectionState&, Buffer::Instance&));
  MOCK_METHOD2(completeUpgrade, void(ThriftConnectionState&, ThriftObject&));

  std::string name_{"mock"};
  ProtocolType type_{ProtocolType::Auto};
};

class MockDecoderCallbacks : public DecoderCallbacks {
public:
  MockDecoderCallbacks();
  ~MockDecoderCallbacks();

  // ThriftProxy::DecoderCallbacks
  MOCK_METHOD0(newDecoderEventHandler, DecoderEventHandler&());
};

class MockDecoderEventHandler : public DecoderEventHandler {
public:
  MockDecoderEventHandler();
  ~MockDecoderEventHandler();

  // ThriftProxy::DecoderEventHandler
  MOCK_METHOD1(transportBegin, FilterStatus(MessageMetadataSharedPtr metadata));
  MOCK_METHOD0(transportEnd, FilterStatus());
  MOCK_METHOD1(messageBegin, FilterStatus(MessageMetadataSharedPtr metadata));
  MOCK_METHOD0(messageEnd, FilterStatus());
  MOCK_METHOD1(structBegin, FilterStatus(const absl::string_view name));
  MOCK_METHOD0(structEnd, FilterStatus());
  MOCK_METHOD3(fieldBegin,
               FilterStatus(const absl::string_view name, FieldType& msg_type, int16_t& field_id));
  MOCK_METHOD0(fieldEnd, FilterStatus());
  MOCK_METHOD1(boolValue, FilterStatus(bool& value));
  MOCK_METHOD1(byteValue, FilterStatus(uint8_t& value));
  MOCK_METHOD1(int16Value, FilterStatus(int16_t& value));
  MOCK_METHOD1(int32Value, FilterStatus(int32_t& value));
  MOCK_METHOD1(int64Value, FilterStatus(int64_t& value));
  MOCK_METHOD1(doubleValue, FilterStatus(double& value));
  MOCK_METHOD1(stringValue, FilterStatus(absl::string_view value));
  MOCK_METHOD3(mapBegin, FilterStatus(FieldType& key_type, FieldType& value_type, uint32_t& size));
  MOCK_METHOD0(mapEnd, FilterStatus());
  MOCK_METHOD2(listBegin, FilterStatus(FieldType& elem_type, uint32_t& size));
  MOCK_METHOD0(listEnd, FilterStatus());
  MOCK_METHOD2(setBegin, FilterStatus(FieldType& elem_type, uint32_t& size));
  MOCK_METHOD0(setEnd, FilterStatus());
};

class MockDirectResponse : public DirectResponse {
public:
  MockDirectResponse();
  ~MockDirectResponse();

  // ThriftProxy::DirectResponse
  MOCK_CONST_METHOD3(encode,
                     DirectResponse::ResponseType(MessageMetadata&, Protocol&, Buffer::Instance&));
};

class MockThriftObject : public ThriftObject {
public:
  MockThriftObject();
  ~MockThriftObject();

  MOCK_CONST_METHOD0(fields, ThriftFieldPtrList&());
  MOCK_METHOD1(onData, bool(Buffer::Instance&));
};

namespace ThriftFilters {

class MockDecoderFilter : public DecoderFilter {
public:
  MockDecoderFilter();
  ~MockDecoderFilter();

  // ThriftProxy::ThriftFilters::DecoderFilter
  MOCK_METHOD0(onDestroy, void());
  MOCK_METHOD1(setDecoderFilterCallbacks, void(DecoderFilterCallbacks& callbacks));
  MOCK_METHOD0(resetUpstreamConnection, void());

  // ThriftProxy::DecoderEventHandler
  MOCK_METHOD1(transportBegin, FilterStatus(MessageMetadataSharedPtr metadata));
  MOCK_METHOD0(transportEnd, FilterStatus());
  MOCK_METHOD1(messageBegin, FilterStatus(MessageMetadataSharedPtr metadata));
  MOCK_METHOD0(messageEnd, FilterStatus());
  MOCK_METHOD1(structBegin, FilterStatus(absl::string_view name));
  MOCK_METHOD0(structEnd, FilterStatus());
  MOCK_METHOD3(fieldBegin,
               FilterStatus(absl::string_view name, FieldType& msg_type, int16_t& field_id));
  MOCK_METHOD0(fieldEnd, FilterStatus());
  MOCK_METHOD1(boolValue, FilterStatus(bool& value));
  MOCK_METHOD1(byteValue, FilterStatus(uint8_t& value));
  MOCK_METHOD1(int16Value, FilterStatus(int16_t& value));
  MOCK_METHOD1(int32Value, FilterStatus(int32_t& value));
  MOCK_METHOD1(int64Value, FilterStatus(int64_t& value));
  MOCK_METHOD1(doubleValue, FilterStatus(double& value));
  MOCK_METHOD1(stringValue, FilterStatus(absl::string_view value));
  MOCK_METHOD3(mapBegin, FilterStatus(FieldType& key_type, FieldType& value_type, uint32_t& size));
  MOCK_METHOD0(mapEnd, FilterStatus());
  MOCK_METHOD2(listBegin, FilterStatus(FieldType& elem_type, uint32_t& size));
  MOCK_METHOD0(listEnd, FilterStatus());
  MOCK_METHOD2(setBegin, FilterStatus(FieldType& elem_type, uint32_t& size));
  MOCK_METHOD0(setEnd, FilterStatus());
};

class MockDecoderFilterCallbacks : public DecoderFilterCallbacks {
public:
  MockDecoderFilterCallbacks();
  ~MockDecoderFilterCallbacks();

  // ThriftProxy::ThriftFilters::DecoderFilterCallbacks
  MOCK_CONST_METHOD0(streamId, uint64_t());
  MOCK_CONST_METHOD0(connection, const Network::Connection*());
  MOCK_METHOD0(continueDecoding, void());
  MOCK_METHOD0(route, Router::RouteConstSharedPtr());
  MOCK_CONST_METHOD0(downstreamTransportType, TransportType());
  MOCK_CONST_METHOD0(downstreamProtocolType, ProtocolType());
  MOCK_METHOD2(sendLocalReply, void(const DirectResponse&, bool));
  MOCK_METHOD2(startUpstreamResponse, void(Transport&, Protocol&));
  MOCK_METHOD1(upstreamData, ResponseStatus(Buffer::Instance&));
  MOCK_METHOD0(resetDownstreamConnection, void());

  uint64_t stream_id_{1};
  NiceMock<Network::MockConnection> connection_;
};

class MockFilterConfigFactory : public ThriftFilters::FactoryBase<ProtobufWkt::Struct> {
public:
  MockFilterConfigFactory();
  ~MockFilterConfigFactory();

  ThriftFilters::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const ProtobufWkt::Struct& proto_config,
                                    const std::string& stat_prefix,
                                    Server::Configuration::FactoryContext& context) override;

  std::shared_ptr<MockDecoderFilter> mock_filter_;
  ProtobufWkt::Struct config_struct_;
  std::string config_stat_prefix_;
};

} // namespace ThriftFilters

namespace Router {

class MockRouteEntry : public RouteEntry {
public:
  MockRouteEntry();
  ~MockRouteEntry();

  // ThriftProxy::Router::RouteEntry
  MOCK_CONST_METHOD0(clusterName, const std::string&());
  MOCK_CONST_METHOD0(metadataMatchCriteria, const Envoy::Router::MetadataMatchCriteria*());
};

class MockRoute : public Route {
public:
  MockRoute();
  ~MockRoute();

  // ThriftProxy::Router::Route
  MOCK_CONST_METHOD0(routeEntry, const RouteEntry*());
};

} // namespace Router
} // namespace ThriftProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
