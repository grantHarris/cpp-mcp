cmake_minimum_required(VERSION 3.14)

set(TEST_PROJECT_NAME "mcp_tests")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Threads REQUIRED)

# Fetch Google Test if not already available
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Test sources
set(TEST_SOURCES
    mcp_test.cpp
)

# Create test executable
add_executable(${TEST_PROJECT_NAME} ${TEST_SOURCES})

target_link_libraries(${TEST_PROJECT_NAME} PRIVATE
    mcp
    gtest
    gtest_main
    Threads::Threads
)

# Suppress warnings in test code for external library headers
target_compile_options(${TEST_PROJECT_NAME} PRIVATE -w)

if(APPLE)
    set_target_properties(${TEST_PROJECT_NAME} PROPERTIES LINK_FLAGS "-Wl,-no_warn_duplicate_libraries")
endif()

# If OpenSSL is found, link OpenSSL libraries
if(OPENSSL_FOUND)
    target_link_libraries(${TEST_PROJECT_NAME} PRIVATE ${OPENSSL_LIBRARIES})
endif()

enable_testing()
add_test(NAME ${TEST_PROJECT_NAME} COMMAND ${TEST_PROJECT_NAME})

add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose
    DEPENDS ${TEST_PROJECT_NAME}
)
