#include "test/integration/fake_upstream.h"

namespace Envoy {

class AutonomousUpstream;

// A stream which automatically responds when the downstream request is
// completely read. By default the response is 200: OK with 10 bytes of
// payload. This behavior can be overridden with custom request headers defined below.
class AutonomousStream : public FakeStream {
public:
  // The number of response bytes to send. Payload is randomized.
  static const char RESPONSE_SIZE_BYTES[];
  // If set to an integer, the AutonomousStream will expect the response body to
  // be this large.
  static const char EXPECT_REQUEST_SIZE_BYTES[];
  // If set, the stream will reset when the request is complete, rather than
  // sending a response.
  static const char RESET_AFTER_REQUEST[];

  AutonomousStream(FakeHttpConnection& parent, Http::StreamEncoder& encoder,
                   AutonomousUpstream& upstream);
  ~AutonomousStream();

  void setEndStream(bool set) override;

private:
  AutonomousUpstream& upstream_;
  void sendResponse();
};

// An upstream which creates AutonomousStreams for new incoming streams.
class AutonomousHttpConnection : public FakeHttpConnection {
public:
  AutonomousHttpConnection(SharedConnectionWrapper& shared_connection, Stats::Store& store,
                           Type type, AutonomousUpstream& upstream);

  Http::StreamDecoder& newStream(Http::StreamEncoder& response_encoder) override;

private:
  AutonomousUpstream& upstream_;
  std::vector<FakeStreamPtr> streams_;
};

typedef std::unique_ptr<AutonomousHttpConnection> AutonomousHttpConnectionPtr;

// An upstream which creates AutonomousHttpConnection for new incoming connections.
class AutonomousUpstream : public FakeUpstream {
public:
  AutonomousUpstream(uint32_t port, FakeHttpConnection::Type type,
                     Network::Address::IpVersion version, Event::TestTimeSystem& time_system)
      : FakeUpstream(port, type, version, time_system) {}
  ~AutonomousUpstream();
  bool
  createNetworkFilterChain(Network::Connection& connection,
                           const std::vector<Network::FilterFactoryCb>& filter_factories) override;
  bool createListenerFilterChain(Network::ListenerFilterManager& listener) override;

  void setLastRequestHeaders(const Http::HeaderMap& headers);
  std::unique_ptr<Http::TestHeaderMapImpl> lastRequestHeaders();

private:
  Thread::MutexBasicLockable headers_lock_;
  std::unique_ptr<Http::TestHeaderMapImpl> last_request_headers_;
  std::vector<AutonomousHttpConnectionPtr> http_connections_;
  std::vector<SharedConnectionWrapperPtr> shared_connections_;
};

} // namespace Envoy
