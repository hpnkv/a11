# The native benchmark binary. Its own executable rather than a mode of a test:
# a benchmark links the whole runtime and runs for minutes, and neither belongs
# in a target CI runs on every commit.
add_executable(a11_bench harness.cc bench_main.cc)
add_executable(a11::bench ALIAS a11_bench)
target_link_libraries(
  a11_bench PRIVATE a11::concurrency a11::data a11::net a11::nodes a11::stores
                    a11::actions a11::service nlohmann_json::nlohmann_json
                    a11::allocator)
target_include_directories(a11_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The uv round-trip row includes a11/net/internal/http_transport.h, which pulls
# in uvw; uvw's headers only compile against its own target's definitions.
target_link_libraries(a11_bench PRIVATE uvw::uvw)
