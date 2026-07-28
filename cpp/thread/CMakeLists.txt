add_library(Thread STATIC)
add_library(a11::thread ALIAS Thread)
set_target_properties(Thread PROPERTIES EXPORT_NAME thread)
target_include_directories(
        Thread SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_sources(
        Thread PRIVATE
        thread/boost_primitives.cc
        thread/channel/waiter_state.cc
        thread/channel/waiter_state.h
        thread/fiber.cc
        thread/select.cc
        thread/selectables.cc
        thread/thread_pool.cc
        thread/util.cc
)
target_sources(
        Thread PRIVATE
        thread/cases.h
        thread/boost_primitives.h
        thread/channel.h
        thread/concurrency.h
        thread/executor.h
        thread/fiber.h
        thread/select.h
        thread/selectables.h
        thread/thread_pool.h
        thread/util.h
)
target_link_libraries(
        Thread PUBLIC
        $<BUILD_INTERFACE:absl::base> $<INSTALL_INTERFACE:absl::base>
        $<BUILD_INTERFACE:absl::flags> $<INSTALL_INTERFACE:absl::flags>
        $<BUILD_INTERFACE:absl::flat_hash_map> $<INSTALL_INTERFACE:absl::flat_hash_map>
        $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log>
        $<BUILD_INTERFACE:absl::log_initialize> $<INSTALL_INTERFACE:absl::log_initialize>
        $<BUILD_INTERFACE:absl::log_internal_check_op> $<INSTALL_INTERFACE:absl::log_internal_check_op>
        $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random>
        $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
        Boost::thread
)
target_link_libraries(
        Thread PUBLIC
        Boost::fiber
)
target_compile_options(
        Thread PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wall;-Wextra;-Wno-nullability-completeness;-Wno-sign-compare;-Wthread-safety;-Werror=thread-safety>
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wno-sign-compare>
)
target_compile_features(Thread PUBLIC cxx_std_20)

set(THREAD_DEFAULT_FIBER_STACK_SIZE "65536" CACHE STRING
    "Default stack size in bytes for root fibers whose TreeOptions use zero")
target_compile_definitions(
  Thread PRIVATE
  THREAD_DEFAULT_FIBER_STACK_SIZE=${THREAD_DEFAULT_FIBER_STACK_SIZE})

if(BUILD_TESTING)
  add_executable(thread_test tests/thread_test.cc)
  target_link_libraries(thread_test PRIVATE Thread GTest::gtest_main)
  target_compile_options(
    thread_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wno-nullability-extension>
    $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
  add_test(NAME thread_test COMMAND thread_test)
  set_tests_properties(thread_test PROPERTIES TIMEOUT 30)
endif()
