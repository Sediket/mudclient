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
    test_script_engine.cpp
    test_lua_escaping.cpp
    critic/critic_lua_sandbox.cpp
    critic/critic_lua_coroutine_budget_amplification.cpp
    critic/critic_reset_timer_binding_coverage.cpp
    critic/critic_pump_resumes_shared_budget.cpp)
target_link_libraries(mudclient_tests PRIVATE mudclient_core mudclient_options Catch2::Catch2WithMain)

catch_discover_tests(mudclient_tests DISCOVERY_MODE PRE_TEST)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME replay_zombiemud
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/run_replay_test.py"
        --fake-mud "$<TARGET_FILE:fake_mud>"
        --client "$<TARGET_FILE:mudclient>"
        --record "${CMAKE_CURRENT_SOURCE_DIR}/replay/zombiemud_session.jsonl"
        --script "${CMAKE_CURRENT_SOURCE_DIR}/live/zombiemud.lua"
        --speed 10
        --timeout 60)
