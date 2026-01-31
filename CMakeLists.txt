cmake_minimum_required(VERSION 3.16)
project(yamux VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(YAMUX_BUILD_TESTS "Build tests" ON)

find_package(Threads REQUIRED)

# Library
add_library(yamux
    yamux/frame.cpp
    yamux/stream.cpp
    yamux/session.cpp
)
target_include_directories(yamux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yamux PUBLIC Threads::Threads)

# Tests
if(YAMUX_BUILD_TESTS)
    enable_testing()
    add_executable(yamux_tests
        test/frame_test.cpp
        test/session_test.cpp
        test/e2e_test.cpp
        test/test_main.cpp
    )
    target_link_libraries(yamux_tests PRIVATE yamux)
    add_test(NAME yamux_tests COMMAND yamux_tests)
endif()
