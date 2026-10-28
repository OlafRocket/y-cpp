cmake_minimum_required(VERSION 3.16)
project(ycpp VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # editor tooling (IntelliSense/clangd)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

add_library(ycpp STATIC
  src/doc.cpp
  src/integrate.cpp
  src/store.cpp
  src/transaction.cpp
  src/position.cpp
  src/types.cpp
  src/undo.cpp
  src/update_codec.cpp)
target_include_directories(ycpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# y-protocols port (sync + awareness): separate target, mirroring the
# upstream yjs / y-protocols package split. The core stays protocol-pure.
add_library(ycpp-protocols STATIC src/protocols/awareness.cpp)
target_link_libraries(ycpp-protocols PUBLIC ycpp)

add_executable(ycpp-cli tools/ycpp_cli.cpp)
set_target_properties(ycpp-cli PROPERTIES OUTPUT_NAME ycpp)
target_link_libraries(ycpp-cli PRIVATE ycpp)

# Live sync tool (WebSocket server/client + terminal text box). Uses
# IXWebSocket; the ycpp library itself stays free of any network code.
option(YCPP_BUILD_SYNC "Build the ycpp-sync live sync tool (fetches IXWebSocket)" ON)
if(YCPP_BUILD_SYNC)
  include(FetchContent)
  set(USE_TLS OFF CACHE BOOL "" FORCE)
  set(USE_ZLIB OFF CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.6
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(ixwebsocket)
  add_executable(ycpp-sync tools/ycpp_sync.cpp)
  target_link_libraries(ycpp-sync PRIVATE ycpp ycpp-protocols ixwebsocket)
  target_compile_definitions(ycpp-sync PRIVATE
    YCPP_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
endif()

enable_testing()

add_executable(ycpp_tests tests/test_main.cpp)
target_link_libraries(ycpp_tests PRIVATE ycpp ycpp-protocols)
add_test(NAME unit COMMAND ycpp_tests)

# Interop conformity tests against the real yjs (requires node + npm).
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME interop
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_interop.sh
                   $<TARGET_FILE:ycpp-cli>)
  set_tests_properties(interop PROPERTIES
    ENVIRONMENT "NODE=${NODE_EXECUTABLE}")
  # Extra interop jobs against other yjs versions, so an upstream behaviour
  # change fails a test. Each version is installed on first run under
  # tests/interop/.yjs-versions/. The default job stays on the version pinned
  # in tests/interop/package.json.
  option(YCPP_INTEROP_LATEST13 "Interop job against the latest yjs 13.x (npm spec yjs@13)" ON)
  set(YCPP_INTEROP_V14_SPEC "" CACHE STRING
    "npm spec of a yjs 14 package for an optional interop job, e.g. @y/y@14.0.0-rc.29 (empty: off)")
  if(YCPP_INTEROP_LATEST13)
    add_test(NAME interop-yjs13-latest
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_interop.sh
                     $<TARGET_FILE:ycpp-cli>)
    set_tests_properties(interop-yjs13-latest PROPERTIES
      ENVIRONMENT "NODE=${NODE_EXECUTABLE};YCPP_INTEROP_YJS=yjs@13")
  endif()
  if(YCPP_INTEROP_V14_SPEC)
    add_test(NAME interop-yjs14
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_interop.sh
                     $<TARGET_FILE:ycpp-cli>)
    set_tests_properties(interop-yjs14 PROPERTIES
      ENVIRONMENT "NODE=${NODE_EXECUTABLE};YCPP_INTEROP_YJS=${YCPP_INTEROP_V14_SPEC}")
  endif()
  if(YCPP_BUILD_SYNC)
    add_test(NAME websocket
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_ws_test.sh
                     $<TARGET_FILE:ycpp-sync>)
    set_tests_properties(websocket PROPERTIES
      ENVIRONMENT "NODE=${NODE_EXECUTABLE}")
  endif()
endif()
