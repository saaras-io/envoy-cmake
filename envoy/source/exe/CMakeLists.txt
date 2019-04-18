include(${CMAKE_SOURCE_DIR}/CMakeSettings.txt)
include(${CMAKE_SOURCE_DIR}/CMakeIncludes.txt)

file(GLOB_RECURSE
    sources
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cc
)

set(ev_lib_dir      			${CMAKE_BINARY_DIR}/ev_lib_nodpdk/)
set(ev_external_src_dir         ${CMAKE_SOURCE_DIR}/external/)
set(e_bin_dir      			    ${CMAKE_BINARY_DIR}/e)
set(ev_bin_external_dir      	${CMAKE_BINARY_DIR}/external)

set(rsync_cmd                   rsync -auv)

link_directories(
    ${CMAKE_SOURCE_DIR}/ext-libs
    ${CMAKE_BINARY_DIR}/source/common/access_log
    ${CMAKE_BINARY_DIR}/source/common/api
    ${CMAKE_BINARY_DIR}/source/common/buffer
    ${CMAKE_BINARY_DIR}/source/common/common
    ${CMAKE_BINARY_DIR}/source/common/compressor
    ${CMAKE_BINARY_DIR}/source/common/config
    ${CMAKE_BINARY_DIR}/source/common/decompressor
    ${CMAKE_BINARY_DIR}/source/common/event
    ${CMAKE_BINARY_DIR}/source/common/filesystem
    ${CMAKE_BINARY_DIR}/source/common/grpc
    ${CMAKE_BINARY_DIR}/source/common/html
    ${CMAKE_BINARY_DIR}/source/common/http
    ${CMAKE_BINARY_DIR}/source/common/json
    ${CMAKE_BINARY_DIR}/source/common/memory
    ${CMAKE_BINARY_DIR}/source/common/network
    ${CMAKE_BINARY_DIR}/source/common/profiler
    ${CMAKE_BINARY_DIR}/source/common/protobuf
    ${CMAKE_BINARY_DIR}/source/common/ratelimit
    ${CMAKE_BINARY_DIR}/source/common/request_info
    ${CMAKE_BINARY_DIR}/source/common/request_info
    ${CMAKE_BINARY_DIR}/source/common/router
    ${CMAKE_BINARY_DIR}/source/common/runtime
    ${CMAKE_BINARY_DIR}/source/common/secret
    ${CMAKE_BINARY_DIR}/source/common/singleton
    ${CMAKE_BINARY_DIR}/source/common/ssl
    ${CMAKE_BINARY_DIR}/source/common/stats
    ${CMAKE_BINARY_DIR}/source/common/tcp
    ${CMAKE_BINARY_DIR}/source/common/tcp_proxy
    ${CMAKE_BINARY_DIR}/source/common/thread_local
    ${CMAKE_BINARY_DIR}/source/common/tracing
    ${CMAKE_BINARY_DIR}/source/common/upstream
    ${CMAKE_BINARY_DIR}/source/exe
    ${CMAKE_BINARY_DIR}/source/extensions
    ${CMAKE_BINARY_DIR}/source/server
    ${CMAKE_BINARY_DIR}/source/server/config_validation
    ${CMAKE_BINARY_DIR}/source/server/http
    ${CMAKE_BINARY_DIR}/test/mocks/api
    ${CMAKE_BINARY_DIR}/test/proto
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/algorithm
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/base
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/container
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/debugging
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/memory
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/meta
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/numeric
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/strings
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/synchronization
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/time
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/types
    ${CMAKE_BINARY_DIR}/external/abseil-cpp/absl/utility
    ${CMAKE_BINARY_DIR}/external/backward-cpp
    ${CMAKE_BINARY_DIR}/external/cctz
    ${CMAKE_BINARY_DIR}/external/data-plane-api
    ${CMAKE_BINARY_DIR}/external/fmt-4.1.0/fmt
    ${CMAKE_BINARY_DIR}/external/googletest/googlemock
    ${CMAKE_BINARY_DIR}/external/googletest/googlemock/gtest
    ${CMAKE_BINARY_DIR}/external/grpc
    ${CMAKE_BINARY_DIR}/external/grpc-httpjson-transcoding
    ${CMAKE_BINARY_DIR}/external/grpc/third_party/benchmark/src
    ${CMAKE_BINARY_DIR}/external/grpc/third_party/benchmark/test
    ${CMAKE_BINARY_DIR}/external/grpc/third_party/cares/cares/lib
    ${CMAKE_BINARY_DIR}/external/grpc/third_party/gflags
    ${CMAKE_BINARY_DIR}/external/grpc/third_party/protobuf
    ${CMAKE_BINARY_DIR}/external/grpc/third_party/zlib
    ${CMAKE_BINARY_DIR}/external/http-parser
    ${CMAKE_BINARY_DIR}/external/jwt_verify_lib
    ${CMAKE_BINARY_DIR}/external/libprotobuf_mutator/src
    ${CMAKE_BINARY_DIR}/external/libprotobuf_mutator/src/libfuzzer
    ${CMAKE_BINARY_DIR}/external/lightstep-tracer-cpp
    ${CMAKE_BINARY_DIR}/external/opentracing-cpp
    ${CMAKE_BINARY_DIR}/external/nghttp2-1.30.0/lib
    ${CMAKE_BINARY_DIR}/external/opentracing-cpp/mocktracer
    ${CMAKE_BINARY_DIR}/external/yaml-cpp
    ${CMAKE_SOURCE_DIR}/external/luajit-2.0/src
    ${CMAKE_SOURCE_DIR}/external/xxHash
    ${CMAKE_SOURCE_DIR}/external/libcircllhist/src
    ${CMAKE_BINARY_DIR}/api/
    ${CMAKE_BINARY_DIR}/ev_lib/
    )

add_library(sig_action STATIC ${CMAKE_CURRENT_SOURCE_DIR}/signal_action.cc)
set_target_properties(sig_action PROPERTIES LINKER_LANGUAGE CXX)

add_library(${project_name}-lib ${sources})

#SET(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
#SET(BUILD_SHARED_LIBS OFF)
#SET(CMAKE_EXE_LINKER_FLAGS "-static")

add_executable(${project_name} ${sources})
set_property(TARGET ${project_name} PROPERTY CXX_STANDARD 14)


add_custom_command(TARGET ${project_name}
    PRE_BUILD
    COMMAND echo " --- CUSTOM COMMAND ---"
    COMMAND touch ${CMAKE_SOURCE_DIR}/source/exe/main.cc
    COMMAND mkdir -p ${ev_lib_dir}
    #COMMAND find ${ev_external_src_dir}           -name \"lib*.a\"       -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'
    #COMMAND find ${ev_external_src_dir}           -name \"lib*.so\"      -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'
    #COMMAND find ${ev_external_src_dir}           -name \"lib*.so.*\"    -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'

    #COMMAND find ${e_bin_dir}           -name \"lib*.a\"       -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'
    #COMMAND find ${e_bin_dir}           -name \"lib*.so\"      -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'
    #COMMAND find ${e_bin_dir}           -name \"lib*.so.*\"    -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'

    #COMMAND find ${ev_bin_external_dir}  -name \"lib*.a\"       -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'
    #COMMAND find ${ev_bin_external_dir}  -name \"lib*.so\"      -exec ${rsync_cmd} {} ${ev_lib_dir} '\;'

    #Make sure we overwrite with the right version of libevent - the one without dpdk
    #COMMAND find ${ev_bin_external_le_dir}  -name \"lib*.so.*\"    -exec cp -f {} ${ev_lib_dir} '\;'
    )


SET(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
SET(BUILD_SHARED_LIBS OFF)
SET(CMAKE_EXE_LINKER_FLAGS "-static")

# Note that while inspecting the descriptor pool for protobuf in validateProtoDescriptors()
# some of the descriptors came up missing.
# This resulted in this call Protobuf::DescriptorPool::generated_pool()->FindMethodByName(method) returning nullptr
# The issue here https://github.com/google/protobuf/issues/4221 talks about how the linker drops
# some symbols.
# The fix is to provide linker flags to ensure that linker doesn't drop symbols
# Description about this can be found here
# https://cmake.org/pipermail/cmake/2007-September/016238.html
# and here
# https://www.mail-archive.com/cmake@cmake.org/msg01890.html
# After adding the -Wl,-whole-archive -lev_protos-data-plane-api -Wl,-no-whole-archive
# the linker doesn't drop the symbols.
# Also note that CMake leaves the line alone by directly sending it to the linker since
# it starts with a '-'
target_link_libraries(${project_name}
    -Wl,-whole-archive -lenvoy-source-server -Wl,-no-whole-archive
    -Wl,-whole-archive -lprotos-api -Wl,-no-whole-archive
    -Wl,-whole-archive -ldl -Wl,-no-whole-archive
    -Wl,-whole-archive -lnuma -Wl,-no-whole-archive
    xxhash
    envoy-source-common-memory
    envoy-source-common-singleton
    envoy-source-common-thread_local
    -Wl,-whole-archive -lenvoy-source-common-ssl -Wl,-no-whole-archive
    envoy-source-common-ratelimit
    -Wl,-whole-archive -lenvoy-source-common-tcp -Wl,-no-whole-archive
    -Wl,-whole-archive -lenvoy-source-common-upstream -Wl,-no-whole-archive
    #redis
    envoy-source-common-html
    envoy-source-common-profiler
    envoy-source-common-event
    #network
    envoy-source-common-json
	yaml-cpp
    envoy-source-common-filesystem
    envoy-source-common-api
    -Wl,-whole-archive -lenvoy-source-common-router -Wl,-no-whole-archive
    #mongo
    #dynamo
    -Wl,-whole-archive -lenvoy-source-common-grpc -Wl,-no-whole-archive
    envoy-source-common-stats
    -Wl,-whole-archive -lenvoy-source-common-config -Wl,-no-whole-archive
    -Wl,-whole-archive -lenvoy-source-common-http -Wl,-no-whole-archive
    -Wl,-whole-archive -le-common -Wl,-no-whole-archive
    #filter
    #ext_authz
    envoy-external-http-parser
    envoy-source-common-request_info
    envoy-source-common-tracing
    -Wl,-whole-archive -llightstep_tracer -Wl,-no-whole-archive
	-Wl,-whole-archive -lopentracing -Wl,-no-whole-archive
    envoy-source-common-runtime
    -Wl,-whole-archive -lenvoy-source-common-network -Wl,-no-whole-archive
    -Wl,-whole-archive -lnghttp2  -Wl,-no-whole-archive
    -Wl,-whole-archive -lssl2 -Wl,-no-whole-archive
    -Wl,-whole-archive -lcrypto2 -Wl,-no-whole-archive
    # posix real-time extensions
    rt
    -Wl,-whole-archive -lenvoy-source-common-protobuf -Wl,-no-whole-archive
    protobuf
	absl_utility
	absl_time
	absl_memory
	absl_synchronization
	absl_algorithm
	absl_bad_optional_access
	absl_bad_any_cast
	absl_span
	absl_optional
	absl_any
	absl_strings
	absl_int128
	absl_numeric
	absl_malloc_internal
	absl_throw_delegate
	absl_spinlock_wait
	absl_base
	absl_dynamic_annotations
	absl_meta
	absl_stacktrace
	absl_leak_check
	absl_debugging
    cares
    envoy-source-common-buffer
    pthread
    event_pthreads2
    event_core2
    event2
    event_extra2
    -Wl,-whole-archive -lenvoy-source-extensions -Wl,-no-whole-archive
    -Wl,-whole-archive -lluajit -Wl,-no-whole-archive
	envoy-external-grpc-httpjson-transcoding
	envoy-source-common-compressor
	envoy-source-common-decompressor
    z
    -lcircllhist
    -lenvoy-source-common-tcp_proxy
    -lenvoy-source-server-http
    -lenvoy-source-common-access_log
    -lenvoy-source-common-secret
    -lenvoy-external-jwt_verify_lib
    grpc++
    -lfmt
    )

#set_target_properties(${project_name}   PROPERTIES LINK_FLAGS "-static" )
