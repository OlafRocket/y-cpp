cmake_minimum_required(VERSION 3.16)
project(ycpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

add_library(ycpp STATIC src/ycpp.cpp)
target_include_directories(ycpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(ycpp-cli tools/ycpp_cli.cpp)
set_target_properties(ycpp-cli PROPERTIES OUTPUT_NAME ycpp)
target_link_libraries(ycpp-cli PRIVATE ycpp)

enable_testing()

add_executable(ycpp_tests tests/test_main.cpp)
target_link_libraries(ycpp_tests PRIVATE ycpp)
add_test(NAME unit COMMAND ycpp_tests)

# Interop conformity tests against the real yjs (requires node + npm).
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME interop
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_interop.sh
                   $<TARGET_FILE:ycpp-cli>)
  set_tests_properties(interop PROPERTIES
    ENVIRONMENT "NODE=${NODE_EXECUTABLE}")
endif()
