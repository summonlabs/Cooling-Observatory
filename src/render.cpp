// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/render.hpp"

namespace dccp::cooling_observatory {

std::string render_maybe_quantity(const Maybe<Quantity>& value) {
  if (!value.has_value()) {
    return "unknown";
  }
  return render_quantity(value.value());
}

std::string render_subject(const SubjectRef& subject) {
  std::string out;
  out += to_token(subject.kind);
  out += ':';
  out += subject.id.view();
  return out;
}

std::string render_authority(const AuthorityRef& authority) {
  std::string out;
  out += to_token(authority.domain);
  if (!authority.authority.empty()) {
    out += '/';
    out += authority.authority;
  }
  out += '@';
  out += std::to_string(authority.generation);
  if (!authority.digest.empty()) {
    out += '#';
    out += authority.digest.substr(0, 16);
  }
  return out;
}

std::string render_evidence_ref(const EvidenceRef& reference) {
  std::string out;
  out += render_subject(reference.subject);
  out += ' ';
  out += to_token(reference.kind);
  out += " r";
  out += std::to_string(reference.revision.value());
  out += " seq";
  out += std::to_string(reference.record_seq.value());
  out += " epoch";
  out += std::to_string(reference.epoch.value());
  out += " gen";
  out += std::to_string(reference.generation.value());
  out += " ord";
  out += std::to_string(reference.ordinal);
  out += ' ';
  out += to_token(reference.freshness);
  return out;
}

std::string render_freshness(Freshness freshness) { return std::string(to_token(freshness)); }

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0xF];
          out += kHex[c & 0xF];
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

std::string json_string(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  out += json_escape(text);
  out += '"';
  return out;
}

std::string json_bool(bool value) { return value ? "true" : "false"; }

std::string json_int(std::int64_t value) { return std::to_string(value); }

std::string json_u64(std::uint64_t value) { return std::to_string(value); }

std::string json_null() { return "null"; }

std::string json_quantity(const Quantity& value) {
  std::string out;
  out += "{\"dimension\":";
  out += json_string(to_token(value.dimension));
  out += ",\"value\":";
  out += std::to_string(value.value);
  out += ",\"unit\":";
  out += json_string(unit_symbol(value.dimension));
  out += ",\"rendered\":";
  out += json_string(render_quantity(value));
  out += '}';
  return out;
}

std::string json_maybe_quantity(const Maybe<Quantity>& value) {
  if (!value.has_value()) {
    return json_null();
  }
  return json_quantity(value.value());
}

}  // namespace dccp::cooling_observatory
