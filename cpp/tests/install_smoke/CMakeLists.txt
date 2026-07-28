cmake_minimum_required(VERSION 3.28)
project(a11_install_smoke LANGUAGES CXX)

find_package(a11 CONFIG REQUIRED)

add_executable(a11_install_smoke smoke.cc)
target_link_libraries(a11_install_smoke PRIVATE a11::service)
target_compile_features(a11_install_smoke PRIVATE cxx_std_20)
