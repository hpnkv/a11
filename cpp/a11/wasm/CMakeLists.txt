# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.28)
project(a11_wasm LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure with emcmake cmake for the WebAssembly target")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(A11_WASM_OPTIMIZE_SIZE "Optimize release modules for download size" ON)
set(A11_WASM_MAXIMUM_MEMORY_MB 512 CACHE STRING
  "Maximum WebAssembly linear memory in MiB for the public module")
if(A11_WASM_MAXIMUM_MEMORY_MB LESS 64 OR
   A11_WASM_MAXIMUM_MEMORY_MB GREATER 2048)
  message(FATAL_ERROR "A11_WASM_MAXIMUM_MEMORY_MB must be 64..2048")
endif()
math(EXPR A11_WASM_MAXIMUM_MEMORY_BYTES
  "${A11_WASM_MAXIMUM_MEMORY_MB} * 1024 * 1024")
if(A11_WASM_OPTIMIZE_SIZE AND CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-Oz)
  add_link_options(-Oz)
endif()
include(FetchContent)

set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG 20260526.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(abseil-cpp nlohmann_json)

set(A11_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${A11_CPP_ROOT}/a11/flow" flow_sources)

add_library(a11_wasm_flow STATIC ${A11_FLOW_LANG_SOURCES})
target_include_directories(a11_wasm_flow PUBLIC "${A11_CPP_ROOT}")
target_compile_options(a11_wasm_flow PRIVATE -fno-exceptions)
target_link_libraries(
  a11_wasm_flow PUBLIC
  absl::status absl::status_macros absl::statusor absl::span
  absl::flat_hash_map absl::flat_hash_set absl::time absl::strings
  absl::str_format nlohmann_json::nlohmann_json)

add_library(a11_wasm_data STATIC
  "${A11_CPP_ROOT}/a11/json_codec.cc"
  "${A11_CPP_ROOT}/a11/status.cc"
  "${A11_CPP_ROOT}/a11/data/json.cc"
  "${A11_CPP_ROOT}/a11/data/msgpack.cc"
  "${A11_CPP_ROOT}/a11/data/serialization.cc"
  "${A11_CPP_ROOT}/a11/data/types.cc"
  "${A11_CPP_ROOT}/a11/net/byte_chunking.cc")
target_include_directories(a11_wasm_data PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/shims" "${A11_CPP_ROOT}")
target_compile_options(a11_wasm_data PRIVATE -fno-exceptions)
set_source_files_properties("${A11_CPP_ROOT}/a11/json_codec.cc"
  PROPERTIES COMPILE_OPTIONS -fexceptions)
target_link_libraries(
  a11_wasm_data PUBLIC
  absl::status absl::status_macros absl::statusor absl::strings
  absl::time absl::flat_hash_map absl::log absl::str_format
  nlohmann_json::nlohmann_json)

add_library(a11_wasm_runtime STATIC runtime.cc)
target_include_directories(a11_wasm_runtime PUBLIC "${A11_CPP_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(a11_wasm_runtime PRIVATE -fno-exceptions)
target_link_libraries(a11_wasm_runtime PUBLIC a11_wasm_data)

add_executable(a11_wasm
  bridge_core.cc
  bridge_flow.cc
  bridge_codec.cc
  bridge_framing.cc
  bridge_runtime.cc)
set_target_properties(a11_wasm PROPERTIES OUTPUT_NAME a11 SUFFIX ".mjs")
target_include_directories(a11_wasm PRIVATE "${A11_CPP_ROOT}")
target_compile_options(a11_wasm PRIVATE -fno-exceptions)
target_link_libraries(a11_wasm PRIVATE a11_wasm_flow a11_wasm_runtime a11_wasm_data)
target_link_options(a11_wasm PRIVATE
  --no-entry
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=${A11_WASM_MAXIMUM_MEMORY_BYTES}
  -sFILESYSTEM=0
  -sEXPORTED_FUNCTIONS=_malloc,_free,_a11_wasm_flow_request,_a11_wasm_wire_encode,_a11_wasm_wire_decode,_a11_wasm_fragment_decode,_a11_wasm_frame_split,_a11_wasm_reassembler_new,_a11_wasm_reassembler_free,_a11_wasm_reassembler_feed,_a11_wasm_runtime_new,_a11_wasm_runtime_free,_a11_wasm_runtime_register,_a11_wasm_runtime_invoke,_a11_wasm_runtime_next_action,_a11_wasm_runtime_finish_action,_a11_wasm_runtime_node_append,_a11_wasm_runtime_node_append_ref,_a11_wasm_runtime_node_put_fragment,_a11_wasm_runtime_node_put_many,_a11_wasm_runtime_node_close,_a11_wasm_runtime_node_read,_a11_wasm_runtime_node_read_arrival,_a11_wasm_runtime_node_read_data,_a11_wasm_runtime_node_clear_data,_a11_wasm_runtime_node_arrival_seq,_a11_wasm_runtime_node_final_seq,_a11_wasm_runtime_node_count,_a11_wasm_runtime_node_closed,_a11_wasm_runtime_resolve,_a11_wasm_runtime_export_snapshot,_a11_wasm_runtime_import_snapshot,_a11_wasm_result_data,_a11_wasm_result_size,_a11_wasm_result_code,_a11_wasm_result_ready,_a11_wasm_result_free
  -sEXPORTED_RUNTIME_METHODS=HEAPU8)

add_executable(a11_wasm_ucontext_probe
  thread/ucontext_compat.cc thread/ucontext_probe.cc)
target_compile_options(a11_wasm_ucontext_probe PRIVATE -fno-exceptions)
target_link_options(a11_wasm_ucontext_probe PRIVATE -sASYNCIFY=1
  -sENVIRONMENT=node -sEXIT_RUNTIME=1)
set_target_properties(a11_wasm_ucontext_probe PROPERTIES SUFFIX ".mjs")

option(A11_WASM_BUILD_BOOST_PROBE "Build the Boost.Context fiber probe" OFF)
if(A11_WASM_BUILD_BOOST_PROBE)
  if(NOT A11_WASM_BOOST_INCLUDE_DIR OR NOT A11_WASM_BOOST_CONTEXT_SOURCE
      OR NOT A11_WASM_BOOST_FIBER_SOURCE)
    message(FATAL_ERROR "Set A11_WASM_BOOST_INCLUDE_DIR and both Boost source directories")
  endif()
  add_library(a11_wasm_boost_context STATIC
    "${A11_WASM_BOOST_CONTEXT_SOURCE}/src/continuation.cpp"
    "${A11_WASM_BOOST_CONTEXT_SOURCE}/src/fiber.cpp"
    "${A11_WASM_BOOST_CONTEXT_SOURCE}/src/posix/stack_traits.cpp")
  target_include_directories(a11_wasm_boost_context PUBLIC
    "${A11_WASM_BOOST_INCLUDE_DIR}")
  target_compile_definitions(a11_wasm_boost_context PUBLIC
    BOOST_USE_UCONTEXT BOOST_CONTEXT_NO_LIB)
  target_compile_definitions(a11_wasm_boost_context PRIVATE BOOST_CONTEXT_SOURCE)
  target_compile_options(a11_wasm_boost_context PRIVATE -pthread -fwasm-exceptions)

  add_library(a11_wasm_boost_fiber STATIC
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/algo/algorithm.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/algo/round_robin.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/algo/shared_work.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/algo/work_stealing.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/barrier.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/condition_variable.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/context.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/fiber.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/future.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/mutex.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/properties.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/recursive_mutex.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/recursive_timed_mutex.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/scheduler.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/timed_mutex.cpp"
    "${A11_WASM_BOOST_FIBER_SOURCE}/src/waker.cpp")
  target_include_directories(a11_wasm_boost_fiber PUBLIC
    "${A11_WASM_BOOST_INCLUDE_DIR}")
  target_compile_definitions(a11_wasm_boost_fiber PUBLIC
    BOOST_FIBER_NO_LIB BOOST_FIBER_STATIC_LINK BOOST_USE_UCONTEXT)
  target_compile_definitions(a11_wasm_boost_fiber PRIVATE
    BOOST_FIBER_SOURCE BOOST_FIBERS_SOURCE)
  target_compile_options(a11_wasm_boost_fiber PRIVATE -pthread -fwasm-exceptions)
  target_link_libraries(a11_wasm_boost_fiber PUBLIC a11_wasm_boost_context)

  add_executable(a11_wasm_boost_context_probe
    thread/ucontext_compat.cc thread/boost_context_probe.cc)
  target_link_libraries(a11_wasm_boost_context_probe PRIVATE
    a11_wasm_boost_context)
  target_compile_options(a11_wasm_boost_context_probe PRIVATE -pthread)
  target_link_options(a11_wasm_boost_context_probe PRIVATE -pthread -sASYNCIFY=1
    -fwasm-exceptions -sENVIRONMENT=node -sEXIT_RUNTIME=1)
  set_target_properties(a11_wasm_boost_context_probe PROPERTIES SUFFIX ".mjs")

  add_executable(a11_wasm_boost_fiber_probe
    thread/ucontext_compat.cc thread/boost_fiber_probe.cc)
  target_link_libraries(a11_wasm_boost_fiber_probe PRIVATE
    a11_wasm_boost_fiber)
  target_compile_options(a11_wasm_boost_fiber_probe PRIVATE -pthread)
  target_link_options(a11_wasm_boost_fiber_probe PRIVATE -pthread -sASYNCIFY=1
    -fwasm-exceptions -sENVIRONMENT=node -sEXIT_RUNTIME=1)
  set_target_properties(a11_wasm_boost_fiber_probe PROPERTIES SUFFIX ".mjs")

  add_library(a11_wasm_thread STATIC
    "${A11_CPP_ROOT}/thread/thread/boost_primitives.cc"
    "${A11_CPP_ROOT}/thread/thread/channel/waiter_state.cc"
    "${A11_CPP_ROOT}/thread/thread/fiber.cc"
    "${A11_CPP_ROOT}/thread/thread/internal/stack_walk.cc"
    "${A11_CPP_ROOT}/thread/thread/introspect.cc"
    "${A11_CPP_ROOT}/thread/thread/select.cc"
    "${A11_CPP_ROOT}/thread/thread/selectables.cc"
    "${A11_CPP_ROOT}/thread/thread/thread_pool.cc"
    "${A11_CPP_ROOT}/thread/thread/util.cc")
  target_include_directories(a11_wasm_thread PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/thread/shims"
    "${A11_CPP_ROOT}/thread" "${A11_WASM_BOOST_INCLUDE_DIR}")
  target_compile_definitions(a11_wasm_thread PRIVATE
    THREAD_DEFAULT_FIBER_STACK_SIZE=262144)
  target_compile_options(a11_wasm_thread PRIVATE -pthread)
  set_source_files_properties(
    "${A11_CPP_ROOT}/thread/thread/boost_primitives.cc"
    "${A11_CPP_ROOT}/thread/thread/thread_pool.cc"
    PROPERTIES COMPILE_OPTIONS -fwasm-exceptions)
  target_link_libraries(a11_wasm_thread PUBLIC a11_wasm_boost_fiber
    absl::base absl::flags absl::flat_hash_map absl::log
    absl::log_initialize absl::log_internal_check_op absl::stacktrace
    absl::str_format absl::strings absl::symbolize absl::random_random
    absl::time)

  add_library(a11_wasm_thread_data STATIC
    thread/json_codec_wasm.cc
    "${A11_CPP_ROOT}/a11/status.cc"
    "${A11_CPP_ROOT}/a11/data/json.cc"
    "${A11_CPP_ROOT}/a11/data/msgpack.cc"
    "${A11_CPP_ROOT}/a11/data/serialization.cc"
    "${A11_CPP_ROOT}/a11/data/types.cc"
    "${A11_CPP_ROOT}/a11/net/byte_chunking.cc")
  target_include_directories(a11_wasm_thread_data PUBLIC
    "${A11_CPP_ROOT}/thread" "${A11_CPP_ROOT}")
  target_compile_options(a11_wasm_thread_data PRIVATE -pthread
    -fwasm-exceptions)
  target_link_libraries(a11_wasm_thread_data PUBLIC a11_wasm_thread
    absl::status absl::status_macros absl::statusor absl::strings
    absl::time absl::flat_hash_map absl::log absl::str_format
    nlohmann_json::nlohmann_json)

  add_executable(a11_wasm_thread_probe
    thread/ucontext_compat.cc thread/thread_probe.cc)
  target_include_directories(a11_wasm_thread_probe PRIVATE
    "${A11_CPP_ROOT}/thread")
  target_link_libraries(a11_wasm_thread_probe PRIVATE a11_wasm_thread)
  target_compile_options(a11_wasm_thread_probe PRIVATE -pthread)
  target_link_options(a11_wasm_thread_probe PRIVATE -pthread -sASYNCIFY=1
    -fwasm-exceptions -sPTHREAD_POOL_SIZE=1 -sENVIRONMENT=web,worker,node
    -sEXIT_RUNTIME=1)
  set_target_properties(a11_wasm_thread_probe PROPERTIES SUFFIX ".mjs")

  add_executable(a11_wasm_store_probe
    thread/ucontext_compat.cc
    thread/store_probe.cc
    "${A11_CPP_ROOT}/a11/exception_guard.cc"
    "${A11_CPP_ROOT}/a11/concurrency/boundary.cc"
    "${A11_CPP_ROOT}/a11/concurrency/executor.cc"
    "${A11_CPP_ROOT}/a11/stores/local_chunk_store.cc")
  target_include_directories(a11_wasm_store_probe PRIVATE
    "${A11_CPP_ROOT}" "${A11_CPP_ROOT}/thread")
  target_link_libraries(a11_wasm_store_probe PRIVATE a11_wasm_thread_data)
  target_compile_options(a11_wasm_store_probe PRIVATE -pthread)
  set_source_files_properties(
    "${A11_CPP_ROOT}/a11/exception_guard.cc"
    "${A11_CPP_ROOT}/a11/concurrency/boundary.cc"
    PROPERTIES COMPILE_OPTIONS -fwasm-exceptions)
  target_link_options(a11_wasm_store_probe PRIVATE -pthread -sASYNCIFY=1
    -fwasm-exceptions -sPTHREAD_POOL_SIZE=1 -sENVIRONMENT=node
    -sEXIT_RUNTIME=1)
  set_target_properties(a11_wasm_store_probe PROPERTIES SUFFIX ".mjs")

  add_executable(a11_wasm_node_probe
    thread/ucontext_compat.cc
    thread/node_probe.cc
    "${A11_CPP_ROOT}/a11/exception_guard.cc"
    "${A11_CPP_ROOT}/a11/uuid.cc"
    "${A11_CPP_ROOT}/a11/concurrency/boundary.cc"
    "${A11_CPP_ROOT}/a11/concurrency/callback_scheduler.cc"
    "${A11_CPP_ROOT}/a11/concurrency/executor.cc"
    "${A11_CPP_ROOT}/a11/stores/chunk_store_reader.cc"
    "${A11_CPP_ROOT}/a11/stores/chunk_store_reference_reader.cc"
    "${A11_CPP_ROOT}/a11/stores/chunk_store_writer.cc"
    "${A11_CPP_ROOT}/a11/stores/local_chunk_store.cc"
    "${A11_CPP_ROOT}/a11/nodes/boundary.cc"
    "${A11_CPP_ROOT}/a11/nodes/async_node.cc"
    "${A11_CPP_ROOT}/a11/nodes/node_map.cc"
    "${A11_CPP_ROOT}/a11/net/wire_stream.cc")
  target_include_directories(a11_wasm_node_probe BEFORE PRIVATE
    "${A11_CPP_ROOT}/thread" "${A11_CPP_ROOT}")
  target_link_libraries(a11_wasm_node_probe PRIVATE a11_wasm_thread_data)
  target_compile_options(a11_wasm_node_probe PRIVATE -pthread)
  set_source_files_properties(
    "${A11_CPP_ROOT}/a11/nodes/boundary.cc"
    PROPERTIES COMPILE_OPTIONS -fwasm-exceptions)
  target_link_options(a11_wasm_node_probe PRIVATE -pthread -sASYNCIFY=1
    -fwasm-exceptions -sPTHREAD_POOL_SIZE=1 -sENVIRONMENT=node
    -sEXIT_RUNTIME=1)
  set_target_properties(a11_wasm_node_probe PROPERTIES SUFFIX ".mjs")

  option(A11_WASM_BUILD_ACTION_PROBE "Test native action and Flow runtime" OFF)
  option(A11_WASM_BUILD_NATIVE_RUNTIME "Build native action and Flow module" OFF)
  if(A11_WASM_BUILD_ACTION_PROBE OR A11_WASM_BUILD_NATIVE_RUNTIME)
    set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RE2_USE_ICU OFF CACHE BOOL "" FORCE)
    set(RE2_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(re2
      GIT_REPOSITORY https://github.com/google/re2.git
      GIT_TAG 2025-11-05
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(re2)
    add_subdirectory("${A11_CPP_ROOT}/a11/actions" actions_sources)
    add_subdirectory("${A11_CPP_ROOT}/a11/service" service_sources)
    add_subdirectory("${A11_CPP_ROOT}/a11/net" net_sources)
    add_subdirectory("${A11_CPP_ROOT}/a11/nodes" nodes_sources)
    add_library(a11_wasm_native_runtime STATIC
      thread/crypto_compat.cc
      thread/obs_stub.cc
      "${A11_CPP_ROOT}/a11/exception_guard.cc"
      "${A11_CPP_ROOT}/a11/uuid.cc"
      "${A11_CPP_ROOT}/a11/time.cc"
      "${A11_CPP_ROOT}/a11/regex.cc"
      "${A11_CPP_ROOT}/a11/obs/trace_context.cc"
      "${A11_CPP_ROOT}/a11/concurrency/boundary.cc"
      "${A11_CPP_ROOT}/a11/concurrency/callback_scheduler.cc"
      "${A11_CPP_ROOT}/a11/concurrency/executor.cc"
      "${A11_CPP_ROOT}/a11/concurrency/parallel.cc"
      "${A11_CPP_ROOT}/a11/stores/chunk_store_reader.cc"
      "${A11_CPP_ROOT}/a11/stores/chunk_store_reference_reader.cc"
      "${A11_CPP_ROOT}/a11/stores/chunk_store_writer.cc"
      "${A11_CPP_ROOT}/a11/stores/local_chunk_store.cc"
      "${A11_CPP_ROOT}/sdk/flow/actions/options.cc"
      "${A11_CPP_ROOT}/sdk/flow/actions/ports.cc"
      "${A11_CPP_ROOT}/sdk/flow/actions/time_actions.cc"
      thread/stop_wasm.cc
      thread/wasm_system_actions.cc
      ${A11_NET_SOURCES}
      ${A11_NODES_SOURCES}
      ${A11_ACTIONS_SOURCES}
      ${A11_SERVICE_SOURCES}
      ${A11_FLOW_RUNTIME_SOURCES})
    target_include_directories(a11_wasm_native_runtime BEFORE PUBLIC
      "${A11_CPP_ROOT}/thread" "${A11_CPP_ROOT}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shims"
      "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(a11_wasm_native_runtime PUBLIC
      a11_wasm_thread_data a11_wasm_flow re2::re2)
    target_compile_options(a11_wasm_native_runtime PRIVATE -pthread)
    set_source_files_properties(
      "${A11_CPP_ROOT}/a11/net/boundary.cc"
      "${A11_CPP_ROOT}/a11/actions/boundary.cc"
      "${A11_CPP_ROOT}/a11/service/boundary.cc"
      PROPERTIES COMPILE_OPTIONS -fwasm-exceptions)

    add_executable(a11_wasm_action_probe
      thread/ucontext_compat.cc thread/action_probe.cc)
    target_link_libraries(a11_wasm_action_probe PRIVATE
      a11_wasm_native_runtime)
    target_compile_options(a11_wasm_action_probe PRIVATE -pthread)
    target_link_options(a11_wasm_action_probe PRIVATE -pthread -sASYNCIFY=1
      -fwasm-exceptions -sPTHREAD_POOL_SIZE=1 -sENVIRONMENT=node
      -sEXIT_RUNTIME=1)
    set_target_properties(a11_wasm_action_probe PROPERTIES SUFFIX ".mjs")

    add_executable(a11_wasm_native
      bridge_core.cc bridge_native.cc bridge_native_objects.cc
      host_stream.cc host_settings.cc graph_snapshot.cc runtime.cc async_task.cc
      thread/ucontext_compat.cc thread/native_module_main.cc)
    set_target_properties(a11_wasm_native PROPERTIES
      OUTPUT_NAME a11-native SUFFIX ".mjs")
    target_link_libraries(a11_wasm_native PRIVATE
      a11_wasm_native_runtime)
    target_compile_options(a11_wasm_native PRIVATE -pthread)
    target_link_options(a11_wasm_native PRIVATE
      -pthread -sASYNCIFY=1 -fwasm-exceptions -sEXIT_RUNTIME=0
      -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node
      -sPTHREAD_POOL_SIZE=1 -sALLOW_MEMORY_GROWTH=1
      -sMAXIMUM_MEMORY=${A11_WASM_MAXIMUM_MEMORY_BYTES} -sFILESYSTEM=0
      -sEXPORTED_FUNCTIONS=_malloc,_free,_a11_wasm_native_flow_start,_a11_wasm_native_objects_new,_a11_wasm_native_objects_free,_a11_wasm_native_command_start,_a11_wasm_task_launch,_a11_wasm_task_poll,_a11_wasm_task_cancel,_a11_wasm_task_free,_a11_wasm_result_data,_a11_wasm_result_size,_a11_wasm_result_code,_a11_wasm_result_ready,_a11_wasm_result_free
      -sEXPORTED_RUNTIME_METHODS=HEAPU8)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_link_options(a11_wasm_native PRIVATE -g -sASSERTIONS=2)
    endif()

    option(A11_WASM_BUILD_NATIVE_TESTS
      "Run selected native A11 tests against the Emscripten runtime" OFF)
    if(A11_WASM_BUILD_NATIVE_TESTS)
      set(A11_WASM_TEST_MAXIMUM_MEMORY_MB 1024 CACHE STRING
        "Maximum memory for the Node-hosted native test binary")
      math(EXPR A11_WASM_TEST_MAXIMUM_MEMORY_BYTES
        "${A11_WASM_TEST_MAXIMUM_MEMORY_MB} * 1024 * 1024")
      set(gtest_build_tests OFF CACHE BOOL "" FORCE)
      set(gmock_build_tests OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
      FetchContent_MakeAvailable(googletest)
      target_compile_options(gtest PRIVATE -fwasm-exceptions)
      target_compile_options(gtest_main PRIVATE -fwasm-exceptions)
      add_executable(a11_wasm_native_test
        "${A11_CPP_ROOT}/tests/local_chunk_store_test.cc"
        "${A11_CPP_ROOT}/tests/chunk_store_reader_test.cc"
        "${A11_CPP_ROOT}/tests/chunk_store_writer_test.cc"
        "${A11_CPP_ROOT}/tests/async_node_test.cc"
        "${A11_CPP_ROOT}/tests/action_test.cc"
        "${A11_CPP_ROOT}/tests/session_test.cc"
        "${A11_CPP_ROOT}/tests/flow_runtime_test.cc"
        "${A11_CPP_ROOT}/tests/flow_frame_test.cc"
        "${A11_CPP_ROOT}/tests/flow_shape_test.cc"
        "${A11_CPP_ROOT}/tests/flow_values_test.cc"
        thread/native_test_main.cc
        thread/ucontext_compat.cc)
      target_link_libraries(a11_wasm_native_test PRIVATE
        a11_wasm_native_runtime GTest::gtest)
      target_compile_options(a11_wasm_native_test PRIVATE
        -pthread -fwasm-exceptions)
      target_link_options(a11_wasm_native_test PRIVATE -pthread -sASYNCIFY=1
        -fwasm-exceptions -sPTHREAD_POOL_SIZE=1 -sENVIRONMENT=node
        -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=${A11_WASM_TEST_MAXIMUM_MEMORY_BYTES}
        -sEXPORTED_RUNTIME_METHODS=callMain
        -sEXPORTED_FUNCTIONS=_main,_a11_wasm_native_test_status)
      set_target_properties(a11_wasm_native_test PROPERTIES SUFFIX ".mjs")
    endif()
  endif()
endif()
