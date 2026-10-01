// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_harness.hpp"

#include <iostream>
#include <string>

#include "child_process.hpp"

namespace cotest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

std::size_t& failure_count() {
  static std::size_t count = 0;
  return count;
}

std::string& current_test() {
  static std::string name;
  return name;
}

std::vector<std::string>& failure_messages() {
  static std::vector<std::string> messages;
  return messages;
}

void record_failure(const std::string& message, const char* file, int line) {
  ++failure_count();
  std::ostringstream out;
  out << file << ':' << line << ": " << current_test() << ": " << message;
  failure_messages().push_back(out.str());
  std::cerr << "FAIL " << out.str() << '\n';
}

int run_all(const char* suite_name) {
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (TestCase& test : registry()) {
    current_test() = test.name;
    const std::size_t before = failure_count();
    try {
      test.body();
    } catch (const Failure& failure) {
      record_failure(std::string("aborted: ") + failure.what(), test.file.c_str(), test.line);
    } catch (const std::exception& error) {
      record_failure(std::string("unexpected exception: ") + error.what(), test.file.c_str(),
                     test.line);
    } catch (...) {
      record_failure("unexpected non-standard exception", test.file.c_str(), test.line);
    }
    if (failure_count() == before) {
      ++passed;
    } else {
      ++failed;
    }
  }
  std::cout << suite_name << ": " << passed << " passed, " << failed << " failed, "
            << registry().size() << " total\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace cotest

int main(int argc, char** argv) {
  // The multiprocess proofs start this same executable again with a --child-
  // argument. Dispatching here keeps the child program and the parent's
  // expectations in one place, so neither can drift from the other.
  int child_exit = 0;
  if (cotest::dispatch_child(argc, argv, child_exit)) {
    return child_exit;
  }
  return cotest::run_all("cooling_observatory_tests");
}
