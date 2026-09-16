# Dependency resolution. Two providers, one set of imported target names:
#   nlohmann_json::nlohmann_json, spdlog::spdlog, Catch2::Catch2WithMain,
#   nlohmann_json_schema_validator::validator, at::nats, at::curl, at::ixwebsocket
include(FetchContent)

if(AT_DEPS STREQUAL "auto")
  if(CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
    set(AT_DEPS "vcpkg")
  else()
    set(AT_DEPS "fetchcontent")
  endif()
endif()
message(STATUS "AT_DEPS = ${AT_DEPS}")

if(AT_DEPS STREQUAL "vcpkg")
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(nlohmann_json_schema_validator CONFIG REQUIRED)
  find_package(spdlog CONFIG REQUIRED)
  add_library(at::jsonschema INTERFACE IMPORTED)
  target_link_libraries(at::jsonschema INTERFACE nlohmann_json_schema_validator::validator)
  if(AT_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
  endif()
  if(AT_WITH_NATS)
    find_package(cnats CONFIG REQUIRED)
    add_library(at::nats INTERFACE IMPORTED)
    target_link_libraries(at::nats INTERFACE cnats::nats_static)
  endif()
  if(AT_WITH_NET)
    find_package(CURL CONFIG REQUIRED)
    find_package(ixwebsocket CONFIG REQUIRED)
    add_library(at::curl INTERFACE IMPORTED)
    target_link_libraries(at::curl INTERFACE CURL::libcurl)
    add_library(at::ixwebsocket INTERFACE IMPORTED)
    target_link_libraries(at::ixwebsocket INTERFACE ixwebsocket::ixwebsocket)
  endif()

else() # fetchcontent
  set(FETCHCONTENT_QUIET OFF)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)

  set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE INTERNAL "")
  set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(JSON_VALIDATOR_INSTALL OFF CACHE INTERNAL "")
  set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE INTERNAL "")
  FetchContent_Declare(json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG 2.3.0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(json_schema_validator)
  add_library(at::jsonschema INTERFACE IMPORTED)
  target_link_libraries(at::jsonschema INTERFACE nlohmann_json_schema_validator)

  set(SPDLOG_BUILD_EXAMPLE OFF CACHE INTERNAL "")
  set(SPDLOG_BUILD_TESTS OFF CACHE INTERNAL "")
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(spdlog)

  if(AT_BUILD_TESTS)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.8.1 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
  endif()

  if(AT_WITH_NATS OR AT_WITH_NET)
    find_package(OpenSSL REQUIRED)
  endif()

  if(AT_WITH_NATS)
    set(NATS_BUILD_WITH_TLS ON CACHE INTERNAL "")
    set(NATS_BUILD_STREAMING OFF CACHE INTERNAL "")
    set(NATS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(NATS_BUILD_LIB_SHARED OFF CACHE INTERNAL "")
    set(NATS_BUILD_LIB_STATIC ON CACHE INTERNAL "")
    set(NATS_BUILD_TLS_FORCE_HOST_VERIFY ON CACHE INTERNAL "")
    set(BUILD_TESTING OFF CACHE INTERNAL "")
    FetchContent_Declare(cnats
      GIT_REPOSITORY https://github.com/nats-io/nats.c.git
      GIT_TAG v3.10.1 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(cnats)
    add_library(at::nats INTERFACE IMPORTED)
    target_link_libraries(at::nats INTERFACE nats_static OpenSSL::SSL OpenSSL::Crypto)
    target_include_directories(at::nats INTERFACE ${cnats_SOURCE_DIR}/src)
    if(WIN32 AND NOT CYGWIN)
      target_link_libraries(at::nats INTERFACE ws2_32)
    endif()
  endif()

  if(AT_WITH_NET)
    find_package(CURL REQUIRED)
    add_library(at::curl INTERFACE IMPORTED)
    target_link_libraries(at::curl INTERFACE CURL::libcurl)

    set(USE_TLS ON CACHE INTERNAL "")
    set(USE_OPEN_SSL ON CACHE INTERNAL "")
    set(USE_ZLIB OFF CACHE INTERNAL "")
    set(IXWEBSOCKET_INSTALL OFF CACHE INTERNAL "")
    FetchContent_Declare(ixwebsocket
      GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
      GIT_TAG v11.4.6 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(ixwebsocket)
    if(CYGWIN)
      # cygwin hides getaddrinfo/addrinfo behind feature macros under strict -std=c++20
      target_compile_definitions(ixwebsocket PRIVATE _GNU_SOURCE)
    endif()
    add_library(at::ixwebsocket INTERFACE IMPORTED)
    target_link_libraries(at::ixwebsocket INTERFACE ixwebsocket)
  endif()
endif()
