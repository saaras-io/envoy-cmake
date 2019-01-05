#include "common/http/header_map_impl.h"
#include "common/router/header_parser.h"

#include "test/common/router/header_parser_fuzz.pb.validate.h"
#include "test/fuzz/fuzz_runner.h"
#include "test/fuzz/utility.h"

namespace Envoy {
namespace Fuzz {

DEFINE_PROTO_FUZZER(const test::common::router::TestCase& input) {
  try {
    MessageUtil::validate(input);
    Router::HeaderParserPtr parser =
        Router::HeaderParser::configure(input.headers_to_add(), input.headers_to_remove());
    Http::HeaderMapImpl header_map;
    parser->evaluateHeaders(header_map, fromRequestInfo(input.request_info()));
    ENVOY_LOG_MISC(trace, "Success");
  } catch (const EnvoyException& e) {
    ENVOY_LOG_MISC(debug, "EnvoyException: {}", e.what());
  }
}

} // namespace Fuzz
} // namespace Envoy
