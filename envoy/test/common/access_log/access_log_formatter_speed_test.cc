#if 0
#include "common/access_log/access_log_formatter.h"
#include "common/network/address_impl.h"

#include "test/common/access_log/test_util.h"
#include "test/mocks/http/mocks.h"

#include "testing/base/public/benchmark.h"

namespace {

static std::unique_ptr<Envoy::AccessLog::FormatterImpl> formatter;
static std::unique_ptr<Envoy::TestRequestInfo> request_info;

} // namespace

namespace Envoy {

static void BM_AccessLogFormatter(benchmark::State& state) {
  size_t output_bytes = 0;
  Http::TestHeaderMapImpl request_headers;
  Http::TestHeaderMapImpl response_headers;
  Http::TestHeaderMapImpl response_trailers;
  for (auto _ : state) {
    output_bytes +=
        formatter->format(request_headers, response_headers, response_trailers, *request_info)
            .length();
  }
  benchmark::DoNotOptimize(output_bytes);
}
BENCHMARK(BM_AccessLogFormatter);

} // namespace Envoy

// Boilerplate main(), which discovers benchmarks in the same file and runs them.
int main(int argc, char** argv) {
  static const char* LogFormat =
      "%DOWNSTREAM_REMOTE_ADDRESS_WITHOUT_PORT% %START_TIME(%Y/%m/%dT%H:%M:%S%z %s)% "
      "%REQ(:METHOD)% "
      "%REQ(X-FORWARDED-PROTO)%://%REQ(:AUTHORITY)%%REQ(X-ENVOY-ORIGINAL-PATH?:PATH)% %PROTOCOL% "
      "s%RESPONSE_CODE% %BYTES_SENT% %DURATION% %REQ(REFERER)% \"%REQ(USER-AGENT)%\" - - -\n";

  formatter = std::make_unique<Envoy::AccessLog::FormatterImpl>(LogFormat);
  request_info = std::make_unique<Envoy::TestRequestInfo>();
  request_info->setDownstreamRemoteAddress(
      std::make_shared<Envoy::Network::Address::Ipv4Instance>("203.0.113.1"));
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
}
#endif
