cmake_minimum_required(VERSION 3.20)
project(gateway_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gateway_app INTERFACE)
add_library(gateway_app::gateway_app ALIAS gateway_app)

target_include_directories(gateway_app INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

if (MSVC)
  target_compile_options(gateway_app INTERFACE /W4 /permissive-)
else()
  target_compile_options(gateway_app INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(.vix/vix_deps.cmake)
target_link_libraries(gateway_app INTERFACE vix::app vix::web_app vix::api_app)

include(CTest)
enable_testing()

add_executable(gateway_app_basic_test tests/test_basic.cpp)
target_link_libraries(gateway_app_basic_test PRIVATE gateway_app::gateway_app)
add_test(NAME gateway_app.basic COMMAND gateway_app_basic_test)
