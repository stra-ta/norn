cmake_minimum_required(VERSION 3.25)

project(norn VERSION 0.2.0 LANGUAGES CXX)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# Tests and benchmarks are on by default only when Norn is the top-level
# project; subprojects that add_subdirectory Norn opt in explicitly.
if(PROJECT_IS_TOP_LEVEL)
  set(NORN_BUILD_TESTS_DEFAULT ON)
  set(NORN_BUILD_BENCHMARKS_DEFAULT ON)
else()
  set(NORN_BUILD_TESTS_DEFAULT OFF)
  set(NORN_BUILD_BENCHMARKS_DEFAULT OFF)
endif()

option(NORN_BUILD_TESTS "Build Norn tests" ${NORN_BUILD_TESTS_DEFAULT})
option(NORN_BUILD_BENCHMARKS "Build Norn benchmarks" ${NORN_BUILD_BENCHMARKS_DEFAULT})
option(NORN_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Canonical header-only core.
add_library(norn_core INTERFACE)
target_compile_features(norn_core INTERFACE cxx_std_20)
target_include_directories(
  norn_core
  INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(norn_core PROPERTIES EXPORT_NAME core)
add_library(norn::core ALIAS norn_core)
# The core tier uses std::thread facilities (backoff yields); propagate the
# platform threading flags to every consumer, including via norn::norn.
target_link_libraries(norn_core INTERFACE Threads::Threads)

# Full Norn library target; links the core dependency tier.
add_library(norn INTERFACE)
target_link_libraries(norn INTERFACE norn_core)
set_target_properties(norn PROPERTIES EXPORT_NAME norn)
add_library(norn::norn ALIAS norn)

# Project-only warning flags. Never installed or exported: consumers of the
# installed package must not inherit Norn's warning settings.
add_library(norn_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(norn_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(NORN_WARNINGS_AS_ERRORS)
    target_compile_options(norn_warnings INTERFACE -Werror)
  endif()
endif()

# Installable, relocatable package.
include(CMakePackageConfigHelpers)

install(TARGETS norn_core norn EXPORT nornTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT nornTargets
  FILE nornTargets.cmake
  NAMESPACE norn::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/norn)

configure_package_config_file(
  cmake/nornConfig.cmake.in ${PROJECT_BINARY_DIR}/nornConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/norn)
write_basic_package_version_file(
  ${PROJECT_BINARY_DIR}/nornConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(
  FILES ${PROJECT_BINARY_DIR}/nornConfig.cmake ${PROJECT_BINARY_DIR}/nornConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/norn)

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
  add_executable(norn_foundation_tests tests/core_test.cpp tests/queue_api_test.cpp)
  target_link_libraries(norn_foundation_tests PRIVATE norn norn_warnings Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(norn_foundation_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_tests tests/smoke_test.cpp)
  target_link_libraries(norn_tests PRIVATE norn norn_warnings Catch2::Catch2WithMain)
  catch_discover_tests(norn_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_mpmc_tests tests/mpmc_test.cpp)
  target_link_libraries(norn_mpmc_tests PRIVATE norn norn_warnings Catch2::Catch2WithMain)
  catch_discover_tests(norn_mpmc_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_stress_tests tests/stress_test.cpp)
  target_link_libraries(norn_stress_tests PRIVATE norn norn_warnings Catch2::Catch2WithMain)
  catch_discover_tests(norn_stress_tests PROPERTIES TIMEOUT 300)
  add_executable(norn_hazard_tests tests/hazard_test.cpp)
  target_link_libraries(norn_hazard_tests PRIVATE norn norn_warnings Catch2::Catch2WithMain)
  catch_discover_tests(norn_hazard_tests PROPERTIES TIMEOUT 300)

  add_executable(norn_hardware_thread_control_tests tests/hardware_thread_control_test.cpp)
  target_include_directories(norn_hardware_thread_control_tests PRIVATE ${PROJECT_SOURCE_DIR}/benchmarks)
  target_link_libraries(norn_hardware_thread_control_tests PRIVATE norn norn_warnings Threads::Threads)
  add_test(NAME hardware_thread_control COMMAND norn_hardware_thread_control_tests)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME hardware_campaign_tool
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/hardware_campaign_tool_test.py)
    # Output-contract test for the hardware benchmark binary JSON. Verifies
    # completeness and worker statistics; skipped when benchmarks are not built.
    add_test(
      NAME hardware_binary_json_contract
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/hardware_campaign_tool_test.py --contract)
    add_test(
      NAME memory_order_source_contract
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_memory_order.py --self-test)
    add_test(
      NAME committed_evidence_contract
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_evidence.py)
  endif()

  # External-consumer check: install this build into a private prefix, then
  # configure and build tests/package_consumer with only CMAKE_PREFIX_PATH.
  add_test(
    NAME package_consumer
    COMMAND
      ${CMAKE_COMMAND}
      -Dnorn_source_dir=${PROJECT_SOURCE_DIR}
      -Dnorn_build_dir=${PROJECT_BINARY_DIR}
      -Dcmake_command=${CMAKE_COMMAND}
      -Dgenerator=${CMAKE_GENERATOR}
      -Dcxx_compiler=${CMAKE_CXX_COMPILER}
      -Dbuild_config=$<CONFIG>
      -P ${PROJECT_SOURCE_DIR}/tests/package_consumer_test.cmake)
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
  target_link_libraries(norn_benchmarks PRIVATE norn norn_warnings benchmark::benchmark_main)
  add_executable(norn_mpmc_benchmarks benchmarks/mpmc_benchmark.cpp)
  target_link_libraries(norn_mpmc_benchmarks PRIVATE norn norn_warnings benchmark::benchmark_main)
  add_executable(norn_hardware_benchmarks benchmarks/hardware_campaign_benchmark.cpp)
  target_link_libraries(norn_hardware_benchmarks PRIVATE norn norn_warnings Threads::Threads)
endif()
