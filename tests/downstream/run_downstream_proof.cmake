# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Configure, build and run the out-of-tree consumer against an installed prefix.
#
# Invoked by CTest, and usable directly:
#
#   cmake -DCO_WORK_DIR=<scratch> -DCO_PREFIX=<install prefix> -P run_downstream_proof.cmake
#
# Every step is a real process: a fresh configure reading only the installed
# package, a build linking only the installed library, and the resulting program
# run against a state directory of its own.

foreach(required CO_WORK_DIR CO_PREFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be set")
  endif()
endforeach()

set(consumer_source "${CMAKE_CURRENT_LIST_DIR}")
set(work_dir "${CO_WORK_DIR}")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")

set(generator_args "")
if(CO_GENERATOR)
  list(APPEND generator_args -G "${CO_GENERATOR}")
endif()
if(CO_GENERATOR_PLATFORM)
  list(APPEND generator_args -A "${CO_GENERATOR_PLATFORM}")
endif()

message(STATUS "downstream proof: configuring against ${CO_PREFIX}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${consumer_source}"
          -B "${work_dir}/build"
          ${generator_args}
          -DCMAKE_BUILD_TYPE=Release
          "-DCMAKE_PREFIX_PATH=${CO_PREFIX}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: configure failed\n${configure_output}\n${configure_error}")
endif()
message(STATUS "downstream proof: configure succeeded")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${work_dir}/build" --config Release
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: build failed\n${build_output}\n${build_error}")
endif()
message(STATUS "downstream proof: build succeeded")

# The executable may be at the top of the build tree (single configuration) or
# under a per-configuration directory (multi-configuration generators).
set(candidates
    "${work_dir}/build/downstream_consumer"
    "${work_dir}/build/downstream_consumer.exe"
    "${work_dir}/build/Release/downstream_consumer.exe"
    "${work_dir}/build/Release/downstream_consumer")
set(consumer "")
foreach(candidate IN LISTS candidates)
  if(EXISTS "${candidate}")
    set(consumer "${candidate}")
    break()
  endif()
endforeach()
if(consumer STREQUAL "")
  message(FATAL_ERROR "downstream proof: the consumer executable was not produced")
endif()

file(REMOVE_RECURSE "${work_dir}/state")
execute_process(
  COMMAND "${consumer}" "${work_dir}/state"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: the consumer failed\n${run_output}\n${run_error}")
endif()

message(STATUS "downstream proof: ${run_output}")
file(REMOVE_RECURSE "${work_dir}")
message(STATUS "downstream proof: passed")
