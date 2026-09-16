add_library(at_alpaca STATIC
  http.cpp
  client.cpp
  trade_stream.cpp
)
target_link_libraries(at_alpaca PUBLIC at_strategy at_common at::curl at::ixwebsocket)
