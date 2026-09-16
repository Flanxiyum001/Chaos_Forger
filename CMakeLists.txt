cmake_minimum_required(VERSION 3.10)
project(Chaos_Forger VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Toolchain baseline
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release so a bare `cmake -S . -B build` yields an optimized daemon.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Options (no unnecessary dependencies; everything is stdlib + POSIX)
# ---------------------------------------------------------------------------
option(Chaos_Forger_BUILD_TESTS  "Build the unit-test suites (run with ctest)" ON)
option(Chaos_Forger_ENABLE_ASAN  "Instrument with AddressSanitizer (development only)" OFF)
option(Chaos_Forger_ENABLE_UBSAN "Instrument with UndefinedBehaviorSanitizer (development only)" OFF)

if(Chaos_Forger_ENABLE_ASAN OR Chaos_Forger_ENABLE_UBSAN)
  # Sanitizers are a development tool: they make no sense in a stripped
  # optimized release and are unsafe in the shipped daemon. Force Debug
  # unless the operator explicitly asked for something else, and never
  # combine them with a MinSizeRel-style build.
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Chaos_Forger: sanitizers requested - forcing CMAKE_BUILD_TYPE=Debug (was '${CMAKE_BUILD_TYPE}')")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()
endif()

# ---------------------------------------------------------------------------
# Warning policy (GCC/Clang; sane subset for MSVC-less Linux project)
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Sanitizer flags apply to every target defined below (library, daemon, tests).
if(Chaos_Forger_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(Chaos_Forger_ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Chaos_Forger static library: everything except the daemon entry point
# ---------------------------------------------------------------------------
add_library(Chaos_Forger STATIC
  src/log.cpp
  src/json.cpp
  src/config.cpp
  src/http_socket.cpp
  src/docker_api.cpp
  src/discovery.cpp
  src/chaos.cpp
  src/shutdown.cpp
  src/engine.cpp
)
target_include_directories(Chaos_Forger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Chaos_Forger PUBLIC Threads::Threads)
target_compile_features(Chaos_Forger PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# Chaos_Forger daemon executable
# ---------------------------------------------------------------------------
add_executable(Chaos_Forger main.cpp)
target_link_libraries(Chaos_Forger PRIVATE Chaos_Forger)

# ---------------------------------------------------------------------------
# Tests (optional; dependency-free assert harnesses, run via CTest)
# ---------------------------------------------------------------------------
if(Chaos_Forger_BUILD_TESTS)
  enable_testing()

  set(Chaos_Forger_TEST_SOURCES
    test_http
    test_config
    test_discovery
    test_chaos
    test_shutdown
    test_engine
  )

  foreach(name IN LISTS Chaos_Forger_TEST_SOURCES)
    add_executable(Chaos_Forger_${name}_tests tests/${name}.cpp)
    target_link_libraries(Chaos_Forger_${name}_tests PRIVATE Chaos_Forger)
    add_test(NAME Chaos_Forger_${name}_tests COMMAND Chaos_Forger_${name}_tests)
    # Disk-based fixtures (config.json, tests/data/) resolve from the repo root.
    set_tests_properties(Chaos_Forger_${name}_tests PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endforeach()

  # Real-Docker end-to-end test: SKIPPED (exit 77) unless Chaos_Forger_IT_LIVE=1,
  # so a plain `ctest` never creates or stops containers. See
  # scripts/integration_test.sh and the README integration-testing section.
  add_test(NAME Chaos_Forger_integration_tests
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/integration_test.sh
                   --Chaos_Forger-bin $<TARGET_FILE:Chaos_Forger>)
  set_tests_properties(Chaos_Forger_integration_tests PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "Chaos_Forger_IT_LIVE=${Chaos_Forger_IT_LIVE};Chaos_Forger_IT_KILL=${Chaos_Forger_IT_KILL}")
endif()

# ---------------------------------------------------------------------------
# Install: the daemon binary plus the reference configuration.
# The static library and headers are internal build artifacts of a daemon
# project, not an installed API - only ship what operators run.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS Chaos_Forger RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config.json DESTINATION ${CMAKE_INSTALL_SYSCONFDIR} RENAME Chaos_Forger.json)
