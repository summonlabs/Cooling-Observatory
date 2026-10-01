# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# The benchmark measures completed work: a fixed number of records ingested and
# a fixed number of queries answered, with the elapsed time of each phase
# reported separately. It prints no throughput figure it did not measure.

add_executable(observatory_bench observatory_bench.cpp)
target_link_libraries(observatory_bench PRIVATE dccp::cooling_observatory)
set_target_properties(observatory_bench PROPERTIES CXX_EXTENSIONS OFF)
co_configure_target(observatory_bench)
