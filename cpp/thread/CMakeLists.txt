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
        thread/internal/work_queue.h
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

# Boost.Fiber and Boost.Context throw -- fiber_error, and the forced_unwind that
# tears a fibre down -- and these two translation units are the only ones that
# include them. Everything else here, including the public headers, is
# boost-free by design (see the pImpl in boost_primitives.h), so the exception
# boundary stops at these two files.
a11_disallow_exceptions(Thread)
a11_allow_exceptions(thread/boost_primitives.cc thread/thread_pool.cc)

# 512 KiB, not 64 KiB, and the reason is measured rather than cautious.
#
# A store write driven inline needs 67,695 B below Drive() once it tees to an
# attached WireStream (see the table in stores/chunk_store_writer.cc), which is
# more than a 64 KiB stack holds -- and `DriveInline` permits four nested drives,
# so the worst case is ~271 KiB. That is what blocked FINDINGS.md item 3: the
# ~9x available from flushing a confirmed write inline could not be taken because
# the caller's stack could not hold the work.
#
# Raised globally rather than per store or per call site. A per-store stack would
# mean every *caller* of a store write needed to know it was one, which is exactly
# the knowledge a pump that drives on the caller's thread does not have.
#
# The cost is address space, not resident memory: a fibre stack is mmap'd and only
# touched pages become resident, so a fibre that uses 2 KiB of its 512 KiB costs
# 2 KiB of RSS. `memory_slope` and the RSS rows in `python -m bench` are what
# check that claim, and they are the reason to re-measure if this changes again.
set(THREAD_DEFAULT_FIBER_STACK_SIZE "524288" CACHE STRING
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

  # A11_POOL_PIN. Its own executable because it has to set the variable before
  # anything touches the pool -- which reads its environment when it starts, and
  # starts on first use -- so it provides its own main and links gtest without
  # gtest_main.
  add_executable(thread_affinity_test tests/thread_affinity_test.cc)
  target_link_libraries(thread_affinity_test PRIVATE Thread GTest::gtest)
  target_compile_options(
    thread_affinity_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wno-nullability-extension>
    $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
  add_test(NAME thread_affinity_test COMMAND thread_affinity_test)
  set_tests_properties(thread_affinity_test PROPERTIES TIMEOUT 30)
endif()
