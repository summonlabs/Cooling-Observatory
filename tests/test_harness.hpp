// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef COOLING_OBSERVATORY_TEST_HARNESS_HPP
#define COOLING_OBSERVATORY_TEST_HARNESS_HPP

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/error.hpp"

namespace cotest {

/// A test failure. Thrown by an assertion and caught by the runner, so one
/// failing expectation reports and the suite continues.
class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string name;
  std::string file;
  int line = 0;
  std::function<void()> body;
};

[[nodiscard]] std::vector<TestCase>& registry();
[[nodiscard]] std::size_t& failure_count();
[[nodiscard]] std::string& current_test();
[[nodiscard]] std::vector<std::string>& failure_messages();

/// Register a test at static-initialisation time.
struct Registrar {
  Registrar(const char* name, const char* file, int line, std::function<void()> body) {
    registry().push_back(TestCase{name, file, line, std::move(body)});
  }
};

/// Run every registered test. Returns the number of failures.
int run_all(const char* suite_name);

void record_failure(const std::string& message, const char* file, int line);

template <typename T>
std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string describe(bool value) { return value ? "true" : "false"; }

inline std::string describe(const dccp::cooling_observatory::Error& error) {
  return std::string(dccp::cooling_observatory::to_token(error.code)) + "/" + error.reason + ": " +
         error.detail;
}

template <>
inline std::string describe<std::string>(const std::string& value) { return value; }

inline std::string describe(const char* value) { return value == nullptr ? "(null)" : value; }

}  // namespace cotest

#define CO_TEST_CONCAT_INNER(a, b) a##b
#define CO_TEST_CONCAT(a, b) CO_TEST_CONCAT_INNER(a, b)

#define CO_TEST(name)                                                                        \
  static void CO_TEST_CONCAT(co_test_body_, __LINE__)();                                     \
  static const ::cotest::Registrar CO_TEST_CONCAT(co_test_registrar_, __LINE__)(             \
      name, __FILE__, __LINE__, &CO_TEST_CONCAT(co_test_body_, __LINE__));                   \
  static void CO_TEST_CONCAT(co_test_body_, __LINE__)()

#define CO_CHECK(condition)                                                                  \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      ::cotest::record_failure("expected: " #condition, __FILE__, __LINE__);                 \
    }                                                                                        \
  } while (false)

#define CO_CHECK_EQ(actual, expected)                                                        \
  do {                                                                                       \
    const auto& co_actual = (actual);                                                        \
    const auto& co_expected = (expected);                                                    \
    if (!(co_actual == co_expected)) {                                                       \
      ::cotest::record_failure(std::string("expected ") + #actual + " == " + #expected +     \
                                   " but got " + ::cotest::describe(co_actual) + " vs " +    \
                                   ::cotest::describe(co_expected),                          \
                               __FILE__, __LINE__);                                          \
    }                                                                                        \
  } while (false)

#define CO_CHECK_NE(actual, expected)                                                        \
  do {                                                                                       \
    const auto& co_actual = (actual);                                                        \
    const auto& co_expected = (expected);                                                    \
    if (co_actual == co_expected) {                                                          \
      ::cotest::record_failure(std::string("expected ") + #actual + " != " + #expected,      \
                               __FILE__, __LINE__);                                          \
    }                                                                                        \
  } while (false)

#define CO_REQUIRE_EQ(actual, expected)                                                      \
  do {                                                                                       \
    const auto& co_actual = (actual);                                                        \
    const auto& co_expected = (expected);                                                    \
    if (!(co_actual == co_expected)) {                                                       \
      ::cotest::record_failure(std::string("required ") + #actual + " == " + #expected +     \
                                   " but got " + ::cotest::describe(co_actual) + " vs " +    \
                                   ::cotest::describe(co_expected),                          \
                               __FILE__, __LINE__);                                          \
      throw ::cotest::Failure("required equality failed: " #actual);                          \
    }                                                                                        \
  } while (false)

#define CO_REQUIRE(condition)                                                                \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      ::cotest::record_failure("required: " #condition, __FILE__, __LINE__);                 \
      throw ::cotest::Failure("required condition failed: " #condition);                     \
    }                                                                                        \
  } while (false)

#define CO_REQUIRE_OK(result)                                                                \
  do {                                                                                       \
    if (!(result)) {                                                                         \
      ::cotest::record_failure(std::string("expected success from " #result " but got ") +   \
                                   ::cotest::describe((result).error()),                     \
                               __FILE__, __LINE__);                                          \
      throw ::cotest::Failure("required success failed: " #result);                          \
    }                                                                                        \
  } while (false)

#define CO_CHECK_OK(result)                                                                  \
  do {                                                                                       \
    if (!(result)) {                                                                         \
      ::cotest::record_failure(std::string("expected success from " #result " but got ") +   \
                                   ::cotest::describe((result).error()),                     \
                               __FILE__, __LINE__);                                          \
    }                                                                                        \
  } while (false)

#define CO_CHECK_ERR(result, expected_code)                                                  \
  do {                                                                                       \
    if ((result)) {                                                                          \
      ::cotest::record_failure(std::string("expected failure from " #result),                \
                               __FILE__, __LINE__);                                          \
    } else if ((result).code() != (expected_code)) {                                         \
      ::cotest::record_failure(std::string("expected code ") +                               \
                                   ::cotest::describe(static_cast<int>(expected_code)) +     \
                                   " from " #result " but got " +                            \
                                   ::cotest::describe((result).error()),                     \
                               __FILE__, __LINE__);                                          \
    }                                                                                        \
  } while (false)

#endif  // COOLING_OBSERVATORY_TEST_HARNESS_HPP
