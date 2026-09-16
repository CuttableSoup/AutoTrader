add_library(at_backtest STATIC
  mock_validator.cpp
  metrics.cpp
  backtester.cpp
  walk_forward.cpp
)
target_link_libraries(at_backtest PUBLIC at_risk at_strategy at_common)
