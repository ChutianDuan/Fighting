cmake_minimum_required(VERSION 3.20)
project(fighter_netcode_demo LANGUAGES CXX)

option(LAB_BUILD_SERVER "Build lab_server executable" ON)
option(LAB_BUILD_CLIENT "Build lab_client SDL executable" ON)
option(LAB_BUILD_TESTS "Build lab regression and integration tests" ON)
option(LAB_BUILD_STRESS "Build lab_stress when LAB_BUILD_TESTS is ON" ON)
option(LAB_BUILD_NETWORK_INTEGRATION "Build lab_network_integration when LAB_BUILD_TESTS is ON" ON)
option(LAB_BUILD_PERFORMANCE "Build and register the full performance suite" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG QUIET)

if(LAB_BUILD_CLIENT)
  find_package(SDL2 CONFIG REQUIRED)
  find_package(SDL2_ttf CONFIG REQUIRED)
endif()

set(LIBEVENT_HINTS
  $ENV{CONDA_PREFIX}
  $ENV{LIBEVENT_ROOT}
  /usr/local
  /usr
  /opt/homebrew
  /opt
)

find_path(LIBEVENT_INCLUDE_DIR event2/event.h
  HINTS ${LIBEVENT_HINTS}
  PATH_SUFFIXES include)
find_library(LIBEVENT_LIBRARY event
  HINTS ${LIBEVENT_HINTS}
  PATH_SUFFIXES lib)

if(NOT LIBEVENT_INCLUDE_DIR OR NOT LIBEVENT_LIBRARY)
  message(FATAL_ERROR "libevent not found. Set CONDA_PREFIX/LIBEVENT_ROOT or install libevent dev package.")
endif()

# ---- Core library sources (NO main.cpp here) ----
set(LAB_CORE_SOURCES
  src/time/Clock.cpp
  src/sim/InputBuffer.cpp
  src/sim/World.cpp
  src/sim/Hasher.cpp
  src/net/NetStub.cpp
  src/net/NetCode.cpp
  src/net/UdpSocket.cpp
  src/server/AuthoritativeServer.cpp
  src/io/RecordReplay.cpp
  src/core/FixedTimestepRunner.cpp
)

add_library(lab_core STATIC ${LAB_CORE_SOURCES})
target_include_directories(lab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(lab_core PUBLIC ${LIBEVENT_INCLUDE_DIR})

target_link_libraries(lab_core PUBLIC 
  ${LIBEVENT_LIBRARY}
)

if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(lab_core PUBLIC nlohmann_json::nlohmann_json)
endif()

function(lab_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

lab_enable_warnings(lab_core)

if(LAB_BUILD_SERVER)
  add_executable(lab_server apps/server_main.cpp)
  target_link_libraries(lab_server PRIVATE lab_core)
  lab_enable_warnings(lab_server)
endif()

if(LAB_BUILD_CLIENT)
  add_executable(lab_client
    apps/client_main.cpp
    src/app/InputPrediction.cpp
    src/app/ClientRender.cpp
  )
  target_link_libraries(lab_client PRIVATE lab_core)
  target_link_libraries(lab_client PRIVATE SDL2::SDL2 SDL2_ttf::SDL2_ttf)
  lab_enable_warnings(lab_client)
endif()

if(LAB_BUILD_TESTS)
  enable_testing()

  add_executable(lab_tests tests/core_tests.cpp)
  target_link_libraries(lab_tests PRIVATE lab_core)
  lab_enable_warnings(lab_tests)
  add_test(NAME lab_tests COMMAND lab_tests)

  if(LAB_BUILD_STRESS)
    add_executable(lab_stress tests/stress_tests.cpp)
    target_link_libraries(lab_stress PRIVATE lab_core)
    lab_enable_warnings(lab_stress)
    add_test(NAME lab_stress_smoke COMMAND lab_stress --ticks 3000 --history 512 --state-delay 5)
  endif()

  if(LAB_BUILD_NETWORK_INTEGRATION)
    add_executable(lab_network_integration tests/network_integration_tests.cpp)
    target_link_libraries(lab_network_integration PRIVATE lab_core)
    lab_enable_warnings(lab_network_integration)
    add_test(NAME lab_network_integration COMMAND lab_network_integration)
  endif()

  if(LAB_BUILD_PERFORMANCE)
    add_executable(lab_performance
      tests/performance/performance_tests.cpp
    )
    target_link_libraries(lab_performance PRIVATE lab_core)
    lab_enable_warnings(lab_performance)
    add_test(NAME lab_performance_standard
      COMMAND lab_performance
        --profile standard
        --mode all
        --json ${CMAKE_CURRENT_BINARY_DIR}/performance_results.json)
    set_tests_properties(lab_performance_standard PROPERTIES
      LABELS performance
      RUN_SERIAL TRUE
      TIMEOUT 900)
  endif()
endif()
