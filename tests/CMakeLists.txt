list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(mudclient_tests
    test_telnet_parser.cpp
    test_event_queue.cpp
    test_network_client.cpp)
target_link_libraries(mudclient_tests PRIVATE mudclient_core mudclient_options Catch2::Catch2WithMain)

catch_discover_tests(mudclient_tests DISCOVERY_MODE PRE_TEST)
