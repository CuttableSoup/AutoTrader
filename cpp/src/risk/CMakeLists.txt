add_library(at_risk STATIC
  limits.cpp
  risk_manager.cpp
)
target_link_libraries(at_risk PUBLIC at_strategy at_common)
