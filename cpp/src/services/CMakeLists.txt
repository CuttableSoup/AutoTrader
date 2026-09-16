add_executable(at_backtester backtester_main.cpp)
target_link_libraries(at_backtester PRIVATE at_backtest)

if(AT_WITH_NATS)
  add_library(at_service_common STATIC service_common.cpp)
  target_link_libraries(at_service_common PUBLIC at_risk at_strategy at_natsbus at_common)

  add_executable(at_strategy_svc strategy_main.cpp)
  target_link_libraries(at_strategy_svc PRIVATE at_service_common)

  add_executable(at_risk_svc risk_main.cpp)
  target_link_libraries(at_risk_svc PRIVATE at_service_common)
endif()

if(AT_WITH_NATS AND AT_WITH_NET)
  add_executable(at_execution_svc execution_main.cpp)
  target_link_libraries(at_execution_svc PRIVATE at_execution at_service_common)

  add_executable(at_portfolio_svc portfolio_main.cpp)
  target_link_libraries(at_portfolio_svc PRIVATE at_portfolio at_service_common)

  add_executable(at_reconciler_svc reconciler_main.cpp)
  target_link_libraries(at_reconciler_svc PRIVATE at_reconciler at_service_common)

  add_executable(at_ingestor_svc ingestor_main.cpp)
  target_link_libraries(at_ingestor_svc PRIVATE at_ingestor at_service_common)
endif()
