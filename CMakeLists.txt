cmake_minimum_required(VERSION 3.23)
project(LANLink LANGUAGES CXX)

option(LANLINK_ENABLE_TLS "Build the OpenSSL TLS transport backend" ON)

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
  src/core/protocol/wire.cpp
  src/core/transport/connection.cpp
  src/core/transport/session.cpp
  src/core/transport/manager.cpp
  src/core/runtime/node.cpp
  src/platform/fake_input_backend.cpp
  src/platform/key_mapping.cpp
  src/platform/windows/windows_input_backend.cpp
  src/platform/macos/macos_input_backend.cpp
)
target_compile_features(lanlink_core PUBLIC cxx_std_20)
target_include_directories(lanlink_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(LANLINK_HAS_OPENSSL 0)
if(LANLINK_ENABLE_TLS)
  find_package(OpenSSL COMPONENTS SSL Crypto QUIET)
  if(OpenSSL_FOUND)
    set(LANLINK_HAS_OPENSSL 1)
    target_link_libraries(lanlink_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  endif()
endif()
target_compile_definitions(lanlink_core PUBLIC LANLINK_HAS_OPENSSL=${LANLINK_HAS_OPENSSL})
if(APPLE)
  target_link_libraries(lanlink_core PRIVATE "-framework ApplicationServices")
endif()
if(WIN32)
  target_link_libraries(lanlink_core PRIVATE user32 ws2_32)
endif()
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

add_executable(protocol_wire_test tests/unit/protocol_wire_test.cpp)
target_link_libraries(protocol_wire_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(protocol_wire_test)
add_test(NAME protocol_wire COMMAND protocol_wire_test)

add_executable(transport_connection_test tests/unit/transport_connection_test.cpp)
target_link_libraries(transport_connection_test PRIVATE lanlink_core GTest::gtest_main)
target_compile_definitions(transport_connection_test PRIVATE
  LANLINK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
gtest_discover_tests(transport_connection_test)
add_test(NAME transport_connection COMMAND transport_connection_test)

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

add_executable(m0_two_node_test tests/integration/m0_two_node_test.cpp)
target_link_libraries(m0_two_node_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(m0_two_node_test)
add_test(NAME m0_two_node COMMAND m0_two_node_test)

add_executable(recovery_test tests/fault_injection/recovery_test.cpp)
target_link_libraries(recovery_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(recovery_test)
add_test(NAME recovery COMMAND recovery_test)

add_executable(backend_contract_test tests/unit/backend_contract_test.cpp)
target_link_libraries(backend_contract_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(backend_contract_test)
add_test(NAME backend_contract COMMAND backend_contract_test)

add_executable(windows_backend_test tests/unit/windows_backend_test.cpp)
target_link_libraries(windows_backend_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(windows_backend_test)
add_test(NAME windows_backend COMMAND windows_backend_test)

add_executable(key_mapping_test tests/unit/key_mapping_test.cpp)
target_link_libraries(key_mapping_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(key_mapping_test)
add_test(NAME key_mapping COMMAND key_mapping_test)

add_executable(macos_backend_test tests/unit/macos_backend_test.cpp)
target_link_libraries(macos_backend_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(macos_backend_test)
add_test(NAME macos_backend COMMAND macos_backend_test)

add_executable(platform_recovery_test tests/integration/platform_recovery_test.cpp)
target_link_libraries(platform_recovery_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(platform_recovery_test)
add_test(NAME platform_recovery COMMAND platform_recovery_test)

add_executable(transport_session_test tests/integration/transport_session_test.cpp)
target_link_libraries(transport_session_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(transport_session_test)
add_test(NAME transport_session COMMAND transport_session_test)

add_executable(transport_manager_test tests/integration/transport_manager_test.cpp)
target_link_libraries(transport_manager_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(transport_manager_test)
add_test(NAME transport_manager COMMAND transport_manager_test)

add_executable(input_node_runtime_test tests/integration/input_node_runtime_test.cpp)
target_link_libraries(input_node_runtime_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(input_node_runtime_test)
add_test(NAME input_node_runtime COMMAND input_node_runtime_test)

add_library(lanlink_app_support src/app/options.cpp)
target_link_libraries(lanlink_app_support PUBLIC lanlink_core)
target_compile_features(lanlink_app_support PUBLIC cxx_std_20)

add_executable(app_options_test tests/unit/app_options_test.cpp)
target_link_libraries(app_options_test PRIVATE lanlink_app_support GTest::gtest_main)
gtest_discover_tests(app_options_test)
add_test(NAME app_options COMMAND app_options_test)
