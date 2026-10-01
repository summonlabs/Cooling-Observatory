// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_ERROR_HPP
#define DCCP_COOLING_OBSERVATORY_ERROR_HPP

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace dccp::cooling_observatory {

/// Stable, machine-readable outcome codes.
///
/// The codes are part of the public contract: a caller may branch on them, and
/// they are rendered verbatim in text and JSON output. New codes may be added;
/// existing codes never change meaning.
enum class Code : std::uint16_t {
  Ok = 0,

  // Input and syntax
  InvalidArgument = 10,
  MalformedInput = 11,
  UnknownToken = 12,
  UnknownSubjectKind = 13,
  LimitExceeded = 14,
  NotImplemented = 15,

  // Evidence and authority
  MissingEvidence = 20,
  StaleEvidence = 21,
  ConflictingEvidence = 22,
  RecoveredEvidenceNotCurrent = 23,
  AuthorityMismatch = 24,
  UnknownEvidence = 25,
  UnsupportedValue = 26,

  // Generations, epochs and lifecycle
  StaleEpoch = 30,
  StaleGeneration = 31,
  UnknownGeneration = 32,
  RevisionRegression = 33,
  DuplicateRecord = 34,
  LifecycleRefused = 35,

  // Persistence
  NotFound = 40,
  CorruptSnapshot = 41,
  IncompatibleSchema = 42,
  RecoveredFromTornTail = 43,
  StoreBusy = 44,
  StoreClosed = 45,
  IoFailure = 46,
  LockFailure = 47,

  // Internal
  InternalError = 90,
};

/// Human-facing rendering of a code. Stable, lower snake case, suitable for
/// both text and JSON.
[[nodiscard]] std::string_view to_token(Code code) noexcept;

/// Parse a code token produced by to_token(). Unknown tokens yield nothing.
[[nodiscard]] std::optional<Code> parse_code(std::string_view token) noexcept;

/// True when the code denotes a successful outcome.
[[nodiscard]] constexpr bool is_ok(Code code) noexcept { return code == Code::Ok; }

/// True when the code denotes that a requested answer is not fully evidenced.
/// These are answers, not failures: the engine reports them without pretending
/// to know more than the evidence supports.
[[nodiscard]] constexpr bool is_indeterminate(Code code) noexcept {
  switch (code) {
    case Code::MissingEvidence:
    case Code::StaleEvidence:
    case Code::ConflictingEvidence:
    case Code::RecoveredEvidenceNotCurrent:
    case Code::UnknownEvidence:
    case Code::UnsupportedValue:
    case Code::UnknownGeneration:
      return true;
    default:
      return false;
  }
}

/// A failure or an indeterminate outcome, with the data needed to explain it.
struct Error {
  Code code = Code::Ok;
  /// Short, stable, lower snake case reason token.
  std::string reason;
  /// Free-form detail. Never machine-parsed.
  std::string detail;

  Error() = default;
  Error(Code c, std::string r, std::string d = {})
      : code(c), reason(std::move(r)), detail(std::move(d)) {}

  [[nodiscard]] bool ok() const noexcept { return code == Code::Ok; }
};

/// Outcome of an operation that produces no value.
class Status {
 public:
  Status() = default;

  static Status success() { return Status(); }

  static Status failure(Code code, std::string reason, std::string detail = {}) {
    return Status(Error(code, std::move(reason), std::move(detail)));
  }

  /// Build a failure from an Error that a helper already produced, so that a
  /// helper can report a typed outcome without the caller re-splitting it.
  static Status failure(Error error) { return Status(std::move(error)); }

  [[nodiscard]] bool ok() const noexcept { return error_.code == Code::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] Code code() const noexcept { return error_.code; }
  [[nodiscard]] const std::string& reason() const noexcept { return error_.reason; }
  [[nodiscard]] const std::string& detail() const noexcept { return error_.detail; }

 private:
  explicit Status(Error e) : error_(std::move(e)) {}
  Error error_{};
};

/// Outcome of an operation that produces a value: either the value or an Error.
template <typename T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const noexcept { return std::get<Error>(storage_); }
  [[nodiscard]] Code code() const noexcept {
    return ok() ? Code::Ok : std::get<Error>(storage_).code;
  }
  [[nodiscard]] const std::string& reason() const noexcept {
    static const std::string empty;
    return ok() ? empty : std::get<Error>(storage_).reason;
  }
  [[nodiscard]] const std::string& detail() const noexcept {
    static const std::string empty;
    return ok() ? empty : std::get<Error>(storage_).detail;
  }

  /// Precondition: ok(). The check is a hard failure in debug builds because
  /// every call site is expected to have branched on ok() first.
  [[nodiscard]] T& value() & {
    if (!ok()) {
      fail_fast_on_empty_value();
    }
    return std::get<T>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    if (!ok()) {
      fail_fast_on_empty_value();
    }
    return std::get<T>(storage_);
  }
  [[nodiscard]] T&& value() && {
    if (!ok()) {
      fail_fast_on_empty_value();
    }
    return std::get<T>(std::move(storage_));
  }

  /// Value or a caller-provided fallback.
  template <typename U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return ok() ? std::get<T>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  /// Declared and defined here, inside the template, so that every
  /// instantiation in every translation unit has it. A definition in one
  /// translation unit would leave the others with an unresolved symbol, which
  /// is a link error rather than the runtime check this library intends.
  [[noreturn]] static void fail_fast_on_empty_value() { std::terminate(); }

  std::variant<T, Error> storage_;
};

/// Convenience constructors so call sites read as intent, not as plumbing.
template <typename T>
[[nodiscard]] Result<std::decay_t<T>> ok(T&& value) {
  return Result<std::decay_t<T>>(std::forward<T>(value));
}

template <typename T = void>
[[nodiscard]] Error fail(Code code, std::string reason, std::string detail = {}) {
  return Error(code, std::move(reason), std::move(detail));
}

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_ERROR_HPP
