add_library(at_execution STATIC engine.cpp)
target_link_libraries(at_execution PUBLIC at_alpaca at_common)
