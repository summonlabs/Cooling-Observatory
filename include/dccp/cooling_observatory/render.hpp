// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_RENDER_HPP
#define DCCP_COOLING_OBSERVATORY_RENDER_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/identity.hpp"
#include "dccp/cooling_observatory/units.hpp"

namespace dccp::cooling_observatory {

/// Canonical value rendering, shared by every public surface.
///
/// The renderers exist so that the text surface, the JSON surface, the CLI and
/// the tests all spell a value the same way. A value that renders differently
/// in two places is a value a consumer will disagree with itself about.
[[nodiscard]] std::string render_maybe_quantity(const Maybe<Quantity>& value);
[[nodiscard]] std::string render_subject(const SubjectRef& subject);
[[nodiscard]] std::string render_authority(const AuthorityRef& authority);
[[nodiscard]] std::string render_evidence_ref(const EvidenceRef& reference);
[[nodiscard]] std::string render_freshness(Freshness freshness);

/// JSON helpers. Every string is escaped; every number is an integer.
[[nodiscard]] std::string json_escape(std::string_view text);
[[nodiscard]] std::string json_string(std::string_view text);
[[nodiscard]] std::string json_bool(bool value);
[[nodiscard]] std::string json_int(std::int64_t value);
[[nodiscard]] std::string json_u64(std::uint64_t value);
[[nodiscard]] std::string json_null();
[[nodiscard]] std::string json_maybe_quantity(const Maybe<Quantity>& value);
[[nodiscard]] std::string json_quantity(const Quantity& value);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_RENDER_HPP
