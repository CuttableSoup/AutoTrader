add_library(at_reconciler STATIC service.cpp)
target_link_libraries(at_reconciler PUBLIC at_alpaca at_common)
