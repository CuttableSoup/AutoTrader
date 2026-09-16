add_library(at_ingestor STATIC service.cpp)
target_link_libraries(at_ingestor PUBLIC at_alpaca at_strategy at_common)
