# External-consumer test for the installed Norn package.
#
# Installs the current build into a private prefix under the Norn build tree,
# configures tests/package_consumer as a separate project that can only find
# Norn through CMAKE_PREFIX_PATH, builds it, and runs the consumer binaries.
#
# Driven by the `package_consumer` CTest entry; every input is passed with -D.
# Inputs:
#   norn_source_dir / norn_build_dir : the Norn checkout and current build tree
#   cmake_command                    : CMake executable to drive the steps
#   generator                        : generator for the consumer build
#   cxx_compiler                     : compiler for the consumer build
#   build_config                     : configuration; may be empty for a
#                                      single-config build with no build type

if(NOT DEFINED norn_source_dir OR NOT DEFINED norn_build_dir)
  message(FATAL_ERROR "norn_source_dir and norn_build_dir must be defined")
endif()
if(NOT DEFINED cmake_command)
  set(cmake_command "${CMAKE_COMMAND}")
endif()

set(consumer_source_dir "${norn_source_dir}/tests/package_consumer")
set(consumer_prefix_dir "${norn_build_dir}/package_consumer/prefix")
set(consumer_build_dir "${norn_build_dir}/package_consumer/build")

file(REMOVE_RECURSE "${consumer_prefix_dir}" "${consumer_build_dir}")

macro(check_result step)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "package_consumer: ${step} failed (${result}):\n${output}")
  endif()
endmacro()

# 1. Install this Norn build into the private prefix.
set(install_args --install "${norn_build_dir}" --prefix "${consumer_prefix_dir}")
if(build_config)
  list(APPEND install_args --config "${build_config}")
endif()
execute_process(
  COMMAND "${cmake_command}" ${install_args}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE output)
check_result("install")

# 2. Configure the consumer project against only that prefix.
set(consumer_multi_config FALSE)
if(generator MATCHES "Visual Studio|Xcode|Ninja Multi-Config")
  set(consumer_multi_config TRUE)
endif()
set(configure_args -S "${consumer_source_dir}" -B "${consumer_build_dir}" -G "${generator}")
if(build_config AND NOT consumer_multi_config)
  list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${build_config}")
endif()
list(APPEND configure_args "-DCMAKE_PREFIX_PATH=${consumer_prefix_dir}")
if(cxx_compiler)
  list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${cxx_compiler}")
endif()
execute_process(
  COMMAND "${cmake_command}" ${configure_args}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE output)
check_result("consumer configure")

# 3. Build the consumers.
set(build_args --build "${consumer_build_dir}")
if(build_config)
  list(APPEND build_args --config "${build_config}")
endif()
execute_process(
  COMMAND "${cmake_command}" ${build_args}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE output)
check_result("consumer build")

function(find_consumer_binary out_var name)
  set(candidate "${consumer_build_dir}/${name}")
  if(build_config AND EXISTS "${consumer_build_dir}/${build_config}/${name}")
    set(candidate "${consumer_build_dir}/${build_config}/${name}")
  endif()
  if(NOT EXISTS "${candidate}")
    message(FATAL_ERROR "package_consumer: binary not found: ${candidate}")
  endif()
  set(${out_var} "${candidate}" PARENT_SCOPE)
endfunction()

find_consumer_binary(core_binary core_consumer)
find_consumer_binary(queue_binary norn_consumer)

execute_process(
  COMMAND "${core_binary}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE core_output ERROR_VARIABLE core_output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "package_consumer: core_consumer failed (${result}):\n${core_output}")
endif()

execute_process(
  COMMAND "${queue_binary}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE queue_output ERROR_VARIABLE queue_output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "package_consumer: norn_consumer failed (${result}):\n${queue_output}")
endif()

message(STATUS "package_consumer: install, configure, build, run all passed")
