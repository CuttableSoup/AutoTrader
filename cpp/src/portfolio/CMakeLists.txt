add_library(at_portfolio STATIC service.cpp)
target_link_libraries(at_portfolio PUBLIC at_alpaca at_strategy at_common)
