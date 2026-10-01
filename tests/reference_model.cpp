// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "reference_model.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <string>

namespace reference {

using dccp::cooling_observatory::PlantLink;

namespace detail {

/// Decimal long multiplication of two magnitudes, then decimal long division.
/// Written on strings so that it shares nothing with the binary implementation
/// it is used to check.
std::string decimal_multiply(const std::string& a, const std::string& b) {
  if (a == "0" || b == "0") {
    return "0";
  }
  std::vector<int> digits(a.size() + b.size(), 0);
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (std::size_t j = 0; j < b.size(); ++j) {
      const int product = (a[a.size() - 1 - i] - '0') * (b[b.size() - 1 - j] - '0');
      digits[i + j] += product;
    }
  }
  for (std::size_t i = 0; i + 1 < digits.size(); ++i) {
    digits[i + 1] += digits[i] / 10;
    digits[i] %= 10;
  }
  std::string out;
  bool started = false;
  for (std::size_t i = digits.size(); i-- > 0;) {
    if (digits[i] != 0) {
      started = true;
    }
    if (started) {
      out.push_back(static_cast<char>('0' + digits[i]));
    }
  }
  return out.empty() ? "0" : out;
}

/// Compare two decimal magnitudes.
int compare_magnitudes(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  if (a == b) {
    return 0;
  }
  return a < b ? -1 : 1;
}

/// Add two decimal magnitudes. Written out so the reference shares no arithmetic
/// with the library it checks.
std::string add_magnitudes(const std::string& a, const std::string& b) {
  std::string out;
  const std::size_t length = std::max(a.size(), b.size()) + 1;
  int carry = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const int left = i < a.size() ? a[a.size() - 1 - i] - '0' : 0;
    const int right = i < b.size() ? b[b.size() - 1 - i] - '0' : 0;
    const int sum = left + right + carry;
    out.insert(out.begin(), static_cast<char>('0' + (sum % 10)));
    carry = sum / 10;
  }
  const std::size_t first = out.find_first_not_of('0');
  return first == std::string::npos ? "0" : out.substr(first);
}

std::string subtract_magnitudes(const std::string& a, const std::string& b) {
  std::string out = a;
  int borrow = 0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int left = out[out.size() - 1 - i] - '0' - borrow;
    const int right = i < b.size() ? b[b.size() - 1 - i] - '0' : 0;
    int digit = left - right;
    if (digit < 0) {
      digit += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    out[out.size() - 1 - i] = static_cast<char>('0' + digit);
  }
  const std::size_t first = out.find_first_not_of('0');
  return first == std::string::npos ? "0" : out.substr(first);
}

/// Long division of a decimal magnitude by a small integer, returning the
/// quotient. The remainder is discarded, which is what integer division does.
std::string divide_magnitude(const std::string& numerator, std::uint64_t denominator) {
  std::string quotient;
  std::uint64_t remainder = 0;
  for (const char c : numerator) {
    const std::uint64_t current = remainder * 10 + static_cast<std::uint64_t>(c - '0');
    quotient.push_back(static_cast<char>('0' + (current / denominator)));
    remainder = current % denominator;
  }
  const std::size_t first = quotient.find_first_not_of('0');
  return first == std::string::npos ? "0" : quotient.substr(first);
}

std::string magnitude_of(std::int64_t value) {
  if (value >= 0) {
    return std::to_string(value);
  }
  // Negate through the unsigned domain so that the most negative value is
  // representable, exactly as the library must.
  const std::uint64_t unsigned_value = static_cast<std::uint64_t>(value);
  const std::uint64_t magnitude = ~unsigned_value + 1ull;
  return std::to_string(magnitude);
}

/// The exact product of two int64 magnitudes as a decimal string. Used so that
/// the reference never forms a 64-bit product that could overflow.
std::string exact_product_string(std::int64_t value, std::int64_t num) {
  return decimal_multiply(magnitude_of(value), magnitude_of(num));
}

/// The exact product of two decimal magnitudes.
std::string exact_product_string(const std::string& a, const std::string& b) {
  return decimal_multiply(a, b);
}

std::int64_t decimal_to_i64(const std::string& magnitude, bool negative) {
  const std::uint64_t value = std::strtoull(magnitude.c_str(), nullptr, 10);
  if (negative) {
    if (value == 0x8000000000000000ull) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(value);
  }
  return static_cast<std::int64_t>(value);
}

/// Divide a decimal magnitude by a divisor, rounding halves away from zero, and
/// return the signed result. The rounding is expressed by scaling the dividend
/// by ten and adding half the divisor, which is a different construction from the
/// library's and therefore an independent check of it.
std::int64_t exact_divide_rounded(const std::string& magnitude, bool negative,
                                  std::uint64_t divisor) {
  const std::string scaled = decimal_multiply(magnitude, "10");
  const std::string biased = add_magnitudes(scaled, std::to_string(divisor * 5));
  return decimal_to_i64(divide_magnitude(biased, divisor * 10), negative);
}

/// (value * num) / den, computed on decimal strings so that the intermediate
/// product is exact and cannot overflow, rounded half away from zero.
std::int64_t exact_mul_div_product(std::int64_t value, std::int64_t num, std::int64_t den) {
  const bool negative = ((value < 0) != (num < 0)) != (den < 0);
  const std::uint64_t divisor = static_cast<std::uint64_t>(den < 0 ? -den : den);
  return exact_divide_rounded(exact_product_string(value, num), negative, divisor);
}

}  // namespace detail

std::int64_t exact_mul_div(std::int64_t value, std::int64_t num, std::int64_t den) {
  return detail::exact_mul_div_product(value, num, den);
}

std::int64_t heat_removal_watts(std::int64_t flow_ul_per_s, std::int64_t difference_mk,
                                std::int64_t capacity_uj_per_l_k) {
  // The derivation from the units, written out so it can be checked by hand:
  //
  //   flow is microlitres per second, which is 1e-6 l/s
  //   capacity is stored in micro-joules per litre per kelvin, which is 1e-6
  //     J/(l*K)
  //   difference is millikelvin, which is 1e-3 K
  //
  // so the product of the three carries 1e-15 and is exactly watts once divided
  // by 1e15. The division happens once, at the end, on the exact product.
  const std::string product = detail::exact_product_string(
      detail::exact_product_string(flow_ul_per_s, difference_mk),
      detail::magnitude_of(capacity_uj_per_l_k));
  return detail::exact_divide_rounded(
      product, ((flow_ul_per_s < 0) != (difference_mk < 0)) != (capacity_uj_per_l_k < 0),
      1000000000000000ull);
}

bool heat_removal_representable(std::int64_t flow_ul_per_s, std::int64_t difference_mk,
                                std::int64_t capacity_uj_per_l_k) {
  // The library forms the product in two checked steps and refuses either if it
  // does not fit. Both steps are checked here on decimal magnitudes, which is a
  // different way of asking the same question.
  const std::string first = detail::exact_product_string(flow_ul_per_s, capacity_uj_per_l_k);
  const std::string second = detail::exact_product_string(first, detail::magnitude_of(difference_mk));
  const std::string limit = std::to_string(std::numeric_limits<std::int64_t>::max());
  return detail::compare_magnitudes(first, limit) <= 0 &&
         detail::compare_magnitudes(second, limit) <= 0;
}

std::int64_t derated(std::int64_t capacity, std::int64_t derate_ppm) {
  return exact_mul_div(capacity, 1000000 - derate_ppm, 1000000);
}

bool fresh_by_age(std::int64_t observed_at_ms, std::int64_t now_ms, std::int64_t bound_ms) {
  const std::int64_t age = now_ms - observed_at_ms;
  if (age < 0) {
    return false;
  }
  return age <= bound_ms;
}

ReferenceTraversal reference_traverse(const PlantModel& model, const SubjectRef& root, bool downstream,
                                     std::uint32_t max_depth) {
  ReferenceTraversal out;
  std::map<std::string, bool> visited;
  auto key_of = [](const SubjectRef& subject) {
    return std::string(to_token(subject.kind)) + "|" + subject.id.str();
  };
  std::deque<std::pair<SubjectRef, std::uint32_t>> queue;
  if (model.knows(root)) {
    queue.emplace_back(root, 0);
    visited[key_of(root)] = true;
    out.nodes.push_back(root);
    out.with_depth.emplace_back(root, 0);
  }
  while (!queue.empty()) {
    const auto current = queue.front();
    queue.pop_front();
    if (current.second >= max_depth) {
      continue;
    }
    std::vector<PlantLink> edges = downstream ? model.outgoing(current.first, true)
                                              : model.incoming(current.first, true);
    std::vector<SubjectRef> neighbours;
    for (const PlantLink& edge : edges) {
      neighbours.push_back(downstream ? edge.to : edge.from);
    }
    std::sort(neighbours.begin(), neighbours.end());
    neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
    for (const SubjectRef& neighbour : neighbours) {
      if (visited.find(key_of(neighbour)) != visited.end()) {
        continue;
      }
      visited[key_of(neighbour)] = true;
      queue.emplace_back(neighbour, current.second + 1);
      out.nodes.push_back(neighbour);
      out.with_depth.emplace_back(neighbour, current.second + 1);
    }
  }
  std::sort(out.nodes.begin(), out.nodes.end());
  return out;
}

std::optional<std::int64_t> sum_flows(const std::vector<std::int64_t>& flows) {
  std::int64_t total = 0;
  for (const std::int64_t flow : flows) {
    if (flow > 0 && total > std::numeric_limits<std::int64_t>::max() - flow) {
      return std::nullopt;
    }
    if (flow < 0 && total < std::numeric_limits<std::int64_t>::min() - flow) {
      return std::nullopt;
    }
    total += flow;
  }
  return total;
}

}  // namespace reference