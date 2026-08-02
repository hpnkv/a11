cmake_minimum_required(VERSION 3.28)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif ()

project(
        a11
        VERSION 0.1.3
        DESCRIPTION "A11 action and streaming runtime"
        LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

option(A11_BUILD_PYTHON "Build the pybind11 extension" ON)
option(A11_BUILD_HTTP "Build HTTP wire streams and servers" ON)
option(A11_BUILD_WEBRTC "Build WebRTC wire streams and signalling" ON)
option(A11_FETCH_MISSING_DEPS "Fetch required dependencies not installed" ON)
option(A11_REQUIRE_STATIC_DEPS
        "Fail configuration when a non-system dependency is only shared" ON)
option(A11_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(A11_WITH_OTLP_HTTP
        "Build the native OTLP/HTTP (JSON) span exporter (needs libcurl)" ON)
set(A11_PYBIND11_ABSEIL_SOURCE_DIR "" CACHE PATH
        "Optional local pybind11_abseil checkout")

function(a11_require_static_target target_name)
    if (NOT A11_REQUIRE_STATIC_DEPS OR NOT TARGET ${target_name})
        return()
    endif ()
    get_target_property(A11_DEPENDENCY_TYPE ${target_name} TYPE)
    if (A11_DEPENDENCY_TYPE STREQUAL "SHARED_LIBRARY" OR
            A11_DEPENDENCY_TYPE STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "${target_name} resolved to a shared library")
    endif ()
    foreach (A11_LOCATION_PROPERTY IN ITEMS
            IMPORTED_LOCATION IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION_RELEASE
            IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION_MINSIZEREL)
        get_target_property(
                A11_DEPENDENCY_LOCATION ${target_name} ${A11_LOCATION_PROPERTY})
        if (A11_DEPENDENCY_LOCATION AND
                A11_DEPENDENCY_LOCATION MATCHES "\\.(dylib|so)(\\.|$)")
            message(FATAL_ERROR
                    "${target_name} resolved to shared object: ${A11_DEPENDENCY_LOCATION}")
        endif ()
    endforeach ()
    unset(A11_DEPENDENCY_LOCATION)
    unset(A11_DEPENDENCY_TYPE)
    unset(A11_LOCATION_PROPERTY)
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
# A11's libraries name their linkage explicitly; this setting governs fetched
# dependencies and prevents a parent project from silently making them shared.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build bundled dependencies statically"
        FORCE)
if (APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/.libs;@loader_path/../lib")
elseif (UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/.libs;$ORIGIN/../lib")
endif ()

# Rewrites __FILE__/__builtin_FILE() (assertions, source_location, logging
# macros) to a path relative to the source tree so build-host absolute paths
# never end up embedded in shipped binaries. Set directory-wide, before any
# FetchContent_MakeAvailable() below, so it also reaches dependencies built
# from source (Abseil, libdatachannel, pybind11/pybind11_abseil) and not just
# A11's own targets.
add_compile_options(
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>)
add_compile_definitions(A11_CPP_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}")

# A11 uses APIs first released with Abseil 20260526 (notably the upstream
# status macros). Pinning the source archive keeps every build, including
# wheels, on one ABI-compatible Abseil rather than silently selecting an older
# system package.
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_MONOLITHIC_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        abseil-cpp
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20260526.0
        GIT_SHALLOW TRUE
        SYSTEM)
FetchContent_MakeAvailable(abseil-cpp)
a11_require_static_target(absl::base)
a11_require_static_target(absl::log)
a11_require_static_target(absl::status)
a11_require_static_target(absl::strings)
a11_require_static_target(absl::time)

# OpenTelemetry C++ supplies A11's span/trace/context model. It is built
# statically from source (like Abseil) so the shipped extension stays
# self-contained and relocatable. Only the API + SDK and the local
# in-memory/ostream exporters are needed for now; the OTLP/protobuf exporters
# are introduced by a later phase and stay OFF here to avoid pulling in
# protobuf. WITH_STL=CXX20 resolves OTel's nostd:: aliases to std:: types
# (span/variant/string_view) instead of its vendored Abseil copy, which fails
# to compile under C++20 libc++ (ambiguous is_trivially_destructible); it also
# avoids coupling to A11's pinned Abseil ABI, so WITH_ABSEIL stays OFF.
set(WITH_STL CXX20 CACHE STRING "" FORCE)
set(WITH_ABSEIL OFF CACHE BOOL "" FORCE)
set(WITH_OTLP_HTTP OFF CACHE BOOL "" FORCE)
set(WITH_OTLP_GRPC OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_FUNC_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)
# Use the non-deprecated SDK factory path (A11 builds the provider from a
# TracerContext), silencing opentelemetry-cpp's temporary migration warning.
set(WITH_DEPRECATED_SDK_FACTORY OFF CACHE BOOL "" FORCE)
# opentelemetry-cpp gates a Windows-only DLL build purely on whether
# OPENTELEMETRY_BUILD_DLL is *defined* (any value), fataling on non-Windows.
# A11 never wants that here (Linux/macOS now, Windows later can opt in), so
# clear any lingering cache entry -- e.g. one an earlier configure or IDE tree
# wrote -- before OTel's CMake reads it. Fresh trees are unaffected.
unset(OPENTELEMETRY_BUILD_DLL CACHE)
# opentelemetry-cpp keys its own test targets off the top-level BUILD_TESTING.
# Suppress them (they need GTest wired the OTel way) while it is added, then
# restore A11's setting for our suite.
set(A11_SAVED_BUILD_TESTING "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
FetchContent_Declare(
        opentelemetry-cpp
        GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
        GIT_TAG v1.16.1
        GIT_SHALLOW TRUE
        SYSTEM)
FetchContent_MakeAvailable(opentelemetry-cpp)
set(BUILD_TESTING "${A11_SAVED_BUILD_TESTING}")
unset(A11_SAVED_BUILD_TESTING)
# Consumed via FetchContent, so the raw (non-namespaced) target names are used;
# opentelemetry-cpp only defines its opentelemetry-cpp:: aliases when installed.
a11_require_static_target(opentelemetry_api)
a11_require_static_target(opentelemetry_trace)
a11_require_static_target(opentelemetry_resources)
a11_require_static_target(opentelemetry_exporter_in_memory)
a11_require_static_target(opentelemetry_exporter_ostream_span)

if (A11_WITH_OTLP_HTTP)
    # The native OTLP/HTTP exporter posts OTLP-JSON via libcurl. In wheels the
    # deps prefix supplies a static libcurl (built against A11's static
    # OpenSSL); for local builds the system libcurl is used. Static linkage is
    # verified for shipped wheels by scripts/audit_wheel.py rather than forced
    # here, so a dynamic system libcurl remains fine for development.
    find_package(CURL REQUIRED)
endif ()

find_package(nlohmann_json CONFIG REQUIRED)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME OFF)
# For isolated static builds (wheels) the deps prefix on CMAKE_PREFIX_PATH is the
# single source of truth. Restrict Boost's upstream CMake config to that prefix
# (NO_DEFAULT_PATH + PATHS) so an incomplete prefix fails loudly with "could not
# find Boost" instead of silently resolving a system/Homebrew Boost -- which on a
# cross build is the wrong architecture, and elsewhere hides a broken prefix.
# Boost_NO_SYSTEM_PATHS is a module-mode variable and does not constrain config
# mode, so the search paths must be pinned on the call itself. Developer builds
# (static deps not required) keep the default search so a system Boost works.
if (A11_REQUIRE_STATIC_DEPS)
    find_package(Boost 1.82 REQUIRED CONFIG COMPONENTS context fiber thread
            NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
else ()
    find_package(Boost 1.82 REQUIRED COMPONENTS context fiber thread)
endif ()
a11_require_static_target(Boost::context)
a11_require_static_target(Boost::fiber)
a11_require_static_target(Boost::thread)

# The deps prefix builds Boost.Fiber with a futex spinlock on macOS (see
# scripts/bootstrap_wheel_deps.sh). Boost.Fiber selects its spinlock in headers,
# so every A11 translation unit must define the SAME macro the prefix's library
# was built with; otherwise boost::fibers::mutex has different layouts in the
# archive and this extension (an ODR violation). Only apply this for the
# isolated static-deps build that uses that prefix -- a system/Homebrew Boost is
# unpatched (no BOOST_FIBERS_HAS_FUTEX on macOS) and would fail to compile with a
# futex spinlock requested. A11_FIBER_SPINLOCK must match the value bootstrap
# used, and needs a >= 14.4 deployment target for futex availability.
if (APPLE AND A11_REQUIRE_STATIC_DEPS)
    set(A11_FIBER_SPINLOCK "$ENV{A11_FIBER_SPINLOCK}")
    if (NOT A11_FIBER_SPINLOCK)
        set(A11_FIBER_SPINLOCK BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX)
    endif ()
    if (NOT A11_FIBER_SPINLOCK STREQUAL "BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX"
            AND NOT A11_FIBER_SPINLOCK STREQUAL "BOOST_FIBERS_SPINLOCK_TTAS_FUTEX")
        message(FATAL_ERROR
                "A11_FIBER_SPINLOCK must be BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX"
                " or BOOST_FIBERS_SPINLOCK_TTAS_FUTEX (got ${A11_FIBER_SPINLOCK})")
    endif ()
    add_compile_definitions(${A11_FIBER_SPINLOCK})
endif ()

if (A11_BUILD_HTTP)
    # Resolve OpenSSL before libdatachannel so its transitive crypto linkage uses
    # the same static archives as A11's HTTP implementation.
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(PkgConfig REQUIRED)
    if (UNIX AND A11_REQUIRE_STATIC_DEPS)
        pkg_check_modules(OPENSSL_PC REQUIRED openssl)
        find_library(
                A11_OPENSSL_CRYPTO_STATIC_LIBRARY
                NAMES libcrypto.a crypto_static
                HINTS ${OPENSSL_PC_STATIC_LIBRARY_DIRS} ${OPENSSL_PC_LIBRARY_DIRS}
                NO_DEFAULT_PATH REQUIRED)
        find_library(
                A11_OPENSSL_SSL_STATIC_LIBRARY
                NAMES libssl.a ssl_static
                HINTS ${OPENSSL_PC_STATIC_LIBRARY_DIRS} ${OPENSSL_PC_LIBRARY_DIRS}
                NO_DEFAULT_PATH REQUIRED)
        set(OPENSSL_CRYPTO_LIBRARY "${A11_OPENSSL_CRYPTO_STATIC_LIBRARY}"
                CACHE FILEPATH "Static OpenSSL crypto archive" FORCE)
        set(OPENSSL_SSL_LIBRARY "${A11_OPENSSL_SSL_STATIC_LIBRARY}"
                CACHE FILEPATH "Static OpenSSL SSL archive" FORCE)
    endif ()
    find_package(OpenSSL REQUIRED)
    a11_require_static_target(OpenSSL::Crypto)
    a11_require_static_target(OpenSSL::SSL)
endif ()

if (A11_BUILD_WEBRTC)
    find_package(LibDataChannel CONFIG QUIET)
endif ()
if (A11_BUILD_WEBRTC AND NOT LibDataChannel_FOUND AND A11_FETCH_MISSING_DEPS)
    set(NO_MEDIA ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(USE_SYSTEM_JSON ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
            libdatachannel
            GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
            GIT_TAG 443f6934d9007eb7076ab7825ba330f355fcbead
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(libdatachannel)
elseif (A11_BUILD_WEBRTC AND NOT LibDataChannel_FOUND)
    message(FATAL_ERROR
            "libdatachannel is required; install it or enable A11_FETCH_MISSING_DEPS")
endif ()
if (A11_BUILD_WEBRTC)
    a11_require_static_target(LibDataChannel::LibDataChannel)
endif ()
if (A11_BUILD_WEBRTC AND NOT A11_BUILD_HTTP)
    message(FATAL_ERROR
            "A11_BUILD_WEBRTC requires A11_BUILD_HTTP for nghttp2 signalling")
endif ()
if (A11_BUILD_HTTP)
    find_package(uvw CONFIG REQUIRED)
    a11_require_static_target(uvw::uvw)
    pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)
    find_library(
            A11_NGHTTP2_STATIC_LIBRARY
            NAMES libnghttp2.a nghttp2_static
            HINTS ${NGHTTP2_STATIC_LIBRARY_DIRS} ${NGHTTP2_LIBRARY_DIRS}
            NO_DEFAULT_PATH)
    if (A11_NGHTTP2_STATIC_LIBRARY)
        add_library(a11_nghttp2 STATIC IMPORTED GLOBAL)
        set_target_properties(
                a11_nghttp2 PROPERTIES
                IMPORTED_LOCATION "${A11_NGHTTP2_STATIC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIRS}"
                INTERFACE_LINK_OPTIONS "${NGHTTP2_STATIC_LDFLAGS_OTHER}")
        add_library(a11::nghttp2 ALIAS a11_nghttp2)
    elseif (A11_REQUIRE_STATIC_DEPS)
        message(FATAL_ERROR
                "A static libnghttp2 archive is required but was not found in: "
                "${NGHTTP2_LIBRARY_DIRS}")
    else ()
        pkg_check_modules(NGHTTP2_SHARED REQUIRED IMPORTED_TARGET libnghttp2)
        add_library(a11::nghttp2 ALIAS PkgConfig::NGHTTP2_SHARED)
        message(WARNING
                "Using shared libnghttp2; consumers must place it on the loader path")
    endif ()
endif ()

if (BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif ()

# Each A11 component owns its source list beside the implementation. Targets
# remain here so their public dependency graph is visible in one place.
add_subdirectory(a11)
add_subdirectory(a11/actions)
add_subdirectory(a11/concurrency)
add_subdirectory(a11/data)
add_subdirectory(a11/net)
add_subdirectory(a11/obs)
add_subdirectory(a11/nodes)
add_subdirectory(a11/service)
add_subdirectory(a11/stores)
add_subdirectory(python)

# The bundled thread library is the scheduling substrate for every A11 task.
add_subdirectory(thread)

add_library(a11_warnings INTERFACE)
add_library(a11::warnings ALIAS a11_warnings)
set_target_properties(a11_warnings PROPERTIES EXPORT_NAME warnings)
target_compile_options(
        a11_warnings
        INTERFACE
        $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wmissing-field-initializers;-Wthread-safety;-Werror=thread-safety;-Wno-nullability-extension>
        $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
if (A11_WARNINGS_AS_ERRORS)
    target_compile_options(
            a11_warnings INTERFACE
            $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Werror>)
endif ()

add_library(a11_core STATIC ${A11_CORE_SOURCES})
add_library(a11::core ALIAS a11_core)
set_target_properties(a11_core PROPERTIES EXPORT_NAME core)
target_include_directories(
        a11_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_core
        PUBLIC $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::status_macros> $<INSTALL_INTERFACE:absl::status_macros> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
        $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
        PRIVATE $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log> a11::warnings nlohmann_json::nlohmann_json)

add_library(a11_data STATIC ${A11_DATA_SOURCES})
add_library(a11::data ALIAS a11_data)
set_target_properties(a11_data PROPERTIES EXPORT_NAME data)
target_link_libraries(
        a11_data
        PUBLIC a11::core Thread $<BUILD_INTERFACE:absl::flat_hash_map> $<INSTALL_INTERFACE:absl::flat_hash_map> $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        nlohmann_json::nlohmann_json
        PRIVATE $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time> a11::warnings)
target_include_directories(
        a11_data PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Observability (tracing) core. OpenTelemetry is linked PRIVATE and never
# appears in a11_obs's public headers (they use a fast pImpl), so it is fully
# absorbed into the static archive and does not leak into the exported package
# interface.
add_library(a11_obs STATIC ${A11_OBS_SOURCES})
add_library(a11::obs ALIAS a11_obs)
set_target_properties(a11_obs PROPERTIES EXPORT_NAME obs)
target_include_directories(
        a11_obs PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_obs
        PUBLIC a11::data $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        PRIVATE opentelemetry_trace opentelemetry_exporter_in_memory
        opentelemetry_exporter_ostream_span
        $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> $<BUILD_INTERFACE:absl::synchronization> $<INSTALL_INTERFACE:absl::synchronization>
        a11::warnings)
if (A11_WITH_OTLP_HTTP)
    target_compile_definitions(a11_obs PRIVATE A11_WITH_OTLP_HTTP)
    target_link_libraries(a11_obs PRIVATE CURL::libcurl)
endif ()

# Remaining components are added by their own source lists below as the public
# targets are intentionally independently linkable.
add_library(a11_concurrency STATIC ${A11_CONCURRENCY_SOURCES})
add_library(a11::concurrency ALIAS a11_concurrency)
set_target_properties(a11_concurrency PROPERTIES EXPORT_NAME concurrency)
target_include_directories(
        a11_concurrency PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_concurrency PUBLIC a11::core Thread $<BUILD_INTERFACE:absl::any_invocable> $<INSTALL_INTERFACE:absl::any_invocable> $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
        $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        PRIVATE $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log> a11::warnings)

add_library(a11_stores STATIC ${A11_STORES_SOURCES})
add_library(a11::stores ALIAS a11_stores)
set_target_properties(a11_stores PROPERTIES EXPORT_NAME stores)
target_include_directories(
        a11_stores PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_stores PUBLIC a11::concurrency a11::data $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        PRIVATE $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> a11::warnings)

add_library(a11_net STATIC ${A11_NET_SOURCES})
if (A11_BUILD_HTTP)
    target_sources(a11_net PRIVATE ${A11_NET_HTTP_SOURCES})
endif ()
if (A11_BUILD_WEBRTC)
    target_sources(a11_net PRIVATE ${A11_NET_WEBRTC_SOURCES})
endif ()
add_library(a11::net ALIAS a11_net)
set_target_properties(a11_net PROPERTIES EXPORT_NAME net)
target_include_directories(
        a11_net PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_net PUBLIC a11::concurrency a11::data $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        PRIVATE $<BUILD_INTERFACE:a11::obs> $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> a11::warnings
)
if (A11_BUILD_WEBRTC)
    target_link_libraries(a11_net PUBLIC LibDataChannel::LibDataChannel)
endif ()
if (A11_BUILD_HTTP)
    target_link_libraries(a11_net PRIVATE a11::nghttp2 uvw::uvw $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log>
            OpenSSL::SSL OpenSSL::Crypto)
endif ()
# ChunkStoreWriter tees persisted fragments through this transport interface.
# a11_net itself deliberately has no dependency on stores, so this is acyclic.
target_link_libraries(a11_stores PUBLIC a11::net)

add_library(a11_nodes STATIC ${A11_NODES_SOURCES})
add_library(a11::nodes ALIAS a11_nodes)
set_target_properties(a11_nodes PROPERTIES EXPORT_NAME nodes)
target_include_directories(
        a11_nodes PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_nodes PUBLIC a11::data a11::stores $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        PRIVATE a11::warnings)

add_library(a11_actions STATIC ${A11_ACTIONS_SOURCES})
add_library(a11::actions ALIAS a11_actions)
set_target_properties(a11_actions PROPERTIES EXPORT_NAME actions)
target_include_directories(
        a11_actions PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_actions PUBLIC a11::data a11::net a11::nodes a11::stores
        $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        # a11::obs (and the OpenTelemetry it privately links) is an
        # implementation detail carried only in the build interface for now, so
        # it stays out of the exported/installed package. The full relocatable
        # install + OTLP story lands in a later phase.
        PRIVATE $<BUILD_INTERFACE:a11::obs> $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> a11::warnings)

add_library(a11_service STATIC ${A11_SERVICE_SOURCES})
add_library(a11::service ALIAS a11_service)
set_target_properties(a11_service PROPERTIES EXPORT_NAME service)
target_include_directories(
        a11_service PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_service PUBLIC a11::actions a11::data a11::net a11::nodes
        $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status> $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        PRIVATE $<BUILD_INTERFACE:a11::obs> $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> a11::warnings
        nlohmann_json::nlohmann_json)

# Action method bodies call Session hooks, while Session owns Actions. Static
# archives tolerate this intentional interface cycle; final consumers link
# both public components through their transitive target metadata.
target_link_libraries(a11_actions PUBLIC a11::service)

if (A11_BUILD_PYTHON)
    # PEP 517 frontends run CMake through a temporary build environment. Cache a
    # stable interpreter from its Python root so an editable rebuild remains
    # valid after that environment is removed.
    if (SKBUILD AND Python3_ROOT_DIR)
        set(A11_STABLE_PYTHON_EXECUTABLE "${Python3_ROOT_DIR}/bin/python")
        if (EXISTS "${A11_STABLE_PYTHON_EXECUTABLE}")
            set(PYTHON_EXECUTABLE "${A11_STABLE_PYTHON_EXECUTABLE}" CACHE PATH ""
                    FORCE)
            set(Python_EXECUTABLE "${A11_STABLE_PYTHON_EXECUTABLE}" CACHE PATH ""
                    FORCE)
            set(Python3_EXECUTABLE "${A11_STABLE_PYTHON_EXECUTABLE}" CACHE PATH ""
                    FORCE)
        endif ()
        unset(A11_STABLE_PYTHON_EXECUTABLE)
    endif ()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
    if (SKBUILD)
        set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
                pybind11
                GIT_REPOSITORY https://github.com/pybind/pybind11.git
                GIT_TAG v3.0.1
                GIT_SHALLOW TRUE
                SYSTEM)
        FetchContent_MakeAvailable(pybind11)
    else ()
        find_package(pybind11 3.0 CONFIG REQUIRED)
    endif ()

    if (SKBUILD)
        # The wheel build vendors pybind11 through FetchContent, whose include tree
        # is clean: it holds no competing Abseil, so there is nothing to fence off.
        # Here pybind11::pybind11_headers is an ALIAS (set_target_properties is
        # illegal on it) and its INTERFACE_INCLUDE_DIRECTORIES are build/install
        # generator expressions rather than a plain path. Point pybind11_abseil's
        # directory-wide include_directories() straight at the vendored headers and
        # leave the alias untouched.
        set(pybind11_INCLUDE_DIRS
                "${pybind11_SOURCE_DIR}/include;${Python3_INCLUDE_DIRS}")
    else ()
        # Some package managers expose pybind11 through a broad include directory
        # that also contains a different Abseil release. Restrict that imported
        # target to a generated pybind11-only overlay so <absl/...> always resolves
        # to the pinned source tree above.
        get_target_property(
                A11_PYBIND11_INCLUDE_DIRS pybind11::pybind11_headers
                INTERFACE_INCLUDE_DIRECTORIES)
        set(A11_PYBIND11_INCLUDE_OVERLAY
                "${CMAKE_CURRENT_BINARY_DIR}/pybind11_include_overlay")
        file(MAKE_DIRECTORY "${A11_PYBIND11_INCLUDE_OVERLAY}")
        foreach (A11_PYBIND11_INCLUDE_DIR IN LISTS A11_PYBIND11_INCLUDE_DIRS)
            if (EXISTS "${A11_PYBIND11_INCLUDE_DIR}/pybind11" AND
                    NOT EXISTS "${A11_PYBIND11_INCLUDE_OVERLAY}/pybind11")
                file(CREATE_LINK
                        "${A11_PYBIND11_INCLUDE_DIR}/pybind11"
                        "${A11_PYBIND11_INCLUDE_OVERLAY}/pybind11"
                        SYMBOLIC)
            endif ()
        endforeach ()
        set_target_properties(
                pybind11::pybind11_headers PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${A11_PYBIND11_INCLUDE_OVERLAY}")
        # pybind11_abseil also consumes this variable through a directory-wide
        # include_directories() call, so narrow it to the same overlay.
        set(pybind11_INCLUDE_DIRS
                "${A11_PYBIND11_INCLUDE_OVERLAY};${Python3_INCLUDE_DIRS}")
        unset(A11_PYBIND11_INCLUDE_DIR)
        unset(A11_PYBIND11_INCLUDE_DIRS)
    endif ()

    if (NOT TARGET pybind11_abseil::status_casters)
        include(FetchContent)
        set(USE_SYSTEM_ABSEIL ON CACHE BOOL "" FORCE)
        set(USE_SYSTEM_PYBIND ON CACHE BOOL "" FORCE)
        # pybind11_abseil follows the top-level BUILD_TESTING option. Suppress its
        # own suite while it is added, then restore A11's setting for our tests.
        set(A11_SAVED_BUILD_TESTING "${BUILD_TESTING}")
        set(BUILD_TESTING OFF)
        if (A11_PYBIND11_ABSEIL_SOURCE_DIR)
            FetchContent_Declare(
                    pybind11_abseil SOURCE_DIR "${A11_PYBIND11_ABSEIL_SOURCE_DIR}")
        else ()
            FetchContent_Declare(
                    pybind11_abseil
                    GIT_REPOSITORY https://github.com/pybind/pybind11_abseil.git
                    GIT_TAG dba6e38b79d93bc718e28b8f8ea2e8b00b60cdec
                    GIT_SHALLOW TRUE)
        endif ()
        FetchContent_MakeAvailable(pybind11_abseil)

        # pybind11 is commonly installed in the same prefix as a system Abseil.
        # Its project adds that broad prefix as a regular -I path, which otherwise
        # wins over transitive -isystem paths and mixes Abseil LTS ABIs. Force all
        # of the dependency's compiled targets to see our pinned headers first.
        foreach (A11_PYBIND11_ABSEIL_TARGET IN ITEMS
                import_status_module
                ok_status_singleton
                ok_status_singleton_lib
                ok_status_singleton_pyinit_google3
                py_base_utilities
                register_status_bindings
                status_from_core_py_exc
                status_from_py_exc
                status_py_extension_stub
                status_pyinit_google3
                utils_pybind11_absl
                void_ptr_from_capsule)
            if (TARGET ${A11_PYBIND11_ABSEIL_TARGET})
                target_include_directories(
                        ${A11_PYBIND11_ABSEIL_TARGET}
                        BEFORE PRIVATE ${abseil-cpp_SOURCE_DIR})
            endif ()
        endforeach ()
        unset(A11_PYBIND11_ABSEIL_TARGET)

        set(BUILD_TESTING "${A11_SAVED_BUILD_TESTING}")
        unset(A11_SAVED_BUILD_TESTING)
    endif ()

    # pybind11_abseil is an external dependency. Keep warnings in its headers
    # from being attributed to A11 consumers, including warnings-as-errors builds.
    get_target_property(A11_STATUS_CASTERS_TARGET
            pybind11_abseil::status_casters ALIASED_TARGET)
    if (A11_STATUS_CASTERS_TARGET)
        set_target_properties(${A11_STATUS_CASTERS_TARGET} PROPERTIES SYSTEM ON)
    endif ()
    unset(A11_STATUS_CASTERS_TARGET)

    pybind11_add_module(a11_python MODULE ${A11_PYTHON_SOURCES})
    set_target_properties(a11_python PROPERTIES OUTPUT_NAME "_native")
    # Direct CMake builds place the module beside the Python sources for a fast
    # edit/build/test loop. scikit-build keeps every ABI in its isolated build
    # directory and installs it into the wheel, avoiding cross-ABI artifacts in
    # the shared source package during matrix builds.
    if (NOT SKBUILD)
        set_target_properties(a11_python PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../a11")
    endif ()
    target_include_directories(a11_python PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Homebrew installs pybind11 and Abseil under the same include prefix. Put
    # the pinned Abseil first so binding translation units cannot accidentally
    # compile against a different LTS inline namespace.
    target_include_directories(
            a11_python SYSTEM BEFORE PRIVATE ${abseil-cpp_SOURCE_DIR})
    target_link_libraries(
            a11_python
            PRIVATE a11::actions a11::concurrency a11::core a11::data a11::net
            a11::nodes a11::obs a11::service a11::stores
            pybind11_abseil::status_casters a11::warnings
            absl::failure_signal_handler absl::symbolize)

    # pybind11_abseil's status caster imports this extension by its canonical
    # package path. Keep direct-CMake outputs beside the source package and
    # install both runtime modules into editable/wheel install trees.
    if (TARGET status_py_extension_stub)
        if (NOT SKBUILD)
            set_target_properties(status_py_extension_stub PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY
                    "${CMAKE_CURRENT_SOURCE_DIR}/../pybind11_abseil")
        endif ()
    endif ()
    if (TARGET ok_status_singleton)
        if (NOT SKBUILD)
            set_target_properties(ok_status_singleton PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY
                    "${CMAKE_CURRENT_SOURCE_DIR}/../pybind11_abseil")
        endif ()
    endif ()
    install(TARGETS a11_python LIBRARY DESTINATION a11 COMPONENT python)
    install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/../a11/_native.pyi"
            "${CMAKE_CURRENT_SOURCE_DIR}/../a11/py.typed"
            DESTINATION a11 COMPONENT python)
    if (NOT SKBUILD)
        add_custom_target(
                a11_python_stubs
                COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_stubs.py"
                DEPENDS a11_python
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
                COMMENT "Generating a11/_native.pyi")
    endif ()
    if (TARGET status_py_extension_stub)
        install(TARGETS status_py_extension_stub
                LIBRARY DESTINATION pybind11_abseil COMPONENT python)
    endif ()
    if (TARGET ok_status_singleton)
        install(TARGETS ok_status_singleton
                LIBRARY DESTINATION pybind11_abseil COMPONENT python)
    endif ()
endif ()

install(
        TARGETS a11_warnings a11_core a11_data a11_concurrency a11_stores a11_net
        a11_nodes a11_actions a11_service Thread
        EXPORT a11Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
        DIRECTORY a11/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a11
        FILES_MATCHING PATTERN "*.h")
install(
        DIRECTORY thread/thread/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thread
        FILES_MATCHING PATTERN "*.h"
        PATTERN "thread_pool.h" EXCLUDE)
configure_package_config_file(
        cmake/a11Config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/a11Config.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a11)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/a11ConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
install(
        EXPORT a11Targets
        NAMESPACE a11::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a11)
install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/a11Config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/a11ConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a11)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
