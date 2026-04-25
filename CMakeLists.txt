cmake_minimum_required(VERSION 3.20)
project(fighter_netcode_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

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
  src/io/RecordReplay.cpp
  src/core/FixedTimestepRunner.cpp
)

add_library(lab_core STATIC ${LAB_CORE_SOURCES})
target_include_directories(lab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(lab_core PUBLIC ${LIBEVENT_INCLUDE_DIR})

# nlohmann_json 是 header-only，但仍推荐用 target link 的方式传播依赖
target_link_libraries(lab_core PUBLIC 
  nlohmann_json::nlohmann_json
  ${LIBEVENT_LIBRARY}
)

function(lab_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

lab_enable_warnings(lab_core)

# ---- Executables ----
add_executable(lab_server apps/server_main.cpp)
target_link_libraries(lab_server PRIVATE lab_core)
lab_enable_warnings(lab_server)

add_executable(lab_client
  apps/client_main.cpp
  src/app/InputPrediction.cpp
  src/app/ClientRender.cpp
)
target_link_libraries(lab_client PRIVATE lab_core)
target_link_libraries(lab_client PRIVATE SDL2::SDL2 SDL2_ttf::SDL2_ttf)
lab_enable_warnings(lab_client)

option(LAB_BUILD_TESTS "Build lab regression tests" ON)
if(LAB_BUILD_TESTS)
  enable_testing()
  add_executable(lab_tests tests/core_tests.cpp)
  target_link_libraries(lab_tests PRIVATE lab_core)
  lab_enable_warnings(lab_tests)
  add_test(NAME lab_tests COMMAND lab_tests)

  add_executable(lab_stress tests/stress_tests.cpp)
  target_link_libraries(lab_stress PRIVATE lab_core)
  lab_enable_warnings(lab_stress)
  add_test(NAME lab_stress_smoke COMMAND lab_stress --ticks 3000 --history 512 --state-delay 5)
endif()
