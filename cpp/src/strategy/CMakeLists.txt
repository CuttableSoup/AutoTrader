add_library(at_strategy STATIC
  types.cpp
  params.cpp
  market_store.cpp
  universe.cpp
  signal.cpp
  exits.cpp
  sizing.cpp
  engine.cpp
  ledger.cpp
)
target_link_libraries(at_strategy PUBLIC at_common)
