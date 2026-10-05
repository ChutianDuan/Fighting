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

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Libevent CONFIG REQUIRED)

if(LAB_BUILD_CLIENT)
  find_package(SDL2 CONFIG REQUIRED)
  find_package(SDL2_ttf CONFIG REQUIRED)

  if(TARGET SDL2::SDL2)
    set(LAB_SDL2_TARGET SDL2::SDL2)
  elseif(TARGET SDL2::SDL2-static)
    set(LAB_SDL2_TARGET SDL2::SDL2-static)
  else()
    message(FATAL_ERROR "SDL2 CMake target not found")
  endif()

  if(TARGET SDL2_ttf::SDL2_ttf)
    set(LAB_SDL2_TTF_TARGET SDL2_ttf::SDL2_ttf)
  elseif(TARGET SDL2_ttf::SDL2_ttf-static)
    set(LAB_SDL2_TTF_TARGET SDL2_ttf::SDL2_ttf-static)
  else()
    message(FATAL_ERROR "SDL2_ttf CMake target not found")
  endif()
endif()

# ---- Core library sources (NO main.cpp here) ----
set(LAB_CORE_SOURCES
  src/session/ServerRuntime.cpp
  src/session/NetworkSimulator.cpp
  src/session/Replay.cpp
  src/session/Protocol.cpp
  src/session/RoomServer.cpp
  src/session/ClientSession.cpp
  src/time/Clock.cpp
  src/sim/InputBuffer.cpp
  src/sim/World.cpp
  src/sim/Hasher.cpp
  src/net/UdpSocket.cpp
)

add_library(lab_core STATIC ${LAB_CORE_SOURCES})
target_include_directories(lab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_core PUBLIC
  libevent::core
  nlohmann_json::nlohmann_json
)

function(lab_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

lab_enable_warnings(lab_core)

# 旧教学/历史基准只供回归链接，生产入口不包含旧网络协议实现。
if(LAB_BUILD_TESTS)
  add_library(lab_teaching STATIC
    src/net/NetStub.cpp
    src/net/NetCode.cpp
    src/server/AuthoritativeServer.cpp
    src/io/RecordReplay.cpp
    src/core/FixedTimestepRunner.cpp)
  target_link_libraries(lab_teaching PUBLIC lab_core)
  lab_enable_warnings(lab_teaching)
endif()

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
  target_link_libraries(lab_client PRIVATE lab_core ${LAB_SDL2_TARGET} ${LAB_SDL2_TTF_TARGET})
  if(TARGET SDL2::SDL2main)
    target_link_libraries(lab_client PRIVATE SDL2::SDL2main)
  endif()
  lab_enable_warnings(lab_client)
endif()

if(LAB_BUILD_TESTS)
  enable_testing()
  add_executable(lab_replay_tests tests/replay_tests.cpp)
  target_link_libraries(lab_replay_tests PRIVATE lab_core)
  lab_enable_warnings(lab_replay_tests)
  add_test(NAME lab_replay_tests COMMAND lab_replay_tests)
  add_executable(lab_v6_udp_tests tests/v6_udp_tests.cpp)
  target_link_libraries(lab_v6_udp_tests PRIVATE lab_core)
  lab_enable_warnings(lab_v6_udp_tests)
  add_test(NAME lab_v6_udp_tests COMMAND lab_v6_udp_tests)
  add_executable(lab_session_tests tests/session_tests.cpp)
  target_link_libraries(lab_session_tests PRIVATE lab_core)
  lab_enable_warnings(lab_session_tests)
  add_test(NAME lab_session_tests COMMAND lab_session_tests)

  add_executable(lab_tests tests/core_tests.cpp)
  target_link_libraries(lab_tests PRIVATE lab_teaching)
  lab_enable_warnings(lab_tests)
  add_test(NAME lab_tests COMMAND lab_tests)

  if(LAB_BUILD_STRESS)
    add_executable(lab_stress tests/stress_tests.cpp)
    target_link_libraries(lab_stress PRIVATE lab_teaching)
    lab_enable_warnings(lab_stress)
    add_test(NAME lab_stress_smoke COMMAND lab_stress --ticks 3000 --history 512 --state-delay 5)
  endif()

  if(LAB_BUILD_NETWORK_INTEGRATION)
    add_executable(lab_network_integration tests/network_integration_tests.cpp)
    target_link_libraries(lab_network_integration PRIVATE lab_teaching)
    lab_enable_warnings(lab_network_integration)
    add_test(NAME lab_network_integration COMMAND lab_network_integration)
  endif()

  if(LAB_BUILD_PERFORMANCE)
    add_executable(lab_performance
      tests/performance/performance_tests.cpp
    )
    target_link_libraries(lab_performance PRIVATE lab_teaching)
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

add_executable(lab_network_lab apps/network_lab_main.cpp)
target_link_libraries(lab_network_lab PRIVATE lab_core)
lab_enable_warnings(lab_network_lab)
if(LAB_BUILD_TESTS)
  add_test(NAME lab_network_lab_smoke COMMAND lab_network_lab --ticks 300 --seed 42)
endif()

add_executable(lab_replay apps/replay_main.cpp)
target_link_libraries(lab_replay PRIVATE lab_core)
lab_enable_warnings(lab_replay)

add_executable(lab_udp_bots apps/udp_bots_main.cpp)
target_link_libraries(lab_udp_bots PRIVATE lab_core)
lab_enable_warnings(lab_udp_bots)
if(LAB_BUILD_TESTS)
  add_test(NAME lab_udp_bots_smoke COMMAND lab_udp_bots --rooms 2 --warmup 0.1 --sample 1 --repeat 1)
endif()
