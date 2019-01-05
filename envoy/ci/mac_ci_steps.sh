#!/bin/bash

set -e

. "$(dirname "$0")"/setup_gcs_cache.sh

BAZEL_BUILD_OPTIONS="--curses=no --show_task_finish --verbose_failures ${BAZEL_BUILD_EXTRA_OPTIONS}"
# TODO(zuercher): remove --flaky_test_attempts when https://github.com/envoyproxy/envoy/issues/2428
# is resolved.
BAZEL_TEST_OPTIONS="${BAZEL_BUILD_OPTIONS} --test_output=all --flaky_test_attempts=integration@2"

# Build envoy and run tests as separate steps so that failure output
# is somewhat more deterministic (rather than interleaving the build
# and test steps).

bazel build ${BAZEL_BUILD_OPTIONS} //source/...
bazel build ${BAZEL_BUILD_OPTIONS} //test/...
bazel test ${BAZEL_TEST_OPTIONS} //test/...
