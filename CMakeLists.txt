cmake_minimum_required(VERSION 3.25)

project(norn VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(NORN_BUILD_TESTS "Build Norn tests" ON)
option(NORN_BUILD_BENCHMARKS "Build Norn benchmarks" ON)
option(NORN_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(norn INTERFACE)
target_include_directories(norn INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(norn INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(norn INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(NORN_WARNINGS_AS_ERRORS)
    target_compile_options(norn INTERFACE -Werror)
  endif()
endif()

include(CTest)
if(NORN_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
  )
  FetchContent_MakeAvailable(Catch2)
  add_executable(norn_tests tests/smoke_test.cpp)
  target_link_libraries(norn_tests PRIVATE norn Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(norn_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_mpmc_tests tests/mpmc_test.cpp)
  target_link_libraries(norn_mpmc_tests PRIVATE norn Catch2::Catch2WithMain)
  catch_discover_tests(norn_mpmc_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_stress_tests tests/stress_test.cpp)
  target_link_libraries(norn_stress_tests PRIVATE norn Catch2::Catch2WithMain)
  catch_discover_tests(norn_stress_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_hazard_tests tests/hazard_test.cpp)
  target_link_libraries(norn_hazard_tests PRIVATE norn Catch2::Catch2WithMain)
  catch_discover_tests(norn_hazard_tests PROPERTIES TIMEOUT 300)

  add_executable(norn_hardware_thread_control_tests tests/hardware_thread_control_test.cpp)
  target_include_directories(norn_hardware_thread_control_tests PRIVATE ${PROJECT_SOURCE_DIR}/benchmarks)
  target_link_libraries(norn_hardware_thread_control_tests PRIVATE norn Threads::Threads)
  add_test(NAME hardware_thread_control COMMAND norn_hardware_thread_control_tests)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME hardware_campaign_tool
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/hardware_campaign_tool_test.py)
  endif()
endif()

if(NORN_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_executable(norn_benchmarks benchmarks/mutex_queue_benchmark.cpp)
  target_link_libraries(norn_benchmarks PRIVATE norn benchmark::benchmark_main)
  add_executable(norn_mpmc_benchmarks benchmarks/mpmc_benchmark.cpp)
  target_link_libraries(norn_mpmc_benchmarks PRIVATE norn benchmark::benchmark_main)
  add_executable(norn_hardware_benchmarks benchmarks/hardware_campaign_benchmark.cpp)
  target_link_libraries(norn_hardware_benchmarks PRIVATE norn Threads::Threads)
endif()
