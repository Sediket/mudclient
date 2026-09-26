cmake_minimum_required(VERSION 3.25)

project(mudclient VERSION 0.1.0 LANGUAGES C CXX)

# CI may override the version string (e.g. from a git tag) without editing
# the project() call.
set(MUDCLIENT_VERSION_OVERRIDE "" CACHE STRING "Override the version string reported by --version")
if(MUDCLIENT_VERSION_OVERRIDE)
    set(MUDCLIENT_VERSION "${MUDCLIENT_VERSION_OVERRIDE}")
else()
    set(MUDCLIENT_VERSION "${PROJECT_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Must be set before any target (including dependencies) is created so the
# whole program uses the static MSVC runtime consistently.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ENABLE_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer (non-MSVC only)" OFF)
option(ENABLE_TESTS "Build unit and integration tests" ON)

if(ENABLE_ASAN AND MSVC)
    message(FATAL_ERROR "ENABLE_ASAN is not supported with MSVC")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---- Dependencies (all via FetchContent, declared SYSTEM) ----

FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW ON
    SYSTEM)
FetchContent_MakeAvailable(asio)
add_library(asio_standalone INTERFACE)
target_include_directories(asio_standalone SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
target_compile_definitions(asio_standalone INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON
    SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

set(PCRE2_BUILD_TESTS OFF CACHE INTERNAL "")
set(PCRE2_BUILD_PCRE2GREP OFF CACHE INTERNAL "")
set(PCRE2_BUILD_PCRE2TEST OFF CACHE INTERNAL "")
set(PCRE2_BUILD_PCRE2GPERF OFF CACHE INTERNAL "")
set(PCRE2_BUILD_DOCS OFF CACHE INTERNAL "")
set(PCRE2_SUPPORT_JIT ON CACHE INTERNAL "")
set(PCRE2_STATIC_PIC ON CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(pcre2
    GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
    GIT_TAG pcre2-10.45
    GIT_SHALLOW ON
    SYSTEM)
FetchContent_MakeAvailable(pcre2)

if(ENABLE_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.4
        GIT_SHALLOW ON
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- Project-wide settings for our own targets only ----

add_library(mudclient_options INTERFACE)
if(MSVC)
    target_compile_options(mudclient_options INTERFACE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(mudclient_options INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()
if(WIN32)
    target_compile_definitions(mudclient_options INTERFACE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(ENABLE_ASAN)
    target_compile_options(mudclient_options INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(mudclient_options INTERFACE -fsanitize=address,undefined)
endif()

# ---- Core library ----

add_library(mudclient_core STATIC
    src/telnet_parser.cpp
    src/network_client.cpp
    src/platform/console.cpp
    src/regex_pattern.cpp
    src/timer_manager.cpp
    src/trigger_manager.cpp
    src/alias_manager.cpp
    src/command_parser.cpp)
target_include_directories(mudclient_core PUBLIC include PRIVATE src)
target_link_libraries(mudclient_core
    PUBLIC asio_standalone nlohmann_json::nlohmann_json Threads::Threads pcre2-8-static
    PRIVATE mudclient_options)
target_compile_options(mudclient_core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
if(WIN32)
    target_compile_definitions(mudclient_core PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(mudclient_core PUBLIC ws2_32 mswsock)
endif()
if(ENABLE_ASAN)
    # Sanitizer link flags must reach every executable linking the core.
    target_link_options(mudclient_core PUBLIC -fsanitize=address,undefined)
endif()

# ---- Executable ----

add_executable(mudclient src/main.cpp)
target_include_directories(mudclient PRIVATE src)
target_link_libraries(mudclient PRIVATE mudclient_core mudclient_options)
target_compile_definitions(mudclient PRIVATE MUDCLIENT_VERSION="${MUDCLIENT_VERSION}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ENABLE_ASAN)
    target_link_options(mudclient PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- Benchmarks ----

add_subdirectory(bench)

# ---- Tests ----

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Install / packaging ----

install(TARGETS mudclient RUNTIME DESTINATION .)
install(FILES scripts/init.lua DESTINATION scripts OPTIONAL)
install(FILES LICENSE README.md DESTINATION .)

if(WIN32)
    set(MUDCLIENT_PKG_OS "windows")
    set(CPACK_GENERATOR "ZIP")
else()
    set(MUDCLIENT_PKG_OS "linux")
    set(CPACK_GENERATOR "TGZ")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" MUDCLIENT_PKG_ARCH)
if(MUDCLIENT_PKG_ARCH STREQUAL "amd64")
    set(MUDCLIENT_PKG_ARCH "x86_64")
elseif(MUDCLIENT_PKG_ARCH STREQUAL "arm64")
    set(MUDCLIENT_PKG_ARCH "aarch64")
endif()
set(CPACK_PACKAGE_NAME "mudclient")
set(CPACK_PACKAGE_VERSION "${MUDCLIENT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "mudclient-${MUDCLIENT_VERSION}-${MUDCLIENT_PKG_OS}-${MUDCLIENT_PKG_ARCH}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
