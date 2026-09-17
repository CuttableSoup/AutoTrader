add_library(at_backtest STATIC
  mock_validator.cpp
  metrics.cpp
  dsr_cli.cpp
  backtester.cpp
  walk_forward.cpp
  tsmom_backtester.cpp
)
target_link_libraries(at_backtest PUBLIC at_risk at_strategy at_common)
