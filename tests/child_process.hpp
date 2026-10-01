// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A real multiprocess probe. The tests that need one start this suite again
// with an argument that selects a small program, and read the child's stdout
// and exit code. Nothing is simulated: the child is a separate process with its
// own address space, its own file handles and its own share of the kernel.

#ifndef COOLING_OBSERVATORY_TEST_CHILD_PROCESS_HPP
#define COOLING_OBSERVATORY_TEST_CHILD_PROCESS_HPP

#include <string>
#include <utility>
#include <vector>

namespace cotest {

struct ChildResult {
  bool started = false;
  int exit_code = -1;
  std::string output{};
  std::string error{};
  std::string failure{};  ///< why the child could not be started, when it could not
};

/// The path of this suite's own executable, or an empty string when the build
/// did not provide it. A test that needs a child fails loudly rather than
/// silently skipping when it is empty.
[[nodiscard]] std::string self_executable();

/// Run this suite again with the given arguments and wait for it to finish.
[[nodiscard]] ChildResult run_self(const std::vector<std::string>& arguments);

/// Run an arbitrary executable with an argument vector and wait for it to
/// finish, capturing stdout and stderr.
[[nodiscard]] ChildResult run_program(const std::string& executable,
                                      const std::vector<std::string>& arguments);

/// Run this suite again as a holder of the store's writer lock. The child opens
/// the engine, reports readiness on stdout, and blocks until its stdin is
/// closed or a line arrives.
[[nodiscard]] ChildResult run_lock_holder(const std::string& state_directory);

/// The path of the inspection tool, provided by the build.
[[nodiscard]] std::string ccoctl_executable();

/// True when this process was started as a child helper, with the helper's name
/// available afterwards. The suite calls this before running its own tests.
[[nodiscard]] bool dispatch_child(int argc, char** argv, int& exit_code);

/// The value following a named argument, or an empty string.
[[nodiscard]] std::string argument_value(int argc, char** argv, const char* name);

}  // namespace cotest

#endif  // COOLING_OBSERVATORY_TEST_CHILD_PROCESS_HPP
