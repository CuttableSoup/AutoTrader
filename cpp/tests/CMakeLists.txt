add_executable(at_tests
  test_money.cpp
  test_time_calendar.cpp
  test_indicators.cpp
  test_ids_envelope_bus.cpp
  test_signal.cpp
  test_exits_sizing.cpp
  test_tsmom_signal.cpp
  test_tsmom_sizing.cpp
  test_tsmom_engine.cpp
  test_tsmom_ledger.cpp
  test_tsmom_risk.cpp
  test_risk.cpp
  test_metrics.cpp
  test_dsr_cli.cpp
  test_mock_validator.cpp
  test_schema.cpp
  test_backtest_smoke.cpp
  test_tsmom_backtest.cpp
)
target_link_libraries(at_tests PRIVATE at_backtest at_risk at_strategy at_common Catch2::Catch2WithMain)
target_compile_definitions(at_tests PRIVATE AT_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/..")
include(Catch)
catch_discover_tests(at_tests)
