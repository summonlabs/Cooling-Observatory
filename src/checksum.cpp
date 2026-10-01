// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/checksum.hpp"

#include <array>
#include <cstring>

namespace dccp::cooling_observatory {
namespace {

/// CRC-32C reflected polynomial, bit-reversed.
constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78u;

/// The 256-entry lookup table is built once, at first use, from the polynomial
/// rather than being pasted in as 256 magic numbers that no reviewer can check.
struct Crc32cTable {
  std::array<std::uint32_t, 256> entries{};

  /// Built by walking the array rather than by indexing it. The two are
  /// equivalent, and the walk carries no index for an analyser to bound: the
  /// indexed form is a bounds obligation a reader has to discharge by hand, and
  /// the static analyser could not, which made it a build failure under /WX.
  constexpr Crc32cTable() noexcept {
    std::uint32_t index = 0;
    for (std::uint32_t& slot : entries) {
      std::uint32_t crc = index++;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1) ^ kCrc32cPolynomial : (crc >> 1);
      }
      slot = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
}};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

}  // namespace

std::uint32_t crc32c(const std::uint8_t* data, std::size_t length, std::uint32_t seed) noexcept {
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < length; ++i) {
    crc = kCrc32cTable.entries[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), 0);
}

Sha256::Sha256() noexcept { reset(); }

void Sha256::reset() noexcept {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  bit_count_ = 0;
  buffered_ = 0;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::compress(const std::uint8_t block[kBlockSize]) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) noexcept {
  if (length == 0) {
    return;
  }
  bit_count_ += static_cast<std::uint64_t>(length) * 8u;
  std::size_t offset = 0;
  if (buffered_ > 0) {
    while (offset < length && buffered_ < kBlockSize) {
      buffer_[buffered_++] = data[offset++];
    }
    if (buffered_ == kBlockSize) {
      compress(buffer_);
      buffered_ = 0;
    }
  }
  while (length - offset >= kBlockSize) {
    compress(data + offset);
    offset += kBlockSize;
  }
  // A full buffer is compressed before the tail is copied. After the loop above
  // the tail is shorter than one block, so this guard is never taken in
  // practice; it is written out because the alternative is an indexing argument
  // every reader — and every analyser — has to reconstruct from the code above.
  while (offset < length) {
    if (buffered_ >= kBlockSize) {
      compress(buffer_);
      buffered_ = 0;
    }
    buffer_[buffered_++] = data[offset++];
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Sha256::finish(std::uint8_t out[kDigestSize]) noexcept {
  const std::uint64_t bits = bit_count_;
  const std::uint8_t pad = 0x80u;
  update(&pad, 1);
  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (int i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bits >> (56 - 8 * i)) & 0xFFu);
  }
  update(length_bytes, 8);
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
}

namespace {

std::string to_hex(const std::uint8_t* bytes, std::size_t length) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out[i * 2] = kDigits[(bytes[i] >> 4) & 0xFu];
    out[i * 2 + 1] = kDigits[bytes[i] & 0xFu];
  }
  return out;
}

}  // namespace

std::string Sha256::hex_digest() {
  std::uint8_t digest[kDigestSize];
  finish(digest);
  return to_hex(digest, kDigestSize);
}

std::string sha256_hex(std::string_view text) {
  Sha256 hasher;
  hasher.update(text);
  return hasher.hex_digest();
}

std::string sha256_hex(const std::uint8_t* data, std::size_t length) {
  Sha256 hasher;
  hasher.update(data, length);
  return hasher.hex_digest();
}

Result<std::string> normalise_sha256_hex(std::string_view text) {
  if (text.size() != 64) {
    return fail(Code::MalformedInput, "digest_wrong_length",
                "a SHA-256 digest is 64 hex characters; got " + std::to_string(text.size()));
  }
  std::string out;
  out.reserve(64);
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      out.push_back(c);
    } else if (c >= 'a' && c <= 'f') {
      out.push_back(c);
    } else if (c >= 'A' && c <= 'F') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      return fail(Code::MalformedInput, "digest_not_hex", "a SHA-256 digest is hex characters only");
    }
  }
  return out;
}

}  // namespace dccp::cooling_observatory
