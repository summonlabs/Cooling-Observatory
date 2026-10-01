# Cooling Observatory - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# The primary product surface is the library target cooling_observatory,
# exported as dccp::cooling_observatory. The inspection CLI, the examples, the
# tests and the benchmarks are optional. Only the library, its public headers
# and the CLI are installed.

cmake_minimum_required(VERSION 3.20)

# Keep in sync with include/dccp/cooling_observatory/version.hpp; the test suite
# compares this value against the compiled-in library version.
project(cooling_observatory
        VERSION 1.0.0
        DESCRIPTION "Cooling delivery observation, constraint localisation and divergence analysis for the Data Center Control Plane"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(COOLING_OBSERVATORY_BUILD_TESTS "Build the Cooling Observatory test suite" ON)
option(COOLING_OBSERVATORY_BUILD_TOOLS "Build the ccoctl inspection tool" ON)
option(COOLING_OBSERVATORY_BUILD_EXAMPLES "Build the Cooling Observatory examples" ON)
option(COOLING_OBSERVATORY_BUILD_BENCHMARKS "Build the Cooling Observatory benchmarks" ON)
option(COOLING_OBSERVATORY_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(COOLING_OBSERVATORY_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(COOLING_OBSERVATORY_ENABLE_ANALYZE "Run MSVC static analysis on first-party targets" OFF)
option(COOLING_OBSERVATORY_BUILD_DOWNSTREAM_TESTS
       "Add the out-of-tree find_package consumer as a CTest case" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COOLING_OBSERVATORY_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/cooling_observatory"
    CACHE STRING "Install destination of the cooling_observatory CMake package files")

# ---------------------------------------------------------------------------
# Build policy
# ---------------------------------------------------------------------------

add_library(cooling_observatory_warnings INTERFACE)
add_library(dccp::cooling_observatory_warnings ALIAS cooling_observatory_warnings)

if(MSVC)
  target_compile_options(cooling_observatory_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  if(COOLING_OBSERVATORY_WARNINGS_AS_ERRORS)
    target_compile_options(cooling_observatory_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(cooling_observatory_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual
    -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(COOLING_OBSERVATORY_WARNINGS_AS_ERRORS)
    target_compile_options(cooling_observatory_warnings INTERFACE -Werror)
  endif()
endif()

add_library(cooling_observatory_sanitizers INTERFACE)
if(COOLING_OBSERVATORY_ENABLE_ASAN)
  if(MSVC)
    # /Zi travels with /fsanitize=address: without debug information the
    # compiler emits C5072 on every translation unit and an ASan report cannot
    # name the line it came from. /fsanitize=address is a compiler-driver
    # option; handing it to link.exe only produces LNK4044, while /DEBUG is what
    # the linker needs for a symbolic report.
    target_compile_options(cooling_observatory_sanitizers INTERFACE /fsanitize=address /Zi)
    target_link_options(cooling_observatory_sanitizers INTERFACE /DEBUG)
  else()
    target_compile_options(cooling_observatory_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(cooling_observatory_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  endif()
endif()

add_library(cooling_observatory_analyze INTERFACE)
if(COOLING_OBSERVATORY_ENABLE_ANALYZE AND MSVC)
  # The analyzer needs /W3 or higher; /W4 /WX already holds first-party code to
  # zero warnings, so analysis results are additive rather than a replacement.
  target_compile_options(cooling_observatory_analyze INTERFACE /analyze)
endif()

# Applies the project build policy to one first-party target. Every entry is
# PRIVATE so nothing used for this project's own build can leak into the
# installed package.
function(co_configure_target target)
  target_link_libraries(${target} PRIVATE cooling_observatory_warnings)
  if(COOLING_OBSERVATORY_ENABLE_ASAN)
    target_link_libraries(${target} PRIVATE cooling_observatory_sanitizers)
  endif()
  if(COOLING_OBSERVATORY_ENABLE_ANALYZE AND MSVC)
    target_link_libraries(${target} PRIVATE cooling_observatory_analyze)
  endif()
  if(MSVC)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(cooling_observatory
  src/version.cpp
  src/error.cpp
  src/checked.cpp
  src/units.cpp
  src/identity.cpp
  src/checksum.cpp
  src/clock.cpp
  src/limits.cpp
  src/semantics.cpp
  src/evidence.cpp
  src/plant.cpp
  src/delivery.cpp
  src/dependency.cpp
  src/analysis.cpp
  src/delivery_analysis.cpp
  src/constraint_analysis.cpp
  src/failure_analysis.cpp
  src/reserve_analysis.cpp
  src/divergence_analysis.cpp
  src/coverage_analysis.cpp
  src/render.cpp
  src/report_render.cpp
  src/ingest.cpp
  src/textproto.cpp
  src/file_ops.cpp
  src/persistence.cpp
  src/stats.cpp
  src/engine.cpp
)
add_library(dccp::cooling_observatory ALIAS cooling_observatory)

target_include_directories(cooling_observatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(cooling_observatory PUBLIC cxx_std_20)
set_target_properties(cooling_observatory PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

co_configure_target(cooling_observatory)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

if(COOLING_OBSERVATORY_BUILD_TOOLS)
  add_executable(ccoctl tools/ccoctl/main.cpp)
  target_link_libraries(ccoctl PRIVATE dccp::cooling_observatory)
  set_target_properties(ccoctl PROPERTIES CXX_EXTENSIONS OFF)
  co_configure_target(ccoctl)
endif()

# ---------------------------------------------------------------------------
# Examples, benchmarks and tests
# ---------------------------------------------------------------------------

if(COOLING_OBSERVATORY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(COOLING_OBSERVATORY_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(COOLING_OBSERVATORY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

# The build-policy interface targets travel with the export because the library
# links them PRIVATE and CMake requires every target of an exported dependency
# chain to belong to an export set. They carry only PRIVATE compile and link
# options, so nothing on them reaches a consumer's compile line: an installed
# consumer compiles with its own flags and links against this library alone.
install(TARGETS cooling_observatory
              cooling_observatory_warnings
              cooling_observatory_sanitizers
              cooling_observatory_analyze
        EXPORT cooling_observatoryTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(COOLING_OBSERVATORY_BUILD_TOOLS)
  install(TARGETS ccoctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT cooling_observatoryTargets
        NAMESPACE dccp::
        FILE cooling_observatoryTargets.cmake
        DESTINATION ${COOLING_OBSERVATORY_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cooling_observatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/cooling_observatoryConfig.cmake
  INSTALL_DESTINATION ${COOLING_OBSERVATORY_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/cooling_observatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cooling_observatoryConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cooling_observatoryConfigVersion.cmake
        DESTINATION ${COOLING_OBSERVATORY_INSTALL_CMAKEDIR})

install(FILES README.md LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/cooling_observatory
        OPTIONAL)
