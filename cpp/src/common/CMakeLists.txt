add_library(at_common STATIC
  money.cpp
  uuid.cpp
  sha256.cpp
  time.cpp
  ids.cpp
  calendar.cpp
  indicators.cpp
  envelope.cpp
  bus.cpp
  logging.cpp
  config.cpp
  secrets.cpp
  schema.cpp
)
target_include_directories(at_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(at_common PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog at::jsonschema)
if(CYGWIN)
  # cygwin's libstdc++ needs this for std::chrono tzdb-free paths and large files
  target_compile_definitions(at_common PUBLIC _GNU_SOURCE)
endif()

if(AT_WITH_NATS)
  add_library(at_natsbus STATIC nats_bus.cpp)
  target_link_libraries(at_natsbus PUBLIC at_common at::nats)
endif()
