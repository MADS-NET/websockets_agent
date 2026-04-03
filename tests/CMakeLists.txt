add_executable(mads-websockets-test
  test_bridge.cpp
)

target_link_libraries(mads-websockets-test PRIVATE libMadsWebsockets)

add_test(NAME mads-websockets-test COMMAND mads-websockets-test)
