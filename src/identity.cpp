// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/identity.hpp"

#include <array>
#include <limits>

namespace dccp::cooling_observatory {
namespace {

constexpr bool is_alpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_start(char c) noexcept { return is_alpha(c) || c == '_'; }

/// The accepted body bytes. These are the bytes adjacent DCCP authorities
/// permit in an identity; nothing outside this set is accepted, so an identity
/// can never contain whitespace, a quote, a control character or a backslash.
constexpr bool is_body(char c) noexcept {
  return is_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == ':' || c == '-' || c == '/';
}

struct KindName {
  SubjectKind kind;
  std::string_view token;
};

constexpr std::array<KindName, 14> kSubjectKindNames{{
    {SubjectKind::Facility, "facility"},
    {SubjectKind::Plant, "plant"},
    {SubjectKind::Loop, "loop"},
    {SubjectKind::Pump, "pump"},
    {SubjectKind::Valve, "valve"},
    {SubjectKind::Chiller, "chiller"},
    {SubjectKind::Cdu, "cdu"},
    {SubjectKind::Crah, "crah"},
    {SubjectKind::Manifold, "manifold"},
    {SubjectKind::Zone, "zone"},
    {SubjectKind::Branch, "branch"},
    {SubjectKind::RackGroup, "rack_group"},
    {SubjectKind::Measurement, "measurement"},
    {SubjectKind::Sensor, "sensor"},
}};

}  // namespace

Result<StrongId> StrongId::parse(std::string_view text) {
  if (text.empty()) {
    return fail(Code::InvalidArgument, "empty_identity", "an identity must not be empty");
  }
  if (text.size() > kMaxIdentityLength) {
    return fail(Code::LimitExceeded, "identity_too_long",
                "identity of " + std::to_string(text.size()) + " bytes exceeds the limit of " +
                    std::to_string(kMaxIdentityLength));
  }
  if (!is_start(text.front())) {
    return fail(Code::InvalidArgument, "identity_bad_first_byte",
                "an identity must start with an ASCII letter or underscore");
  }
  for (const char c : text) {
    if (!is_body(c)) {
      return fail(Code::InvalidArgument, "identity_bad_byte",
                  std::string("identity contains a byte outside the accepted set: '") + c + "'");
    }
  }
  StrongId out;
  out.text_.assign(text);
  return out;
}

bool StrongId::valid(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxIdentityLength) {
    return false;
  }
  if (!is_start(text.front())) {
    return false;
  }
  for (const char c : text) {
    if (!is_body(c)) {
      return false;
    }
  }
  return true;
}

std::string_view to_token(SubjectKind kind) noexcept {
  for (const KindName& entry : kSubjectKindNames) {
    if (entry.kind == kind) {
      return entry.token;
    }
  }
  return "unknown";
}

std::optional<SubjectKind> parse_subject_kind(std::string_view token) noexcept {
  for (const KindName& entry : kSubjectKindNames) {
    if (entry.token == token) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

Result<Revision> Revision::next() const {
  if (value_ == std::numeric_limits<std::uint64_t>::max()) {
    return fail(Code::LimitExceeded, "revision_exhausted",
                "the durable revision counter cannot advance past its maximum");
  }
  return Revision(value_ + 1);
}

}  // namespace dccp::cooling_observatory
