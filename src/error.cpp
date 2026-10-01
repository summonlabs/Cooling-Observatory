// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/error.hpp"

#include <array>
#include <utility>

namespace dccp::cooling_observatory {
namespace {

struct CodeName {
  Code code;
  std::string_view token;
};

/// The published spelling of every outcome code. Ordering is irrelevant; the
/// table is scanned linearly because it is small and never hot.
constexpr std::array<CodeName, 32> kCodeNames{{
    {Code::Ok, "ok"},
    {Code::InvalidArgument, "invalid_argument"},
    {Code::MalformedInput, "malformed_input"},
    {Code::UnknownToken, "unknown_token"},
    {Code::UnknownSubjectKind, "unknown_subject_kind"},
    {Code::LimitExceeded, "limit_exceeded"},
    {Code::NotImplemented, "not_implemented"},
    {Code::MissingEvidence, "missing_evidence"},
    {Code::StaleEvidence, "stale_evidence"},
    {Code::ConflictingEvidence, "conflicting_evidence"},
    {Code::RecoveredEvidenceNotCurrent, "recovered_evidence_not_current"},
    {Code::AuthorityMismatch, "authority_mismatch"},
    {Code::UnknownEvidence, "unknown_evidence"},
    {Code::UnsupportedValue, "unsupported_value"},
    {Code::StaleEpoch, "stale_epoch"},
    {Code::StaleGeneration, "stale_generation"},
    {Code::UnknownGeneration, "unknown_generation"},
    {Code::RevisionRegression, "revision_regression"},
    {Code::DuplicateRecord, "duplicate_record"},
    {Code::LifecycleRefused, "lifecycle_refused"},
    {Code::NotFound, "not_found"},
    {Code::CorruptSnapshot, "corrupt_snapshot"},
    {Code::IncompatibleSchema, "incompatible_schema"},
    {Code::RecoveredFromTornTail, "recovered_from_torn_tail"},
    {Code::StoreBusy, "store_busy"},
    {Code::StoreClosed, "store_closed"},
    {Code::IoFailure, "io_failure"},
    {Code::LockFailure, "lock_failure"},
    {Code::InternalError, "internal_error"},
}};

}  // namespace

std::string_view to_token(Code code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code && !entry.token.empty()) {
      return entry.token;
    }
  }
  return "internal_error";
}

std::optional<Code> parse_code(std::string_view token) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (!entry.token.empty() && entry.token == token) {
      return entry.code;
    }
  }
  return std::nullopt;
}

// Every Result instantiation is implicit: the class template is header-only, so
// no explicit instantiation list has to be kept in step with its users.


}  // namespace dccp::cooling_observatory