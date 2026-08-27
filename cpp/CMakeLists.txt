cmake_minimum_required(VERSION 3.28)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif ()

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" A11_VERSION_STRING
        LIMIT_COUNT 1)
if (NOT A11_VERSION_STRING MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain one semantic version")
endif ()

project(
        a11
        VERSION ${A11_VERSION_STRING}
        DESCRIPTION "A11 streaming action runtime"
        LANGUAGES C CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

option(A11_BUILD_PYTHON "Build the pybind11 extension" ON)
# The standalone language tool is not part of a wheel: the wheel installs only the
# `python` component, so building it there would be work thrown away. Everywhere
# else it is on, because it costs seconds and is what editors and CI run.
if (DEFINED SKBUILD)
    set(A11_FLOW_TOOL_DEFAULT OFF)
else ()
    set(A11_FLOW_TOOL_DEFAULT ON)
endif ()
option(A11_BUILD_FLOW_TOOL "Build the standalone a11-flow language tool"
        ${A11_FLOW_TOOL_DEFAULT})
option(A11_BUILD_BENCH "Build the native benchmark binary a11_bench" OFF)
option(A11_BUILD_HTTP "Build HTTP wire streams and servers" ON)
option(A11_BUILD_FLOW_ACTIONS
        "Build the Flow standard library of Actions (files, time, processes)"
        ON)
option(A11_BUILD_WEBRTC "Build WebRTC wire streams and signalling" ON)
option(A11_BUILD_REDIS "Build the Redis client and Redis ChunkStore" ON)
option(A11_BUILD_SQLITE "Build the SQLite ChunkStore" ON)
option(A11_BUILD_AUDIO
        "Build PortAudio capture and whisper.cpp speech recognition" ON)
option(A11_FETCH_MISSING_DEPS "Fetch required dependencies not installed" ON)
# Replaces the C library's malloc in A11's native executables. Worth ~+25%
# throughput on the server benchmark and nothing else changes; see
# bench/PERF_PLAN.md for the measurements behind the per-platform default.
# "auto" means tcmalloc on Linux, mimalloc on macOS, and the system allocator
# anywhere else. Deliberately *not* applied to the Python extension -- see the
# a11_allocator block below, and a11/allocator.py for what happens instead.
set(A11_ALLOCATOR "auto" CACHE STRING
        "Allocator for A11's native executables: auto, mimalloc, tcmalloc, system")
set_property(CACHE A11_ALLOCATOR PROPERTY STRINGS
        auto mimalloc tcmalloc system)
option(A11_REQUIRE_STATIC_DEPS
        "Fail configuration when a non-system dependency is only shared" ON)
option(A11_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(A11_WITH_OTLP_HTTP
        "Build the native OTLP/HTTP (JSON) span exporter (needs libcurl)" ON)
set(A11_PYBIND11_ABSEIL_SOURCE_DIR "" CACHE PATH
        "Optional local pybind11_abseil checkout")
# Semicolon- or comma-separated sanitizer list for debugging memory/UB bugs,
# e.g. "address" or "address;undefined". Applied to every target configured
# under this project (A11 and fetched deps) plus the link line, so the whole
# extension shares one sanitizer runtime. Loading the resulting _native.so from
# a non-instrumented python needs the runtime preloaded, e.g.
#   LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
#   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
#   A11_DISABLE_FAILURE_SIGNAL_HANDLER=1 python -m pytest ...
set(A11_SANITIZE "" CACHE STRING
        "Sanitizers to enable (e.g. address, undefined, 'address;undefined')")

# A11's own code does not throw, and these two functions say so to the compiler.
#
# a11_disallow_exceptions(<targets>) compiles a target's sources with
# -fno-exceptions; a11_allow_exceptions(<sources>) hands exceptions back to the
# few translation units that genuinely need them. Exceptions survive only at the
# boundaries that call code which may throw -- boost fiber's internals,
# libdatachannel's API, a callable a caller handed us, and pybind11 -- and each of
# those files names itself in a a11_allow_exceptions() call beside its target.
# Everywhere else a `throw` is a compile error, which is the point: it keeps the
# boundary from drifting inward one convenient catch at a time.
#
# **There is no option to turn this off.** It is not a build configuration but
# part of what A11 is: its errors are `absl::Status` and its invariant violations
# are `CHECK`s, so a build in which A11 could let an exception out would be a
# build whose error handling nobody has written. A switch would also be a promise
# that both halves stay correct, and the half nobody compiles is the half that
# rots -- the `try` that a future edit puts in a hot path would be invisible until
# somebody flipped the flag back.
#
# This constrains A11's build, not its consumers'. -fno-exceptions is a per
# translation unit flag on A11's own sources; a consumer compiles its own
# translation units however it likes, links normally against these objects, and
# may throw freely in its own frames -- A11 simply catches at the boundary where
# it adopts a callable rather than letting one unwind through its internals. The
# public headers compile both ways, which a11_header_canary pins in the direction
# that is easy to break.
#
# The opt-out is per source file rather than per target because the split does not
# follow target boundaries: a11_net holds both the two libdatachannel translation
# units and the HTTP transport, and splitting the target to express that would put
# a build-system seam where there is no design seam. Source COMPILE_OPTIONS are
# emitted after the target's, so the -fexceptions of an opted-out file wins over
# its target's -fno-exceptions.
#
# Both are PRIVATE, so neither reaches the installed export set nor a consumer:
# how A11 was compiled is A11's business.
#
# Source-file properties are directory-scoped, so call a11_allow_exceptions from
# the directory that defines the target consuming the file -- which for every A11
# library is this one, and for the bundled thread library is thread/.
#
# RTTI is deliberately left alone: pybind11 and OpenTelemetry both need it, and
# the serialization registry keys on std::type_index.
function(a11_disallow_exceptions)
    foreach (a11_target IN LISTS ARGN)
        target_compile_options(
                ${a11_target} PRIVATE
                $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fno-exceptions>)
    endforeach ()
endfunction()

function(a11_allow_exceptions)
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS
                 $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fexceptions>)
endfunction()

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

# Fails configuration when a resolved dependency artifact lives outside the
# bootstrapped deps prefix (CMAKE_PREFIX_PATH). The config-mode find_package
# calls below already pin their search to the prefix with NO_DEFAULT_PATH; this
# covers the pkg-config-driven finds (OpenSSL, nghttp2) whose search path CMake
# cannot restrict as cleanly, guaranteeing that no system-installed library is
# ever linked on any platform. Enforced only in the hermetic static-deps mode.
function(a11_require_from_prefix what artifact_path)
    if (NOT A11_REQUIRE_STATIC_DEPS)
        return()
    endif ()
    if (NOT artifact_path)
        message(FATAL_ERROR "${what}: expected a resolved path, got none")
    endif ()
    if (NOT CMAKE_PREFIX_PATH)
        message(FATAL_ERROR
                "${what} resolved to '${artifact_path}' but CMAKE_PREFIX_PATH is "
                "empty; a hermetic build must point it at the bootstrapped deps "
                "prefix so no system library can be linked.")
    endif ()
    file(REAL_PATH "${artifact_path}" A11_ARTIFACT_REAL)
    set(A11_ARTIFACT_IN_PREFIX FALSE)
    foreach (A11_PREFIX_ENTRY IN LISTS CMAKE_PREFIX_PATH)
        file(REAL_PATH "${A11_PREFIX_ENTRY}" A11_PREFIX_REAL)
        string(FIND "${A11_ARTIFACT_REAL}" "${A11_PREFIX_REAL}/" A11_PREFIX_POS)
        if (A11_PREFIX_POS EQUAL 0)
            set(A11_ARTIFACT_IN_PREFIX TRUE)
        endif ()
    endforeach ()
    if (NOT A11_ARTIFACT_IN_PREFIX)
        message(FATAL_ERROR
                "${what} resolved to '${A11_ARTIFACT_REAL}', outside the pinned "
                "deps prefix (CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}'). Refusing "
                "to link a system library; rebuild the deps prefix instead.")
    endif ()
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
    # A11's macOS floor is 14.4 -- the minimum for os_sync_wait_on_address, which
    # the Boost.Fiber futex spinlock (and the deps prefix's Boost) require. Pin it
    # here, before any target or fetched dependency is configured, so an unset
    # target does not silently inherit an arbitrary host/SDK default that drops
    # below 14.4 and fails deep in Boost.Fiber's headers with "futex not supported
    # on this platform". An explicit lower value is a hard configure error instead.
    # A higher value (a newer prefix) is fine and passes through untouched.
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING
                "Minimum macOS version (Boost.Fiber futex spinlock needs >= 14.4)"
                FORCE)
    elseif (CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.4)
        message(FATAL_ERROR
                "A11 on macOS needs CMAKE_OSX_DEPLOYMENT_TARGET >= 14.4 for the "
                "Boost.Fiber futex spinlock (got ${CMAKE_OSX_DEPLOYMENT_TARGET}). "
                "Rebuild the deps prefix at the same target.")
    endif ()
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

# Sanitizer wiring. Set directory-wide before any FetchContent_MakeAvailable()
# below so fetched dependencies are instrumented too; a partially instrumented
# process gives ASan false negatives across the boundary. Keep frame pointers so
# the reports carry readable stacks.
if (A11_SANITIZE)
    string(REPLACE ";" "," A11_SANITIZE_FLAG "${A11_SANITIZE}")
    message(STATUS "A11 sanitizers enabled: ${A11_SANITIZE_FLAG}")
    add_compile_options(-fsanitize=${A11_SANITIZE_FLAG}
            -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${A11_SANITIZE_FLAG})
endif ()

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
    # The native OTLP/HTTP exporter posts OTLP-JSON via libcurl. The deps prefix
    # supplies a static libcurl (built against A11's static OpenSSL); resolve it
    # in CONFIG mode restricted to that prefix so a system libcurl is never
    # picked up -- neither on macOS nor Linux, and neither for wheels nor local
    # builds -- and require it to be static like every other bundled dependency.
    find_package(CURL CONFIG REQUIRED NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
    a11_require_static_target(CURL::libcurl)
endif ()

# Pin nlohmann_json to the bootstrapped prefix (like Boost below). Its type
# shows up in a11_core's public interface (an inline-namespaced json_abi_v3_x_y
# symbol), so a system copy at a different version is an ABI mismatch, not just a
# version drift. NO_DEFAULT_PATH keeps the search off system prefixes entirely.
find_package(nlohmann_json CONFIG REQUIRED
        NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME OFF)

find_package(Boost 1.82 REQUIRED CONFIG COMPONENTS context fiber thread
        NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
a11_require_static_target(Boost::context)
a11_require_static_target(Boost::fiber)
a11_require_static_target(Boost::thread)

# Boost.Fiber's spinlock policy. Chosen per platform, and applied on all of them.
#
# The default differs by platform because the right answer does. On macOS the
# adaptive futex spinlock wins. On Linux the plain `spinlock_ttas` -- which spins
# and yields but never sleeps -- is **measurably better**: the server benchmark
# gives 19.88k ops/s against 18.38k at 256 clients, 21.23k against 19.70k at 64,
# same sign at every client count (bench/PERF_PLAN.md). The critical sections
# under this lock are ready-queue pushes and pops, and a futex syscall costs more
# than spinning through one. `spinlock_ttas::lock()` being the profile's largest
# entry at ~10% is therefore not a bug to fix by sleeping instead; that was tried.
#
# This block used to sit under `if (APPLE)`, which produced the same Linux
# behaviour by accident -- Boost's own fallback -- while leaving no way to select
# or even see the choice there. It is explicit now, on both platforms, so the
# experiment above is one `-DA11_FIBER_SPINLOCK=` away rather than a day of
# rebuilding Boost to find out what you were already running.
#
# Precedence: an explicit -DA11_FIBER_SPINLOCK wins (build frontends that cannot
# pass the environment through -- e.g. uv, which overwrites CMAKE_PREFIX_PATH --
# inject it via CMAKE_ARGS like the other discovery flags), then the environment
# (CMake presets set it there), then the per-platform default. Whatever resolves
# here MUST match the value the deps prefix's Boost.Fiber was built with: Boost
# selects the spinlock in headers, so the choices are different types of
# different sizes, and mixing them changes boost::fibers::mutex's layout between
# the prefix's library and A11's own translation units. The deps bootstrap keys
# its stamp on the same variable so a switch rebuilds Boost rather than silently
# reusing a prefix that no longer matches.
#
# BOOST_FIBERS_SPINLOCK_TTAS names the plain one. Boost has no such macro -- it
# is Boost's `else` branch -- so the define lands nowhere, which is exactly the
# point: both sides pass the same unrecognised macro and both fall through to the
# same type. Naming it beats leaving it blank and hoping the two sides agree.
if (NOT A11_FIBER_SPINLOCK)
    if (DEFINED ENV{A11_FIBER_SPINLOCK})
        set(A11_FIBER_SPINLOCK "$ENV{A11_FIBER_SPINLOCK}")
    elseif (APPLE)
        set(A11_FIBER_SPINLOCK BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX)
    else ()
        set(A11_FIBER_SPINLOCK BOOST_FIBERS_SPINLOCK_TTAS)
    endif ()
endif ()
if (NOT A11_FIBER_SPINLOCK STREQUAL "BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX"
        AND NOT A11_FIBER_SPINLOCK STREQUAL "BOOST_FIBERS_SPINLOCK_TTAS_FUTEX"
        AND NOT A11_FIBER_SPINLOCK STREQUAL "BOOST_FIBERS_SPINLOCK_TTAS")
    message(FATAL_ERROR
            "A11_FIBER_SPINLOCK must be BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX,"
            " BOOST_FIBERS_SPINLOCK_TTAS_FUTEX or BOOST_FIBERS_SPINLOCK_TTAS"
            " (got ${A11_FIBER_SPINLOCK})")
endif ()
add_compile_definitions(${A11_FIBER_SPINLOCK})

# libuv, for A11's one event loop. Found unconditionally, and no longer under
# `if (A11_BUILD_HTTP OR A11_BUILD_REDIS)`: HTTP is where the loop grew up, not
# what needs it. A clock, a directory watch and a listening socket all want the
# same loop, so a build with HTTP off but the Flow standard library on would
# otherwise have to grow a second implementation of every timer behind a build
# flag -- which is two things to keep correct and one of them never exercised.
find_package(uvw CONFIG REQUIRED NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
a11_require_static_target(uvw::uvw)
a11_require_static_target(uvw::uv_a)

# The process allocator for A11's own executables.
#
# glibc's malloc was ~16% of CPU on the server benchmark and its arena contention
# cost more than that again; replacing it is worth ~+25% throughput for no change
# to A11's code at all. jemalloc, mimalloc and tcmalloc all measured within noise
# of each other and all beat glibc by 20-30%, so which one is a per-platform
# choice recorded in A11_ALLOCATOR below rather than a matter of principle.
#
# Two deliberate exclusions:
#  - Sanitizer builds. ASan and TSan install their own allocators to detect
#    exactly what this would be replacing.
#  - The Python extension. `_native.so` is dlopened into an interpreter that has
#    already allocated through libc, and a module that replaced `free` for the
#    whole process would eventually be handed a libc pointer. The wheel instead
#    carries the shared library for preloading, and a11/allocator.py activates
#    it where A11 launches its own processes; see the install rule beside
#    a11_python. This is a property of how a process starts, so it cannot be
#    decided from inside one.
#
# This is an executable-level choice: it goes on the executables and never into
# a11::core's link interface.
add_library(a11_allocator INTERFACE)
add_library(a11::allocator ALIAS a11_allocator)

# What "auto" prefers, and what it settles for.
#
# tcmalloc on Linux and mimalloc on macOS is how they measured: tcmalloc took
# 19.65k ops/s against mimalloc's 18.65k at 256 clients on the Linux reference
# box, while mimalloc is the one with first-class macOS support. Both beat glibc
# by more than they differ from each other -- which is the reason "auto" is a
# *preference order* rather than a requirement. Asking for a specific allocator
# and not getting it is an error; asking for the best available and finding the
# second best is not, and it must not be, because the deps prefix
# scripts/bootstrap_wheel_deps.sh builds carries mimalloc on every platform and
# tcmalloc on none. A hard requirement here failed every Linux configure in CI
# (wheels, the C++ archive, the stub check) with "needs libtcmalloc_minimal".
if (A11_ALLOCATOR STREQUAL "auto")
    if (APPLE)
        set(A11_ALLOCATOR_CANDIDATES mimalloc)
    elseif (UNIX)
        set(A11_ALLOCATOR_CANDIDATES tcmalloc mimalloc)
    else ()
        set(A11_ALLOCATOR_CANDIDATES system)
    endif ()
else ()
    set(A11_ALLOCATOR_CANDIDATES "${A11_ALLOCATOR}")
endif ()
if (NOT A11_ALLOCATOR MATCHES "^(auto|mimalloc|tcmalloc|system)$")
    message(FATAL_ERROR
            "A11_ALLOCATOR must be auto, mimalloc, tcmalloc or system "
            "(got ${A11_ALLOCATOR})")
endif ()
if (A11_SANITIZE AND NOT A11_ALLOCATOR_CANDIDATES STREQUAL "system")
    message(STATUS
            "A11_SANITIZE is set: using the system allocator so the sanitizer "
            "keeps its own")
    set(A11_ALLOCATOR_CANDIDATES system)
endif ()

# Is each candidate actually here? The probes look only in the deps prefix,
# because A11 links its allocator statically and a system-wide shared one is not
# what it is asking for, and each runs only when its candidate is in the running,
# so asking for one allocator never depends on another's package config being
# loadable. The branches below reuse both results.
#
# The tcmalloc archive is named exactly: a bare `tcmalloc_minimal` finds the
# shared library in a prefix that has both.
if ("tcmalloc" IN_LIST A11_ALLOCATOR_CANDIDATES)
    find_library(A11_TCMALLOC_LIBRARY
            NAMES "${CMAKE_STATIC_LIBRARY_PREFIX}tcmalloc_minimal${CMAKE_STATIC_LIBRARY_SUFFIX}"
            tcmalloc_minimal
            PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib
            NO_DEFAULT_PATH)
endif ()
if ("mimalloc" IN_LIST A11_ALLOCATOR_CANDIDATES)
    find_package(mimalloc 2.1 CONFIG QUIET
            NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
endif ()

set(A11_RESOLVED_ALLOCATOR "")
foreach (A11_ALLOCATOR_CANDIDATE IN LISTS A11_ALLOCATOR_CANDIDATES)
    set(A11_ALLOCATOR_MISSING "")
    if (A11_ALLOCATOR_CANDIDATE STREQUAL "system")
        set(A11_RESOLVED_ALLOCATOR system)
    elseif (A11_ALLOCATOR_CANDIDATE STREQUAL "tcmalloc")
        if (A11_TCMALLOC_LIBRARY MATCHES "\\.a$")
            set(A11_RESOLVED_ALLOCATOR tcmalloc)
        else ()
            # One string, not three: expanded into the message below, a list
            # would arrive with semicolons between its parts.
            string(CONCAT A11_ALLOCATOR_MISSING
                    "libtcmalloc_minimal.a in the deps prefix; rebuild it with "
                    "scripts/bootstrap_wheel_deps.sh, or configure with "
                    "-DA11_ALLOCATOR=mimalloc or system")
        endif ()
    elseif (A11_ALLOCATOR_CANDIDATE STREQUAL "mimalloc")
        if (mimalloc_FOUND OR A11_FETCH_MISSING_DEPS)
            set(A11_RESOLVED_ALLOCATOR mimalloc)
        else ()
            string(CONCAT A11_ALLOCATOR_MISSING
                    "mimalloc >= 2.1; install it into the deps prefix, enable "
                    "A11_FETCH_MISSING_DEPS, or configure with "
                    "-DA11_ALLOCATOR=system")
        endif ()
    endif ()
    if (A11_RESOLVED_ALLOCATOR)
        break ()
    endif ()
    # An explicit choice that cannot be honoured is an error; "auto" moves on.
    if (NOT A11_ALLOCATOR STREQUAL "auto")
        message(FATAL_ERROR
                "A11_ALLOCATOR=${A11_ALLOCATOR_CANDIDATE} needs "
                "${A11_ALLOCATOR_MISSING}")
    endif ()
    message(STATUS
            "A11 allocator: ${A11_ALLOCATOR_CANDIDATE} is not available here, "
            "trying the next preference")
endforeach ()
if (NOT A11_RESOLVED_ALLOCATOR)
    string(REPLACE ";" ", " A11_ALLOCATOR_TRIED "${A11_ALLOCATOR_CANDIDATES}")
    message(STATUS
            "A11 allocator: none of ${A11_ALLOCATOR_TRIED} is available; "
            "falling back to the system allocator")
    set(A11_RESOLVED_ALLOCATOR system)
endif ()
message(STATUS "A11 allocator: ${A11_RESOLVED_ALLOCATOR}")

# Set when a preloadable shared build is available for the wheel to carry.
set(A11_ALLOCATOR_SHARED_LIBRARY "")

if (A11_RESOLVED_ALLOCATOR STREQUAL "mimalloc")
    # `mimalloc_FOUND` is the probe above; installed or fetched are the only two
    # ways to get here.
    if (mimalloc_FOUND)
        a11_require_static_target(mimalloc-static)
        # Linked as `mimalloc.o` rather than the archive on purpose: an archive
        # member is only pulled in to satisfy an undefined symbol, and `malloc`
        # is never undefined -- libc always has one -- so linking
        # `libmimalloc.a` silently leaves the override off. An explicitly listed
        # object file's definitions are always included.
        # MIMALLOC_OBJECT_DIR is set by mimalloc's own package config -- but only
        # correctly when the libdir is literally `lib`, because the config gets
        # there by string-replacing "/lib/cmake/" in its own install path. A
        # prefix whose libdir is lib64 (GNUInstallDirs' default on non-Debian
        # 64-bit Linux, which is what manylinux is) leaves that replace unmatched
        # and MIMALLOC_OBJECT_DIR comes out as the cmake directory itself. So
        # derive the layout-independent candidate too: the object sits in a
        # directory named like the config's, two levels up from it
        # (<libdir>/mimalloc-2.1 next to <libdir>/cmake/mimalloc-2.1).
        get_filename_component(A11_MIMALLOC_VERSION_DIR "${mimalloc_DIR}" NAME)
        get_filename_component(A11_MIMALLOC_LIBDIR "${mimalloc_DIR}/../.." ABSOLUTE)
        find_file(A11_MIMALLOC_OBJECT mimalloc.o
                PATHS "${MIMALLOC_OBJECT_DIR}"
                "${A11_MIMALLOC_LIBDIR}/${A11_MIMALLOC_VERSION_DIR}"
                "${A11_MIMALLOC_LIBDIR}"
                NO_DEFAULT_PATH)
        if (NOT A11_MIMALLOC_OBJECT)
            message(FATAL_ERROR
                    "mimalloc was found at ${MIMALLOC_OBJECT_DIR} but without "
                    "mimalloc.o; rebuild the deps prefix with "
                    "scripts/bootstrap_wheel_deps.sh, or configure with "
                    "-DA11_ALLOCATOR=system")
        endif ()
        a11_require_from_prefix(mimalloc "${A11_MIMALLOC_OBJECT}")
        target_link_libraries(a11_allocator INTERFACE "${A11_MIMALLOC_OBJECT}")
        # The shared build, for the wheel to carry as a preloadable library. Not
        # linked against anything. Absent from a prefix built before this was
        # added, which is not an error -- the executables have their allocator
        # either way.
        if (TARGET mimalloc)
            get_target_property(A11_ALLOCATOR_SHARED_LIBRARY
                    mimalloc IMPORTED_LOCATION)
            if (NOT A11_ALLOCATOR_SHARED_LIBRARY)
                get_target_property(A11_ALLOCATOR_SHARED_LIBRARY
                        mimalloc IMPORTED_LOCATION_RELEASE)
            endif ()
        endif ()
    else ()
        # Fetched, because the resolution above only chose mimalloc when it was
        # either installed or fetchable.
        #
        # Only the object and shared builds: the archive would be an install rule
        # for a file nothing ever builds (A11 links the object, so the static
        # target has no dependent), which fails `cmake --install` rather than the
        # build. EXCLUDE_FROM_ALL keeps mimalloc's own install rules out of
        # A11's install altogether -- a dependency consumed as objects has no
        # business putting headers and archives in A11's prefix.
        set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_SHARED ON CACHE BOOL "" FORCE)
        set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
                mimalloc
                GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
                GIT_TAG v2.1.7
                GIT_SHALLOW TRUE
                EXCLUDE_FROM_ALL
                SYSTEM)
        FetchContent_MakeAvailable(mimalloc)
        # In-tree, mimalloc's own object library stands in for the installed
        # mimalloc.o -- but as `$<TARGET_OBJECTS:>` and not as a link to the
        # target. An OBJECT library's objects reach only the targets that link it
        # *directly*; routed through this INTERFACE library they are silently
        # dropped, which builds and links cleanly and leaves the allocator
        # unreplaced. The generator expression names the object files themselves,
        # which do propagate (and carry their own build dependency).
        target_link_libraries(
                a11_allocator INTERFACE $<TARGET_OBJECTS:mimalloc-obj>)
        # EXCLUDE_FROM_ALL means the shared build needs an explicit reason to
        # exist, since nothing links it.
        if (A11_BUILD_PYTHON AND TARGET mimalloc)
            add_custom_target(a11_allocator_shared ALL DEPENDS mimalloc)
            set(A11_ALLOCATOR_SHARED_LIBRARY "$<TARGET_FILE:mimalloc>")
        endif ()
    endif ()
elseif (A11_RESOLVED_ALLOCATOR STREQUAL "tcmalloc")
    # gperftools' minimal build: tcmalloc without the heap profiler and leak
    # checker, which is all that is wanted here and avoids a libunwind
    # dependency. No object-file trick needed, unlike mimalloc: the archive
    # member that defines `malloc` also defines `operator new`, which *is* an
    # undefined symbol in any C++ program, so the member is always pulled in.
    # `nm <binary> | grep -c tc_malloc` is still the check.
    #
    # `A11_TCMALLOC_LIBRARY` is the archive the probe above resolved -- it only
    # resolves to `.a`, since A11 links its allocator statically.
    a11_require_from_prefix(tcmalloc "${A11_TCMALLOC_LIBRARY}")
    target_link_libraries(a11_allocator INTERFACE "${A11_TCMALLOC_LIBRARY}")
    find_library(A11_TCMALLOC_SHARED_LIBRARY
            NAMES tcmalloc_minimal
            PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib
            NO_DEFAULT_PATH
            NO_CACHE)
    file(GLOB A11_TCMALLOC_SHARED_CANDIDATES
         "${CMAKE_PREFIX_PATH}/lib/libtcmalloc_minimal.so"
         "${CMAKE_PREFIX_PATH}/lib/libtcmalloc_minimal.dylib")
    if (A11_TCMALLOC_SHARED_CANDIDATES)
        list(GET A11_TCMALLOC_SHARED_CANDIDATES 0 A11_ALLOCATOR_SHARED_LIBRARY)
    endif ()
endif ()

if (A11_BUILD_REDIS)
    find_package(hiredis 1.3 CONFIG QUIET
            NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
    if (NOT hiredis_FOUND AND A11_FETCH_MISSING_DEPS)
        set(DISABLE_TESTS ON CACHE BOOL "" FORCE)
        set(ENABLE_SSL OFF CACHE BOOL "" FORCE)
        set(ENABLE_SSL_TESTS OFF CACHE BOOL "" FORCE)
        set(ENABLE_ASYNC_TESTS OFF CACHE BOOL "" FORCE)
        set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ENABLE_NUGET OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                hiredis
                GIT_REPOSITORY https://github.com/redis/hiredis.git
                GIT_TAG v1.3.0
                GIT_SHALLOW TRUE
                SYSTEM)
        FetchContent_MakeAvailable(hiredis)
    elseif (NOT hiredis_FOUND)
        message(FATAL_ERROR
                "hiredis >= 1.3 is required; install it or enable "
                "A11_FETCH_MISSING_DEPS")
    endif ()
    a11_require_static_target(hiredis::hiredis)
endif ()

if (A11_BUILD_SQLITE)
    # SQLite is always vendored from the upstream amalgamation, never found on
    # the system. A system SQLite is a shared library, which A11_REQUIRE_STATIC_DEPS
    # forbids, and the version would be unpinned. A11_SQLITE_SOURCE_DIR points an
    # offline build at an already-extracted amalgamation directory.
    if (A11_SQLITE_SOURCE_DIR)
        FetchContent_Declare(sqlite3 SOURCE_DIR "${A11_SQLITE_SOURCE_DIR}")
    elseif (A11_FETCH_MISSING_DEPS)
        FetchContent_Declare(
                sqlite3
                URL https://sqlite.org/2025/sqlite-amalgamation-3500400.zip
                URL_HASH
                SHA3_256=f131b68e6ba5fb891cc13ebb5ff9555054c77294cb92d8d1268bad5dba4fa2a1
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
                SYSTEM)
    else ()
        message(FATAL_ERROR
                "A11_BUILD_SQLITE requires the SQLite amalgamation; enable "
                "A11_FETCH_MISSING_DEPS or set A11_SQLITE_SOURCE_DIR")
    endif ()
    FetchContent_MakeAvailable(sqlite3)

    add_library(a11_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
    add_library(a11::sqlite3 ALIAS a11_sqlite3)
    set_target_properties(
            a11_sqlite3 PROPERTIES
            EXPORT_NAME sqlite3
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            POSITION_INDEPENDENT_CODE ON)
    # The amalgamation ships as one C file and cannot satisfy a11::warnings
    # (-Wconversion -Wshadow -Wpedantic, optionally -Werror), so it is compiled
    # warning-free like the other vendored C dependencies. SYSTEM on the include
    # directory keeps its headers out of our warning surface too.
    target_include_directories(
            a11_sqlite3 SYSTEM PUBLIC
            $<BUILD_INTERFACE:${sqlite3_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_options(a11_sqlite3 PRIVATE -w)
    # SQLITE_API carries the visibility attribute so the statically linked copy
    # inside _native.so can never be exported. CPython's own _sqlite3 module
    # loads the system libsqlite3 into the same process; on ELF, an exported
    # duplicate would interpose on that library's internal calls and corrupt its
    # sqlite3_stmt state. C_VISIBILITY_PRESET alone would suffice, but the two
    # together cost nothing and are checked by scripts/audit_wheel.py.
    target_compile_definitions(
            a11_sqlite3 PRIVATE
            "SQLITE_API=__attribute__((visibility(\"hidden\")))"
            SQLITE_THREADSAFE=1
            SQLITE_DQS=0
            SQLITE_DEFAULT_MEMSTATUS=0
            SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
            SQLITE_LIKE_DOESNT_MATCH_BLOBS
            SQLITE_OMIT_DEPRECATED
            SQLITE_OMIT_LOAD_EXTENSION
            SQLITE_OMIT_SHARED_CACHE
            SQLITE_OMIT_AUTOINIT
            SQLITE_USE_ALLOCA
            # Bounds the recursive-descent expression parser's C-stack use. The
            # store issues only fixed, shallow, hand-written statements.
            SQLITE_MAX_EXPR_DEPTH=50)
    if (UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(a11_sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif ()
endif ()

if (A11_BUILD_AUDIO)
    # PortAudio backs the cross-platform audio input SDK. Prefer a static
    # package from the bootstrapped prefix; otherwise build it from source with
    # only the static library (no shared lib, tests, or examples), like hiredis.
    find_package(portaudio CONFIG QUIET
            NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
    if (NOT portaudio_FOUND AND A11_FETCH_MISSING_DEPS)
        set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(PA_ENABLE_DEBUG_OUTPUT OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                portaudio
                GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
                GIT_TAG v19.7.0
                GIT_SHALLOW TRUE
                SYSTEM)
        # PortAudio 19.7.0 still declares cmake_minimum_required(VERSION 2.8),
        # which CMake 4 rejects. Grant the fetched subproject the 3.5 policy
        # floor without lowering it for the rest of the tree.
        set(A11_SAVED_POLICY_MIN "${CMAKE_POLICY_VERSION_MINIMUM}")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        FetchContent_MakeAvailable(portaudio)
        set(CMAKE_POLICY_VERSION_MINIMUM "${A11_SAVED_POLICY_MIN}")
        unset(A11_SAVED_POLICY_MIN)
    elseif (NOT portaudio_FOUND)
        message(FATAL_ERROR
                "PortAudio is required; install it or enable "
                "A11_FETCH_MISSING_DEPS")
    endif ()
    # PortAudio's target name varies across its packaging (imported package vs
    # in-tree static build). Normalize whichever exists to one variable.
    if (TARGET PortAudio::portaudio)
        set(A11_PORTAUDIO_TARGET PortAudio::portaudio)
        set(A11_PORTAUDIO_INSTALL_TARGET PortAudio::portaudio)
    elseif (TARGET portaudio_static)
        set(A11_PORTAUDIO_TARGET portaudio_static)
        set(A11_PORTAUDIO_INSTALL_TARGET portaudio_static)
    elseif (TARGET portaudio)
        set(A11_PORTAUDIO_TARGET portaudio)
        set(A11_PORTAUDIO_INSTALL_TARGET portaudio)
    else ()
        message(FATAL_ERROR
                "PortAudio was resolved but no known target was defined")
    endif ()
    a11_require_static_target(${A11_PORTAUDIO_TARGET})

    # whisper.cpp supplies local automatic speech recognition. It is built as
    # static whisper + GGML archives. Generic per-architecture CPU kernels keep
    # wheels portable; macOS additionally gets the system Metal, Accelerate,
    # and Apple BLAS backends without introducing loader dependencies.
    find_package(whisper CONFIG QUIET
            NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
    if (NOT whisper_FOUND AND A11_FETCH_MISSING_DEPS)
        set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
        set(WHISPER_COREML OFF CACHE BOOL "" FORCE)
        set(WHISPER_OPENVINO OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
        set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
        if (APPLE)
            set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
            set(GGML_BLAS ON CACHE BOOL "" FORCE)
            set(GGML_BLAS_VENDOR Apple CACHE STRING "" FORCE)
            set(GGML_METAL ON CACHE BOOL "" FORCE)
            set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
            set(GGML_METAL_NDEBUG ON CACHE BOOL "" FORCE)
        else ()
            set(GGML_BLAS OFF CACHE BOOL "" FORCE)
            set(GGML_METAL OFF CACHE BOOL "" FORCE)
        endif ()
        FetchContent_Declare(
                whisper
                GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
                GIT_TAG v1.9.2
                GIT_SHALLOW TRUE
                SYSTEM)
        FetchContent_MakeAvailable(whisper)
    elseif (NOT whisper_FOUND)
        message(FATAL_ERROR
                "whisper.cpp is required; install it or enable "
                "A11_FETCH_MISSING_DEPS")
    endif ()
    if (NOT TARGET whisper)
        message(FATAL_ERROR
                "whisper.cpp was resolved but its whisper target is missing")
    endif ()
    a11_require_static_target(whisper)
    foreach (A11_WHISPER_STATIC_TARGET IN ITEMS
            ggml ggml-base ggml-cpu ggml-blas ggml-metal
            ggml::ggml ggml::ggml-base ggml::ggml-cpu ggml::ggml-blas
            ggml::ggml-metal)
        if (TARGET ${A11_WHISPER_STATIC_TARGET})
            a11_require_static_target(${A11_WHISPER_STATIC_TARGET})
        endif ()
    endforeach ()
    unset(A11_WHISPER_STATIC_TARGET)
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
        a11_require_from_prefix("OpenSSL crypto archive"
                "${A11_OPENSSL_CRYPTO_STATIC_LIBRARY}")
        a11_require_from_prefix("OpenSSL SSL archive"
                "${A11_OPENSSL_SSL_STATIC_LIBRARY}")
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
    find_package(LibDataChannel CONFIG QUIET
            NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH})
endif ()
if (A11_BUILD_WEBRTC AND NOT LibDataChannel_FOUND AND A11_FETCH_MISSING_DEPS)
    set(NO_MEDIA ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(USE_SYSTEM_JSON ON CACHE BOOL "" FORCE)
    # The patch exposes the SCTP path MTU as a settable property of a live
    # association, which usrsctp supports and libdatachannel does not surface.
    # A11's path MTU discovery (RFC 8899) cannot work without it: see
    # scripts/patches/libdatachannel-live-pmtu.patch for the rationale, and the
    # `wire/stream_throughput[webrtc-live-mtu]` bench row for the measurement
    # that catches a build where the patch silently did not apply.
    #
    # `git apply`, and `--reverse --check ||` first so that re-configuring a tree
    # whose source directory already carries the patch is a no-op rather than an
    # error: FetchContent re-runs PATCH_COMMAND against an existing checkout.
    # Two patches, and both are load-bearing for path MTU discovery:
    #
    #   libdatachannel-live-pmtu    exposes the SCTP path MTU as something that
    #                               can be set on a live association at all
    #   usrsctp-raise-association-mtu  lets that value go *up*, which usrsctp
    #                               explicitly refused -- without it the search
    #                               can only ever reduce, so it cannot
    #                               binary-search and cannot recover a path that
    #                               grew (see each patch's header for the
    #                               measurements)
    #   libdatachannel-dtls-buffer  raises the DTLS receive buffer from 4 KiB to
    #                               16 KiB. That buffer is handed to the TLS
    #                               implementation as its MTU, so it is a ceiling
    #                               on the path MTU and not merely a buffer size
    #   libjuice-recv-buffer        raises libjuice's UDP receive buffers to the
    #                               same 16 KiB. This is the one that actually
    #                               moved the cliff: recvfrom() *discards* what
    #                               does not fit, so with the DTLS patch alone
    #                               4132 still worked and 4256 still carried
    #                               nothing. Both are needed; neither suffices
    #
    # The second applies inside libdatachannel's usrsctp submodule, hence the
    # `git -C deps/usrsctp`. Each is guarded by `--reverse --check ||` so that
    # re-configuring a tree whose source directory already carries the patch is a
    # no-op rather than an error: FetchContent re-runs PATCH_COMMAND against an
    # existing checkout.
    #
    # A build where either silently failed to apply looks like one where
    # discovery runs and achieves nothing, so
    # `wire/stream_throughput[webrtc-live-mtu,64K]` exists to catch it: it must
    # match the `webrtc` row for the same MTU, and does not without both patches.
    set(A11_LIBDATACHANNEL_PATCH
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patches/libdatachannel-live-pmtu.patch")
    set(A11_USRSCTP_PATCH
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patches/usrsctp-raise-association-mtu.patch")
    set(A11_LIBDATACHANNEL_DTLS_PATCH
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patches/libdatachannel-dtls-buffer.patch")
    set(A11_LIBJUICE_PATCH
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patches/libjuice-recv-buffer.patch")
    # Git rather than an archive, because the submodules and the `git apply`
    # patches below both need a repository -- but *not* shallow: this pin is a
    # SHA, and a shallow fetch can only ask for advertised refs, which is what
    # made the equivalent pybind11_abseil step flake in CI. A full clone of a
    # SHA always works.
    FetchContent_Declare(
            libdatachannel
            GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
            GIT_TAG 443f6934d9007eb7076ab7825ba330f355fcbead
            GIT_SUBMODULES_RECURSE TRUE
            PATCH_COMMAND
                    sh -c "git apply --reverse --check '${A11_LIBDATACHANNEL_PATCH}' || git apply '${A11_LIBDATACHANNEL_PATCH}'"
                    && sh -c "git apply --reverse --check '${A11_LIBDATACHANNEL_DTLS_PATCH}' || git apply '${A11_LIBDATACHANNEL_DTLS_PATCH}'"
                    && sh -c "git -C deps/libjuice apply --reverse --check '${A11_LIBJUICE_PATCH}' || git -C deps/libjuice apply '${A11_LIBJUICE_PATCH}'"
                    && sh -c "git -C deps/usrsctp apply --reverse --check '${A11_USRSCTP_PATCH}' || git -C deps/usrsctp apply '${A11_USRSCTP_PATCH}'")
    FetchContent_MakeAvailable(libdatachannel)
elseif (A11_BUILD_WEBRTC AND NOT LibDataChannel_FOUND)
    message(FATAL_ERROR
            "libdatachannel is required; install it or enable A11_FETCH_MISSING_DEPS")
endif ()
if (A11_BUILD_WEBRTC)
    a11_require_static_target(LibDataChannel::LibDataChannel)
    # Its headers are somebody else's code, so its warnings are not ours to fix:
    # rtc/*.hpp trips -Wextra-semi, and the translation unit that includes them
    # is A11's, so without this the diagnostics arrive attributed to A11.
    get_target_property(A11_DATACHANNEL_ALIASED
                        LibDataChannel::LibDataChannel ALIASED_TARGET)
    if (A11_DATACHANNEL_ALIASED)
        set_target_properties(${A11_DATACHANNEL_ALIASED} PROPERTIES SYSTEM TRUE)
    endif ()
endif ()
if (A11_BUILD_WEBRTC AND NOT A11_BUILD_HTTP)
    message(FATAL_ERROR
            "A11_BUILD_WEBRTC requires A11_BUILD_HTTP for nghttp2 signalling")
endif ()
if (A11_BUILD_AUDIO AND NOT A11_BUILD_HTTP)
    message(FATAL_ERROR
            "A11_BUILD_AUDIO requires A11_BUILD_HTTP to download whisper and "
            "Silero models by shorthand")
endif ()
if (A11_BUILD_HTTP)
    pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)
    find_library(
            A11_NGHTTP2_STATIC_LIBRARY
            NAMES libnghttp2.a nghttp2_static
            HINTS ${NGHTTP2_STATIC_LIBRARY_DIRS} ${NGHTTP2_LIBRARY_DIRS}
            NO_DEFAULT_PATH)
    if (A11_NGHTTP2_STATIC_LIBRARY)
        a11_require_from_prefix("nghttp2 archive" "${A11_NGHTTP2_STATIC_LIBRARY}")
        add_library(a11_nghttp2 STATIC IMPORTED GLOBAL)
        set_target_properties(
                a11_nghttp2 PROPERTIES
                IMPORTED_LOCATION "${A11_NGHTTP2_STATIC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIRS}"
                INTERFACE_LINK_OPTIONS "${NGHTTP2_STATIC_LDFLAGS_OTHER}")
        add_library(a11::nghttp2 ALIAS a11_nghttp2)
    else ()
        message(FATAL_ERROR
                "A static libnghttp2 archive is required but was not found in: "
                "${NGHTTP2_LIBRARY_DIRS}")
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
add_subdirectory(a11/flow)
add_subdirectory(a11/net)
add_subdirectory(a11/obs)
add_subdirectory(a11/nodes)
add_subdirectory(a11/service)
add_subdirectory(a11/stores)
if (A11_BUILD_REDIS)
    add_subdirectory(redis)
endif ()
if (A11_BUILD_AUDIO)
    add_subdirectory(sdk/audio)
endif ()
if (A11_BUILD_HTTP)
    add_subdirectory(sdk/http)
endif ()
if (A11_BUILD_FLOW_ACTIONS)
    add_subdirectory(sdk/flow)
endif ()
if (A11_BUILD_BENCH)
    add_subdirectory(bench)
endif ()
add_subdirectory(python)

# The bundled thread library is the scheduling substrate for every A11 task.
add_subdirectory(thread)

add_library(a11_warnings INTERFACE)
add_library(a11::warnings ALIAS a11_warnings)
set_target_properties(a11_warnings PROPERTIES EXPORT_NAME warnings)
# One list per compiler rather than one long generator expression: a warning is
# added here often enough that the line has to stay readable.
#
# Two of these are deliberately absent. -Wpadded reports ~440 sites, nearly all
# of them a `bool` next to an eight-aligned member, and reordering 170-odd
# structs by field size to silence it would trade the order a reader wants for
# tail padding nobody measured. -Wunused-member-function fires on the complete
# rule-of-five of small internal helpers -- deleting an unused move-assignment
# to satisfy it makes the type worse, not the build cleaner.
set(A11_CLANG_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wmissing-field-initializers
        -Wthread-safety
        -Werror=thread-safety
        # `_Nullable` and friends are a clang extension, which is not news; a
        # pointer in an annotated file that has no annotation is.
        -Wno-nullability-extension
        -Wnullability-completeness
        -Wimplicit-fallthrough
        -Wextra-semi
        -Wcast-qual
        -Wcomma
        -Wloop-analysis
        -Wredundant-move
        -Wunreachable-code-aggressive)
set(A11_GNU_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        # `=local` rather than plain -Wshadow, which on GCC also reports a
        # constructor parameter that shares a name with the member it
        # initialises -- `S(int value) : value(value) {}`, the canonical way to
        # write one. The subset kept is what clang's -Wshadow reports: a local
        # or parameter hidden by another one.
        -Wshadow=local
        -Wimplicit-fallthrough
        -Wextra-semi
        -Wcast-qual
        -Wredundant-move)
target_compile_options(
        a11_warnings
        INTERFACE
        $<$<CXX_COMPILER_ID:AppleClang,Clang>:${A11_CLANG_WARNINGS}>
        $<$<CXX_COMPILER_ID:GNU>:${A11_GNU_WARNINGS}>)
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
        PRIVATE $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log> a11::warnings nlohmann_json::nlohmann_json
        $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random>
        $<BUILD_INTERFACE:absl::str_format> $<INSTALL_INTERFACE:absl::str_format>)

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
# The Flow language: text to tokens to a syntax tree to a resolved plan, plus
# the diagnostics, highlighting, formatting and completion every editor and CI
# job needs. Nothing here touches a socket or a node, and nothing here links
# anything but Abseil and nlohmann -- see cpp/a11/flow/CMakeLists.txt for why
# that boundary is worth keeping.
add_library(a11_flow_lang STATIC ${A11_FLOW_LANG_SOURCES})
add_library(a11::flow_lang ALIAS a11_flow_lang)
set_target_properties(a11_flow_lang PROPERTIES EXPORT_NAME flow_lang)
target_include_directories(
        a11_flow_lang PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_flow_lang
        PUBLIC $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
        $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        $<BUILD_INTERFACE:absl::span> $<INSTALL_INTERFACE:absl::span>
        $<BUILD_INTERFACE:absl::flat_hash_map> $<INSTALL_INTERFACE:absl::flat_hash_map>
        $<BUILD_INTERFACE:absl::flat_hash_set> $<INSTALL_INTERFACE:absl::flat_hash_set>
        # A duration token carries an absl::Duration, in a public header.
        $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
        nlohmann_json::nlohmann_json
        PRIVATE $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
        $<BUILD_INTERFACE:absl::str_format> $<INSTALL_INTERFACE:absl::str_format>
        a11::warnings)

# The standalone language tool. Off where only the library is wanted (a wheel
# build), on by default: an editor that cannot embed Python runs this, and the
# IntelliJ plugin bundles it per platform instead of carrying language knowledge.
if (A11_BUILD_FLOW_TOOL)
    add_executable(a11_flow_tool ${A11_FLOW_TOOL_SOURCES})
    add_executable(a11::flow_tool ALIAS a11_flow_tool)
    set_target_properties(
            a11_flow_tool PROPERTIES OUTPUT_NAME a11-flow EXPORT_NAME flow_tool)
    target_link_libraries(
            a11_flow_tool
            PRIVATE a11::flow_lang nlohmann_json::nlohmann_json
            absl::strings absl::str_format absl::flat_hash_map a11::warnings
            a11::allocator)
endif ()

add_library(a11_obs STATIC ${A11_OBS_SOURCES})
add_library(a11::obs ALIAS a11_obs)
set_target_properties(a11_obs PROPERTIES EXPORT_NAME obs)
target_compile_definitions(a11_obs PRIVATE A11_VERSION="${PROJECT_VERSION}")
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

if (A11_BUILD_REDIS)
    add_library(a11_redis STATIC ${A11_REDIS_SOURCES})
    add_library(a11::redis ALIAS a11_redis)
    set_target_properties(a11_redis PROPERTIES EXPORT_NAME redis)
    target_include_directories(
            a11_redis PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(
            a11_redis
            PUBLIC a11::concurrency
            $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
            $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
            $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
            PRIVATE hiredis::hiredis uvw::uv_a uvw::uvw
            $<BUILD_INTERFACE:absl::flat_hash_map>
            $<INSTALL_INTERFACE:absl::flat_hash_map>
            $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log>
            $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
            a11::warnings)
endif ()

if (A11_BUILD_AUDIO)
    add_library(a11_audio STATIC ${A11_AUDIO_SOURCES})
    add_library(a11::audio ALIAS a11_audio)
    set_target_properties(a11_audio PROPERTIES EXPORT_NAME audio)
    target_include_directories(
            a11_audio PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(
            a11_audio
            PUBLIC a11::concurrency
            $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
            $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
            $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
            PRIVATE
            $<BUILD_LOCAL_INTERFACE:${A11_PORTAUDIO_TARGET}>
            $<INSTALL_INTERFACE:${A11_PORTAUDIO_INSTALL_TARGET}>
            $<BUILD_LOCAL_INTERFACE:whisper>
            $<INSTALL_INTERFACE:whisper::whisper>
            $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log>
            $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
            a11::warnings)
    # PortAudio's target carries its host-API link requirements. Keep the macOS
    # system frameworks explicit for older package exports; on Linux, do not
    # rediscover ALSA independently because the bootstrapped PortAudio target
    # deliberately records the pinned static libasound archive.
    if (APPLE)
        target_link_libraries(
                a11_audio PRIVATE
                "-framework CoreAudio" "-framework AudioToolbox"
                "-framework AudioUnit" "-framework CoreFoundation"
                "-framework CoreServices")
    elseif (UNIX)
        find_package(Threads REQUIRED)
        # Static ALSA uses dlsym for its plugin loader. On pre-glibc-2.34
        # manylinux hosts that symbol still lives in a separate libdl.
        target_link_libraries(
                a11_audio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif ()
    # whisper.cpp installs its unrelated parakeet library unconditionally.
    # Building the focused A11 audio target must therefore also produce that
    # archive so a subsequent `cmake --install` cannot reference a missing
    # build artifact.
    if (TARGET parakeet)
        add_dependencies(a11_audio parakeet)
    endif ()
endif ()

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
if (A11_BUILD_REDIS)
    target_link_libraries(a11_stores PUBLIC a11::redis)
endif ()
if (A11_BUILD_SQLITE)
    # PRIVATE: sqlite3.h never appears in a public A11 header, so consumers of
    # a11::stores do not inherit it.
    target_link_libraries(a11_stores PRIVATE a11::sqlite3)
endif ()

# A11's one libuv loop. Header-only, and its own target because an event loop is
# not an HTTP concept: a11_net was simply where it grew up, and the Flow standard
# library needs the same loop for watching a directory and accepting a
# connection. Two loops would mean two threads and two answers to "which thread
# owns this handle". Unconditional, so anything that wants a timer or a
# descriptor can simply have one.
add_library(a11_uv INTERFACE)
add_library(a11::uv ALIAS a11_uv)
set_target_properties(a11_uv PROPERTIES EXPORT_NAME uv)
target_include_directories(
        a11_uv INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_uv INTERFACE a11::concurrency uvw::uvw
        $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log>
        $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
        $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
        $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>)

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
    target_link_libraries(a11_net PRIVATE a11::nghttp2 uvw::uvw a11::uv $<BUILD_INTERFACE:absl::log> $<INSTALL_INTERFACE:absl::log>
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
        # nlohmann for a11/actions/describe.cc, which builds the a11.actions/v1
        # document. Private, and the public header includes only json_fwd.hpp,
        # so describing an action costs a consumer no nlohmann dependency.
        PRIVATE $<BUILD_INTERFACE:a11::obs> $<BUILD_INTERFACE:absl::random_random> $<INSTALL_INTERFACE:absl::random_random> $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings> a11::warnings nlohmann_json::nlohmann_json)

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

# The executing half of Flow: the value model and the runtime driving nodes and
# actions with it. Defined here rather than beside a11_flow_lang because this is
# the first point at which the layers it stands on exist.
add_library(a11_flow_runtime STATIC ${A11_FLOW_RUNTIME_SOURCES})
add_library(a11::flow_runtime ALIAS a11_flow_runtime)
set_target_properties(a11_flow_runtime PROPERTIES EXPORT_NAME flow_runtime)
target_include_directories(
        a11_flow_runtime PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
        a11_flow_runtime
        PUBLIC a11::actions a11::core a11::data a11::flow_lang a11::nodes
        $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
        $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
        $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
        nlohmann_json::nlohmann_json
        PRIVATE $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
        $<BUILD_INTERFACE:absl::str_format> $<INSTALL_INTERFACE:absl::str_format>
        a11::warnings)

if (A11_BUILD_AUDIO)
    # The Action-based audio interface: schemas, streaming handlers and the
    # serialization of the audio value types. Sits above both the raw audio SDK
    # and the actions/nodes layer.
    add_library(a11_audio_actions STATIC ${A11_AUDIO_ACTIONS_SOURCES})
    add_library(a11::audio_actions ALIAS a11_audio_actions)
    set_target_properties(a11_audio_actions PROPERTIES EXPORT_NAME audio_actions)
    target_include_directories(
            a11_audio_actions PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(
            a11_audio_actions
            # a11::net for the model downloader: a shorthand like "base.en" is
            # fetched into the shared cache the first time it is asked for.
            PUBLIC a11::actions a11::audio a11::concurrency a11::data a11::net
            a11::nodes
            $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
            $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
            PRIVATE
            $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
            $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
            nlohmann_json::nlohmann_json a11::warnings)
endif ()

if (A11_BUILD_HTTP)
    # HTTP as Actions: make_http_request and web-fetch. Above a11::net because it
    # is a client of it, and above a11::actions because that is what it produces;
    # its own target so neither of those has to know about the other.
    add_library(a11_http_actions STATIC ${A11_HTTP_ACTIONS_SOURCES})
    add_library(a11::http_actions ALIAS a11_http_actions)
    set_target_properties(a11_http_actions PROPERTIES EXPORT_NAME http_actions)
    target_include_directories(
            a11_http_actions PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(
            a11_http_actions
            PUBLIC a11::actions a11::concurrency a11::core a11::data a11::net
            a11::nodes
            $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
            $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
            PRIVATE
            $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
            $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
            nlohmann_json::nlohmann_json a11::warnings)
endif ()

if (A11_BUILD_FLOW_ACTIONS)
    # The Flow standard library. Above a11::actions because that is what it
    # produces, and above nothing else: a host that wants `read_file` should not
    # link the flow engine to get it, which is also why the duration parser in
    # options.cc is its own rather than the runtime's.
    add_library(a11_flow_actions STATIC ${A11_FLOW_ACTIONS_SOURCES})
    add_library(a11::flow_actions ALIAS a11_flow_actions)
    set_target_properties(a11_flow_actions PROPERTIES EXPORT_NAME flow_actions)
    target_include_directories(
            a11_flow_actions PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(
            a11_flow_actions
            PUBLIC a11::actions a11::concurrency a11::core a11::data a11::nodes
            $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
            $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
            PRIVATE
            $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
            $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
            nlohmann_json::nlohmann_json a11::warnings)
    # The loop. Unconditional: a deadline, a directory watch and a listening
    # socket all want it, and an action that has it only in some builds is an
    # action with two implementations, one of which nothing ever runs.
    target_link_libraries(a11_flow_actions PRIVATE a11::uv)
endif ()

if (A11_BUILD_FLOW_ACTIONS)
    # The interpreter: a11-flow-run, which runs a .flow file as a program.
    #
    # A separate binary from a11-flow, and deliberately so: that one is the
    # language's own tool and links nothing but a11::flow_lang, which is the
    # proof the language needs no runtime to be checked, formatted or described.
    # Running a file needs the runtime, the actions layer and the standard
    # library, so it is its own executable rather than a heavier a11-flow.
    # The interpreter itself, as a library: the CLI below and the Python binding
    # both call it, so a program behaves identically whichever started it.
    add_library(a11_flow_interpreter_lib STATIC
            ${CMAKE_CURRENT_SOURCE_DIR}/a11/flow/interpreter/interpreter.cc)
    add_library(a11::flow_interpreter_lib ALIAS a11_flow_interpreter_lib)
    set_target_properties(
            a11_flow_interpreter_lib PROPERTIES EXPORT_NAME flow_interpreter_lib)
    target_include_directories(
            a11_flow_interpreter_lib PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(
            a11_flow_interpreter_lib
            # a11::service for the session a program is given when its `call`
            # steps are to reach a peer.
            PUBLIC a11::flow_runtime a11::flow_lang a11::flow_actions
            a11::actions a11::concurrency a11::data a11::nodes a11::service
            $<BUILD_INTERFACE:absl::status> $<INSTALL_INTERFACE:absl::status>
            $<BUILD_INTERFACE:absl::statusor> $<INSTALL_INTERFACE:absl::statusor>
            PRIVATE
            $<BUILD_INTERFACE:absl::strings> $<INSTALL_INTERFACE:absl::strings>
            $<BUILD_INTERFACE:absl::time> $<INSTALL_INTERFACE:absl::time>
            a11::warnings)
    a11_disallow_exceptions(a11_flow_interpreter_lib)

    add_executable(a11_flow_interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/a11/flow/interpreter/main.cc)
    add_executable(a11::flow_interpreter ALIAS a11_flow_interpreter)
    set_target_properties(
            a11_flow_interpreter PROPERTIES
            OUTPUT_NAME a11-flow-run EXPORT_NAME flow_interpreter)
    target_link_libraries(
            a11_flow_interpreter
            PRIVATE a11::flow_interpreter_lib a11::flow_runtime a11::flow_lang
            a11::flow_actions a11::actions a11::concurrency a11::data
            a11::nodes a11::stores absl::status absl::statusor absl::strings
            absl::time nlohmann_json::nlohmann_json a11::warnings
            a11::allocator)
    a11_disallow_exceptions(a11_flow_interpreter)
endif ()

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
            # A source archive rather than a git checkout, and pinned by content
            # hash. pybind11_abseil publishes one tag (v202402.0) and the commit
            # A11 needs is not it, so the pin is a SHA -- and `GIT_SHALLOW TRUE`
            # with a SHA is the combination that made this step flaky: a shallow
            # fetch can only ask for advertised refs, so the clone depended on
            # the server being willing to serve an unadvertised object and fell
            # over intermittently in CI. Codeload has neither problem, and
            # URL_HASH turns a changed archive into a loud failure instead of a
            # silently different dependency.
            FetchContent_Declare(
                    pybind11_abseil
                    URL "https://github.com/pybind/pybind11_abseil/archive/dba6e38b79d93bc718e28b8f8ea2e8b00b60cdec.tar.gz"
                    URL_HASH
                    SHA256=c54a260dcb34ad2d83cdb273855f79ac4f98a34130aae1ddbf39da2d771f4090
                    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
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
    target_compile_definitions(a11_python PRIVATE
            A11_VERSION="${PROJECT_VERSION}")
    if (A11_BUILD_REDIS)
        target_compile_definitions(a11_python PRIVATE A11_BUILD_REDIS=1)
    endif ()
    if (A11_BUILD_SQLITE)
        target_compile_definitions(a11_python PRIVATE A11_BUILD_SQLITE=1)
    endif ()
    if (A11_BUILD_AUDIO)
        target_compile_definitions(a11_python PRIVATE A11_BUILD_AUDIO=1)
    endif ()
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
            PRIVATE a11::actions a11::concurrency a11::core a11::data
            a11::flow_lang a11::flow_runtime a11::net
            a11::nodes a11::obs a11::service a11::stores
            pybind11_abseil::status_casters a11::warnings
            absl::failure_signal_handler absl::symbolize)
    if (A11_BUILD_AUDIO)
        target_link_libraries(a11_python PRIVATE a11::audio a11::audio_actions)
    endif ()
    if (A11_BUILD_HTTP)
        target_link_libraries(a11_python PRIVATE a11::http_actions)
    endif ()
    if (A11_BUILD_FLOW_ACTIONS)
        # The interpreter, so `a11.flow.run_program` is the same code the CLI is.
        target_link_libraries(a11_python PRIVATE a11::flow_interpreter_lib)
    endif ()

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

    # Ship the shared mimalloc beside the extension.
    #
    # The extension itself is deliberately *not* built against it (see the
    # a11_allocator block): replacing malloc from inside a module that an
    # interpreter dlopens after it has already allocated is not a safe thing to
    # do to somebody else's process. Replacing it from the start, before the
    # interpreter allocates anything, is entirely safe -- and that is a preload,
    # which is a property of how the process is launched and cannot be decided
    # from inside it.
    #
    # So the wheel carries the library and A11 activates it where it launches its
    # own processes; see a11/allocator.py, which finds it here. Nothing imports
    # or loads it as a side effect of `import a11`.
    if (A11_ALLOCATOR_SHARED_LIBRARY)
        install(FILES "${A11_ALLOCATOR_SHARED_LIBRARY}"
                DESTINATION a11/_allocator COMPONENT python)
        if (NOT SKBUILD)
            # Direct CMake builds keep the fast edit/test loop working by putting
            # it where the installed layout puts it. A custom command rather than
            # file(COPY), because for a fetched mimalloc the path is a generator
            # expression that has no value at configure time.
            add_custom_command(TARGET a11_python POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/../a11/_allocator"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${A11_ALLOCATOR_SHARED_LIBRARY}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../a11/_allocator"
                    COMMENT "Staging the preloadable allocator in a11/_allocator")
        endif ()
    endif ()
    install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/../a11/py.typed"
            DESTINATION a11 COMPONENT python)
    # The stubs are a package, one file per module of the extension. See
    # scripts/generate_stubs.py.
    install(DIRECTORY
            "${CMAKE_CURRENT_SOURCE_DIR}/../a11/_native/"
            DESTINATION a11/_native COMPONENT python
            FILES_MATCHING PATTERN "*.pyi")
    if (NOT SKBUILD)
        add_custom_target(
                a11_python_stubs
                COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_stubs.py"
                DEPENDS a11_python
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
                COMMENT "Generating a11/_native/*.pyi")
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

# --- Exception policy -------------------------------------------------------
#
# One place that says which of A11 may throw, and it is not configurable: see
# a11_disallow_exceptions and a11_allow_exceptions near the top of this file for
# how it is applied, why the opt-out is per file, and why there is no flag. The
# rule for adding to the opted-out list: the translation unit must *call* code
# that can throw. Catching an exception that A11 itself would have to raise is not
# a reason, because A11 does not raise any.
a11_disallow_exceptions(
        a11_core a11_data a11_concurrency a11_obs a11_stores a11_net a11_nodes
        a11_actions a11_service a11_flow_lang a11_flow_runtime)
if (A11_BUILD_REDIS)
    a11_disallow_exceptions(a11_redis)
endif ()
if (A11_BUILD_AUDIO)
    a11_disallow_exceptions(a11_audio a11_audio_actions)
endif ()
if (A11_BUILD_HTTP)
    a11_disallow_exceptions(a11_http_actions)
endif ()
if (A11_BUILD_FLOW_ACTIONS)
    # Nothing here throws, which is worth stating for a library made of
    # std::filesystem and nlohmann calls: both have throwing overloads that are
    # the default, and -fno-exceptions is what stops one being reached for.
    a11_disallow_exceptions(a11_flow_actions)
endif ()

# The boundaries, each with the throwing code it calls.
a11_allow_exceptions(
        # Parses text and dumps bytes through nlohmann. Two of its five entry
        # points have no non-throwing overload to use instead.
        a11/json_codec.cc
        # The definition of exception_guard::Wrap, which is the `try` every
        # boundary below is built out of. Listed even though a header compiles as
        # part of whatever includes it: this is the file that must never be
        # included anywhere else, and naming it here is what says so in the same
        # place as the rest of the policy. If it is ever added to a target's
        # sources -- for an IDE, or a unity build -- the property is already right.
        #
        # It now enforces that itself with an #error, because the property here
        # cannot: a11/exception_guard.cc included it while being compiled
        # -fno-exceptions, and whether the `try` blocks in an uninstantiated
        # template are a diagnostic turned out to be compiler-dependent (Apple
        # clang 21 accepts them, the CI runner's older one and GCC do not). The
        # trait that file actually wanted lives in exception_guard_failure.h,
        # which needs no opt-out and is deliberately absent from this list.
        a11/internal/exception_guard_impl.h
        # Wrap a callable a caller handed us, so that whatever it throws becomes a
        # Status in this file's frame rather than unwinding through one of A11's.
        a11/concurrency/boundary.cc a11/net/boundary.cc
        a11/nodes/boundary.cc
        a11/service/boundary.cc a11/actions/boundary.cc
        a11/flow/boundary.cc)
if (A11_BUILD_AUDIO)
    # whisper.cpp is C++ behind a C API that promises no noexcept, and this file
    # is where A11 calls into it.
    a11_allow_exceptions(sdk/audio/speech_recognizer.cc)
endif ()
if (A11_BUILD_WEBRTC)
    # libdatachannel's rtc:: API throws. The path MTU search
    # (a11/net/internal/path_mtu.cc) touches no rtc:: type and stays
    # -fno-exceptions; only the stream that calls into libdatachannel needs the
    # opt-out.
    a11_allow_exceptions(
            a11/net/webrtc_wire_stream.cc a11/net/internal/rtc_binary_channel.cc)
endif ()
if (A11_BUILD_FLOW_TOOL)
    # A development tool, not part of the shipped library: dense LSP JSON where
    # nlohmann's own throw-on-mistyped-field is the error handling.
    a11_allow_exceptions(a11/flow/tool/main.cc a11/flow/tool/lsp.cc)
endif ()

set(A11_INSTALL_TARGETS
        a11_warnings a11_core a11_data a11_concurrency a11_flow_lang a11_stores
        a11_net a11_nodes a11_actions a11_service Thread)
if (A11_BUILD_REDIS)
    list(APPEND A11_INSTALL_TARGETS a11_redis)
endif ()
if (A11_BUILD_SQLITE)
    # a11_stores links this, so the exported a11Targets set must contain it or
    # the installed config references a missing target.
    list(APPEND A11_INSTALL_TARGETS a11_sqlite3)
endif ()
if (A11_BUILD_AUDIO)
    list(APPEND A11_INSTALL_TARGETS a11_audio a11_audio_actions)
endif ()
if (A11_BUILD_HTTP)
    list(APPEND A11_INSTALL_TARGETS a11_http_actions)
endif ()
if (A11_BUILD_FLOW_ACTIONS)
    # Not the interpreter library: it links a11_flow_runtime, which is not in
    # the export set, and nothing outside this build consumes it -- the CLI and
    # the Python module are both built here.
    list(APPEND A11_INSTALL_TARGETS a11_flow_actions)
endif ()
list(APPEND A11_INSTALL_TARGETS a11_uv)
if (A11_BUILD_FLOW_TOOL)
    list(APPEND A11_INSTALL_TARGETS a11_flow_tool)
endif ()
install(
        TARGETS ${A11_INSTALL_TARGETS}
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
if (A11_BUILD_REDIS)
    install(
            DIRECTORY redis/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/redis
            FILES_MATCHING PATTERN "*.h")
endif ()
if (A11_BUILD_AUDIO)
    install(
            DIRECTORY sdk/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sdk
            FILES_MATCHING PATTERN "*.h")
    # whisper.cpp's package config validates its binary directory even when
    # WHISPER_BUILD_EXAMPLES is disabled and no executable is installed.
    install(DIRECTORY DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()
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

# The public headers must compile for a consumer whose build has exceptions
# disabled, which is also how A11 builds itself. See tests/header_canary.cc.
add_library(a11_header_canary STATIC tests/header_canary.cc)
target_link_libraries(
        a11_header_canary PRIVATE
        a11::actions a11::concurrency a11::core a11::data a11::net a11::nodes
        a11::obs a11::service a11::stores a11::warnings)
a11_disallow_exceptions(a11_header_canary)
