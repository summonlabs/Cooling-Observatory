# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Each example is a standalone program that links the library exactly as an
# out-of-tree consumer does.

set(COOLING_OBSERVATORY_EXAMPLES
  delivery_and_reserve
  silence_and_stale
  failures_with_residual_delivery
  divergence_and_contradiction
  persistence_and_restart
)

foreach(example IN LISTS COOLING_OBSERVATORY_EXAMPLES)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE dccp::cooling_observatory)
  set_target_properties(${example} PROPERTIES CXX_EXTENSIONS OFF)
  co_configure_target(${example})
endforeach()
