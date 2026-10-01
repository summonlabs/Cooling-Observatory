# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# One executable per suite: a failure names the area it came from, and a suite
# that hangs (which this project treats as a defect, not a flake) does not hide
# the others behind it.

set(CO_TEST_SUPPORT test_main.cpp support.cpp reference_model.cpp child_process.cpp)

set(CO_TEST_SUITES
  unit_foundations
  unit_evidence
  unit_plant
  integration_engine
  reference_arithmetic
  adversarial_evidence
  adversarial_persistence
  concurrency_threads
  restart_recovery
  process_isolation
  property_determinism
)

foreach(suite IN LISTS CO_TEST_SUITES)
  add_executable(${suite} ${suite}.cpp ${CO_TEST_SUPPORT})
  target_link_libraries(${suite} PRIVATE dccp::cooling_observatory)
  target_include_directories(${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(${suite} PROPERTIES CXX_EXTENSIONS OFF)
  co_configure_target(${suite})
  if(WIN32)
    # The test sources use std::min and std::max. Without NOMINMAX the Windows
    # headers define them as macros and rewrite those calls.
    target_compile_definitions(${suite} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()

  # The multiprocess proofs need to start this suite again as a child. The
  # suite is told where to find itself rather than guessing, so a renamed or
  # relocated binary is a clear failure instead of a silent skip.
  target_compile_definitions(${suite} PRIVATE
    CO_TEST_EXECUTABLE="$<TARGET_FILE:${suite}>"
    CO_CCAOCTL_EXECUTABLE="$<TARGET_FILE:ccoctl>")

  add_test(NAME ${suite} COMMAND ${suite})
  set_tests_properties(${suite} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

if(COOLING_OBSERVATORY_BUILD_DOWNSTREAM_TESTS)
  add_test(NAME downstream_find_package
           COMMAND ${CMAKE_COMMAND}
             -DCO_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/downstream
             -DCO_PREFIX=${COOLING_OBSERVATORY_DOWNSTREAM_PREFIX}
             -P ${CMAKE_CURRENT_SOURCE_DIR}/downstream/run_downstream_proof.cmake)
endif()
