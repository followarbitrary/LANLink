cmake_minimum_required(VERSION 3.23)
project(LANLink LANGUAGES CXX)

include(FetchContent)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_library(lanlink_core
  src/core/common/ids.cpp
  src/core/common/clock.cpp
  src/core/common/liveness.cpp
  src/core/topology/topology.cpp
  src/core/input/arbiter.cpp
  src/core/input/router.cpp
  src/core/input/mocks.cpp
)
target_compile_features(lanlink_core PUBLIC cxx_std_20)
target_include_directories(lanlink_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(lanlink_core PRIVATE /W4)
else()
  target_compile_options(lanlink_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(common_types_test tests/unit/common_types_test.cpp)
target_link_libraries(common_types_test PRIVATE lanlink_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(common_types_test)
add_test(NAME common_types COMMAND common_types_test)

add_executable(topology_test tests/unit/topology_test.cpp)
target_link_libraries(topology_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(topology_test)
add_test(NAME topology COMMAND topology_test)

add_executable(arbiter_test tests/unit/arbiter_test.cpp)
target_link_libraries(arbiter_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(arbiter_test)
add_test(NAME arbiter COMMAND arbiter_test)

add_executable(router_test tests/unit/router_test.cpp)
target_link_libraries(router_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(router_test)
add_test(NAME router COMMAND router_test)
