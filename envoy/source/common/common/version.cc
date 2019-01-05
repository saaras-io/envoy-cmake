#include "common/common/version.h"

#include <string>

#include "common/common/fmt.h"
#include "common/common/macros.h"

extern const char build_scm_revision2[];
extern const char build_scm_status2[];

namespace Envoy {
const std::string& VersionInfo::revision() {
  CONSTRUCT_ON_FIRST_USE(std::string, build_scm_revision2);
}
const std::string& VersionInfo::revisionStatus() {
  CONSTRUCT_ON_FIRST_USE(std::string, build_scm_status2);
}

std::string VersionInfo::version() {
  return fmt::format("{}/{}/{}/{}", revision(), BUILD_VERSION_NUMBER, revisionStatus(),
#ifdef NDEBUG
                     "RELEASE"
#else
                     "DEBUG"
#endif
  );
}
} // namespace Envoy
