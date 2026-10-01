// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_TEXTPROTO_HPP
#define DCCP_COOLING_OBSERVATORY_TEXTPROTO_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/ingest.hpp"
#include "dccp/cooling_observatory/query.hpp"

namespace dccp::cooling_observatory {

/// The interchange encoding: one statement per line, a key and a value
/// separated by whitespace, with blank lines and lines whose first
/// non-blank character is '#' ignored.
///
/// A line-oriented text encoding is the format the source of truth for this
/// runtime's input, because a cooling observation feed is produced by
/// instruments, gateways and scripts that a human has to be able to read, diff
/// and hand-write. The parser is total: every malformed line produces a reason
/// and a line number rather than an exception or a partially built record.
struct TextProtoError {
  std::size_t line = 0;
  std::string reason{};
  std::string detail{};
};

/// One parsed statement.
struct Statement {
  std::size_t line = 0;
  std::string key{};
  std::string value{};
};

/// Split text into statements. Comments and blank lines are dropped. A line
/// with no value is a statement with an empty value, which is how a flag or a
/// section marker is written.
[[nodiscard]] Result<std::vector<Statement>> parse_statements(std::string_view text);

/// Parse the values a caller needs often, each reporting the line it failed on
/// so that a diagnostic points at the input rather than at the parser.
[[nodiscard]] Result<std::int64_t> parse_i64(const Statement& statement);
[[nodiscard]] Result<std::uint64_t> parse_u64(const Statement& statement);
[[nodiscard]] Result<bool> parse_bool(const Statement& statement);
[[nodiscard]] Result<TimestampMs> parse_timestamp_statement(const Statement& statement);
[[nodiscard]] Result<Quantity> parse_quantity_statement(const Statement& statement);

/// Parse a whole batch of records. Records begin at a line whose key is
/// "record"; statements accumulate into the record being built.
[[nodiscard]] Result<std::vector<IngestRecord>> parse_records(std::string_view text);

/// Parse one observe request.
[[nodiscard]] Result<ObserveRequest> parse_request(std::string_view text);

/// Render a report as deterministic text. The same report always renders to the
/// same bytes: sections are emitted in a fixed order and every collection is
/// already canonically ordered by the analysis that produced it.
[[nodiscard]] std::string render_text(const ObservationReport& report);

/// Render a report as JSON. Keys are emitted in a fixed order and numbers are
/// written as integers, so the output is diffable and byte-stable.
[[nodiscard]] std::string render_json(const ObservationReport& report);

/// Render the history section of a report as text.
[[nodiscard]] std::string render_history_text(const std::vector<HistoryEntry>& history);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_TEXTPROTO_HPP
