list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(mudclient_tests
    test_telnet_parser.cpp
    test_event_queue.cpp
    test_network_client.cpp
    test_regex_pattern.cpp
    test_timer_manager.cpp
    test_trigger_manager.cpp
    test_alias_manager.cpp
    test_command_parser.cpp
    test_script_engine.cpp)
target_link_libraries(mudclient_tests PRIVATE mudclient_core mudclient_options Catch2::Catch2WithMain)

catch_discover_tests(mudclient_tests DISCOVERY_MODE PRE_TEST)
