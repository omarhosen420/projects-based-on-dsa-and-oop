cmake_minimum_required(VERSION 3.16)
project(mmf_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)

add_library(mmf_core
  src/config.cpp
  src/lobby.cpp
  src/matchmaker.cpp
  src/observers.cpp
  src/strategy.cpp
)
target_include_directories(mmf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mmf_core PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(mmf_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(mmf_demo src/main.cpp)
target_link_libraries(mmf_demo PRIVATE mmf_core)

add_executable(mmf_bench bench/bench_mmf.cpp)
target_link_libraries(mmf_bench PRIVATE mmf_core)

add_executable(mmf_tests
  tests/test_main.cpp
  tests/test_interval_tree.cpp
  tests/test_heap.cpp
  tests/test_lobby.cpp
  tests/test_strategy.cpp
  tests/test_engine.cpp
  tests/test_concurrency.cpp
)
target_include_directories(mmf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(mmf_tests PRIVATE mmf_core)

enable_testing()
add_test(NAME mmf_tests COMMAND mmf_tests)
