# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Out-of-tree consumer of an INSTALLED cooling_observatory package.
#
# This project is deliberately not part of the library's own build. It proves
# that a third party can consume the exported package exactly as documented:
#
#   find_package(cooling_observatory 1.0 REQUIRED)
#   target_link_libraries(app PRIVATE dccp::cooling_observatory)
#
# Configure it with CMAKE_PREFIX_PATH pointing at the install prefix.

cmake_minimum_required(VERSION 3.20)

# A single-configuration generator has no default build type, and the installed
# package is a Release build. Choose Release before project() so that the
# consumer's runtime library and iterator debug level match the library it links
# against unless the caller says otherwise.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

project(cooling_observatory_downstream
        VERSION 1.0.0
        DESCRIPTION "Out-of-tree consumer of the installed cooling_observatory package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "downstream: requiring cooling_observatory 1.0 (search path: ${CMAKE_PREFIX_PATH})")

find_package(cooling_observatory 1.0 REQUIRED)

if(NOT cooling_observatory_VERSION)
  message(FATAL_ERROR "downstream: the found package reported no version")
endif()

message(STATUS "downstream: found cooling_observatory ${cooling_observatory_VERSION}")
message(STATUS "downstream: package directory ${cooling_observatory_DIR}")
message(STATUS "downstream: include directories ${COOLING_OBSERVATORY_INCLUDE_DIRS}")
message(STATUS "downstream: libraries ${COOLING_OBSERVATORY_LIBRARIES}")

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE dccp::cooling_observatory)
target_compile_features(downstream_consumer PRIVATE cxx_std_20)

# The version the package was found at, baked in so the program can prove at run
# time that the headers and the library it linked against are one release.
target_compile_definitions(downstream_consumer PRIVATE
  COOLING_OBSERVATORY_PACKAGE_VERSION="${cooling_observatory_VERSION}")

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /WX)
  target_compile_definitions(downstream_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "downstream: target downstream_consumer configured")
