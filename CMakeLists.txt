cmake_minimum_required(VERSION 3.23)
project(LANLink LANGUAGES CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

option(LANLINK_BUILD_DESKTOP "Build the optional Qt 6 desktop control panel" ON)

option(LANLINK_ENABLE_TLS "Build the OpenSSL TLS transport backend" ON)
option(LANLINK_REQUIRE_TLS "Fail configuration when the OpenSSL TLS backend cannot be built" OFF)

# GoogleTest is vendored so a clean configure does not need network access or
# depend on the contents of a machine-local FetchContent cache.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/googletest
  ${CMAKE_CURRENT_BINARY_DIR}/third-party/googletest
  EXCLUDE_FROM_ALL
)

add_library(lanlink_core
  src/core/common/ids.cpp
  src/core/common/clock.cpp
  src/core/common/debug.cpp
  src/core/common/liveness.cpp
  src/core/discovery/discovery.cpp
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
  src/platform/fake_clipboard_backend.cpp
  src/platform/windows/windows_clipboard_backend.cpp
  src/platform/macos/macos_clipboard_backend.cpp
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
  elseif(LANLINK_REQUIRE_TLS)
    message(FATAL_ERROR
      "LANLINK_REQUIRE_TLS=ON but OpenSSL SSL and Crypto components were not found. "
      "Configure with the pinned dependency toolchain or set LANLINK_ENABLE_TLS=OFF.")
  endif()
endif()

if(LANLINK_BUILD_DESKTOP)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  set(LANLINK_QT6_WIDGETS_FOUND ${Qt6_FOUND})
  if(LANLINK_QT6_WIDGETS_FOUND)
    find_package(Qt6 COMPONENTS Test QUIET)
    find_package(Qt6 COMPONENTS Svg QUIET)
    find_package(Qt6 COMPONENTS Network QUIET)
  endif()
  if(LANLINK_QT6_WIDGETS_FOUND)
    add_executable(lanlink_desktop
      src/desktop/main.cpp
      src/desktop/main_window.cpp
      src/desktop/main_window.hpp
      src/desktop/desktop_state.hpp
      src/desktop/icons.hpp
      src/desktop/runtime_worker.cpp
      src/desktop/runtime_worker.hpp
      src/desktop/topology_canvas.cpp
      src/desktop/topology_canvas.hpp
      src/desktop/pages.cpp
      src/desktop/pages.hpp
      src/desktop/resources.qrc
    )
    set_target_properties(lanlink_desktop PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    if(APPLE)
      target_sources(lanlink_desktop PRIVATE assets/icons/LANLink.icns)
      set_source_files_properties(assets/icons/LANLink.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
      set_target_properties(lanlink_desktop PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aeg.lanlink"
        MACOSX_BUNDLE_BUNDLE_NAME "LANLink"
        MACOSX_BUNDLE_ICON_FILE "LANLink.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1")
    endif()
    if(WIN32)
      target_sources(lanlink_desktop PRIVATE src/desktop/lanlink.rc)
    endif()
    target_compile_features(lanlink_desktop PRIVATE cxx_std_20)
    target_link_libraries(lanlink_desktop PRIVATE Qt6::Widgets)
    if(TARGET Qt6::Svg)
      target_link_libraries(lanlink_desktop PRIVATE Qt6::Svg)
    endif()
    if(TARGET Qt6::Network)
      target_link_libraries(lanlink_desktop PRIVATE Qt6::Network)
      target_compile_definitions(lanlink_desktop PRIVATE LANLINK_HAS_QT_NETWORK=1)
    endif()
    target_include_directories(lanlink_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
      target_compile_options(lanlink_desktop PRIVATE /W4)
    else()
      target_compile_options(lanlink_desktop PRIVATE -Wall -Wextra -Wpedantic)
    endif()

  endif()
endif()
target_compile_definitions(lanlink_core PUBLIC LANLINK_HAS_OPENSSL=${LANLINK_HAS_OPENSSL})
if(APPLE)
  set_source_files_properties(src/platform/macos/macos_clipboard_backend.cpp
    PROPERTIES LANGUAGE OBJCXX)
  target_link_libraries(lanlink_core PRIVATE "-framework ApplicationServices" "-framework AppKit")
endif()
if(WIN32)
  target_link_libraries(lanlink_core PRIVATE user32 ws2_32 ole32 windowscodecs)
endif()
if(MSVC)
  target_compile_options(lanlink_core PRIVATE /W4)
else()
  target_compile_options(lanlink_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME package_windows_dependency_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_windows_dependency_test.py)
endif()
add_executable(common_types_test tests/unit/common_types_test.cpp)
target_link_libraries(common_types_test PRIVATE lanlink_core GTest::gtest_main)
include(GoogleTest)
if(LANLINK_BUILD_DESKTOP AND LANLINK_QT6_WIDGETS_FOUND)
  add_executable(desktop_state_test tests/desktop_state_test.cpp)
  target_compile_features(desktop_state_test PRIVATE cxx_std_20)
  target_include_directories(desktop_state_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/desktop)
  target_link_libraries(desktop_state_test PRIVATE Qt6::Widgets GTest::gtest_main)
  gtest_discover_tests(desktop_state_test)
  add_test(NAME desktop_state COMMAND desktop_state_test)
endif()
gtest_discover_tests(common_types_test)
add_test(NAME common_types COMMAND common_types_test)

add_executable(discovery_test tests/unit/discovery_test.cpp)
target_link_libraries(discovery_test PRIVATE lanlink_core GTest::gtest_main)
gtest_discover_tests(discovery_test)
add_test(NAME discovery COMMAND discovery_test)

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

add_library(lanlink_app_support src/app/options.cpp src/app/runtime_loop.cpp)
target_link_libraries(lanlink_app_support PUBLIC lanlink_core)
target_compile_features(lanlink_app_support PUBLIC cxx_std_20)
if(LANLINK_BUILD_DESKTOP AND LANLINK_QT6_WIDGETS_FOUND)
  target_link_libraries(lanlink_desktop PRIVATE lanlink_app_support)
endif()
if(LANLINK_BUILD_DESKTOP AND LANLINK_QT6_WIDGETS_FOUND AND Qt6Test_FOUND)
  add_executable(runtime_worker_test tests/runtime_worker_test.cpp src/desktop/runtime_worker.cpp src/desktop/runtime_worker.hpp)
  set_target_properties(runtime_worker_test PROPERTIES AUTOMOC ON)
  target_compile_features(runtime_worker_test PRIVATE cxx_std_20)
  target_include_directories(runtime_worker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/desktop)
  target_link_libraries(runtime_worker_test PRIVATE lanlink_app_support Qt6::Widgets Qt6::Test GTest::gtest_main)
  gtest_discover_tests(runtime_worker_test)
  add_test(NAME runtime_worker COMMAND runtime_worker_test)
endif()
if(LANLINK_BUILD_DESKTOP AND LANLINK_QT6_WIDGETS_FOUND AND Qt6Test_FOUND)
  add_executable(main_window_test
    tests/main_window_test.cpp
    src/desktop/main_window.cpp
    src/desktop/main_window.hpp
    src/desktop/pages.cpp
    src/desktop/pages.hpp
    src/desktop/topology_canvas.cpp
    src/desktop/topology_canvas.hpp
    src/desktop/runtime_worker.cpp
    src/desktop/runtime_worker.hpp
    src/desktop/resources.qrc)
  set_target_properties(main_window_test PROPERTIES AUTOMOC ON AUTORCC ON)
  target_compile_features(main_window_test PRIVATE cxx_std_20)
  target_include_directories(main_window_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/desktop)
  target_link_libraries(main_window_test PRIVATE
    lanlink_app_support Qt6::Widgets Qt6::Test GTest::gtest_main)
  if(TARGET Qt6::Network)
    target_link_libraries(main_window_test PRIVATE Qt6::Network)
    target_compile_definitions(main_window_test PRIVATE LANLINK_HAS_QT_NETWORK=1)
  endif()
  gtest_discover_tests(main_window_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  add_test(NAME main_window COMMAND main_window_test)
  set_tests_properties(main_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(app_runtime_loop_test tests/unit/app_runtime_loop_test.cpp)
target_link_libraries(app_runtime_loop_test PRIVATE lanlink_app_support GTest::gtest_main)
target_compile_features(app_runtime_loop_test PRIVATE cxx_std_20)
gtest_discover_tests(app_runtime_loop_test)
add_test(NAME app_runtime_loop COMMAND app_runtime_loop_test)

add_executable(app_entry_test tests/integration/app_entry_test.cpp)
target_compile_features(app_entry_test PRIVATE cxx_std_20)
target_link_libraries(app_entry_test PRIVATE lanlink_core)

add_executable(lanlink_app src/app/main.cpp)
target_link_libraries(lanlink_app PRIVATE lanlink_app_support)
target_compile_features(lanlink_app PRIVATE cxx_std_20)
add_dependencies(app_entry_test lanlink_app)

add_test(NAME app_entry_help COMMAND app_entry_test $<TARGET_FILE:lanlink_app> help)
add_test(NAME app_entry_invalid_port COMMAND app_entry_test $<TARGET_FILE:lanlink_app> invalid-port)
add_test(NAME app_entry_discovery_flow COMMAND app_entry_test $<TARGET_FILE:lanlink_app> discovery-flow)

add_executable(app_options_test tests/unit/app_options_test.cpp)
target_link_libraries(app_options_test PRIVATE lanlink_app_support GTest::gtest_main)
gtest_discover_tests(app_options_test)
add_test(NAME app_options COMMAND app_options_test)
