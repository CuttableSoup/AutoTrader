cmake_minimum_required(VERSION 3.25)
project(autotrader VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Dependency provider: "vcpkg" (manifest in this directory, MSVC/Linux) or
# "fetchcontent" (GitHub sources + system OpenSSL/libcurl; GCC/cygwin dev boxes).
# Auto: vcpkg if a vcpkg toolchain is in use, otherwise fetchcontent.
set(AT_DEPS "auto" CACHE STRING "vcpkg | fetchcontent | auto")
option(AT_WITH_NATS "Build the NATS JetStream bus (cnats)" ON)
option(AT_WITH_NET  "Build the Alpaca HTTP/WebSocket transport (libcurl + ixwebsocket)" ON)
option(AT_BUILD_TESTS "Build Catch2 tests" ON)

include(cmake/Deps.cmake)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

add_subdirectory(src/common)
add_subdirectory(src/strategy)
add_subdirectory(src/risk)
add_subdirectory(src/backtester)
if(AT_WITH_NET)
  add_subdirectory(src/alpaca)
  add_subdirectory(src/execution)
  add_subdirectory(src/portfolio)
  add_subdirectory(src/reconciler)
  add_subdirectory(src/ingestor)
endif()
add_subdirectory(src/services)

if(AT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
