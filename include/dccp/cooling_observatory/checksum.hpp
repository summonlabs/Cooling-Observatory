// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_CHECKSUM_HPP
#define DCCP_COOLING_OBSERVATORY_CHECKSUM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/cooling_observatory/error.hpp"

namespace dccp::cooling_observatory {

/// CRC-32C (Castagnoli), reflected, initial value all ones, final xor all ones.
///
/// This is the integrity check of the durable record: it answers "were these
/// exact bytes written", which is what a torn tail or a flipped bit violates.
/// It is not a security primitive and is not used as one.
[[nodiscard]] std::uint32_t crc32c(const std::uint8_t* data, std::size_t length, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view text) noexcept;

/// SHA-256 (FIPS 180-4), implemented here: the observatory's canonicalisation
/// digest must not vary with a platform library, and the repository must build
/// with no third-party dependency.
class Sha256 {
 public:
  static constexpr std::size_t kDigestSize = 32;
  static constexpr std::size_t kBlockSize = 64;

  Sha256() noexcept;

  void update(const std::uint8_t* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalise. The object is left in a spent state; reset() to reuse it.
  void finish(std::uint8_t out[kDigestSize]) noexcept;

  [[nodiscard]] std::string hex_digest();

  void reset() noexcept;

 private:
  void compress(const std::uint8_t block[kBlockSize]) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_count_;
  std::uint8_t buffer_[kBlockSize];
  std::size_t buffered_;
};

/// Lower-case hex SHA-256 of a string.
[[nodiscard]] std::string sha256_hex(std::string_view text);

/// Lower-case hex SHA-256 of a byte range.
[[nodiscard]] std::string sha256_hex(const std::uint8_t* data, std::size_t length);

/// Parse 64 lower-case (or upper-case) hex characters into a digest.
[[nodiscard]] Result<std::string> normalise_sha256_hex(std::string_view text);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_CHECKSUM_HPP
