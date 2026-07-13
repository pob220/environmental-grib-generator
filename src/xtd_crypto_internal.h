#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <sodium.h>

namespace environmental_grib::xtd_internal {

inline void EnsureSodium() {
  static const int initialized = sodium_init();
  if (initialized < 0) {
    throw std::runtime_error("could not initialize libsodium");
  }
}

inline std::array<unsigned char, crypto_generichash_BYTES> RuntimeRootKey() {
  EnsureSodium();
  constexpr std::array<std::uint8_t, 32> kPartA{
      0x2d, 0x73, 0xa9, 0x16, 0xe4, 0x5b, 0x8c, 0x01,
      0xf7, 0x38, 0x62, 0xcd, 0x94, 0x0a, 0xb1, 0x57,
      0x6e, 0xd2, 0x43, 0x89, 0x1c, 0xfa, 0x35, 0x70,
      0xab, 0x04, 0xde, 0x91, 0x68, 0x27, 0xc5, 0x3f};
  constexpr std::array<std::uint8_t, 32> kPartB{
      0x86, 0x19, 0x4f, 0xc2, 0x31, 0xe8, 0x75, 0xad,
      0x0b, 0x93, 0xd6, 0x58, 0x2a, 0xbc, 0x47, 0xf0,
      0x15, 0x7c, 0xa3, 0x6d, 0xe1, 0x09, 0x54, 0xca,
      0x72, 0xbf, 0x28, 0x83, 0x0e, 0xd9, 0x64, 0xb6};
  constexpr std::array<unsigned char, 24> kContext{
      'x', 'g', 'r', 'i', 'b', '-', 'x', 't', 'd', '-', 'r', 'u',
      'n', 't', 'i', 'm', 'e', '-', 'k', 'e', 'y', '-', 'v', '1'};

  std::array<unsigned char, 32> combined{};
  for (std::size_t i = 0; i < combined.size(); ++i) {
    combined[i] = static_cast<unsigned char>(kPartA[i] ^ kPartB[i]);
  }
  std::array<unsigned char, crypto_generichash_BYTES> root{};
  if (crypto_generichash(root.data(), root.size(), combined.data(),
                         combined.size(), kContext.data(), kContext.size()) !=
      0) {
    sodium_memzero(combined.data(), combined.size());
    throw std::runtime_error("could not derive XTD runtime key");
  }
  sodium_memzero(combined.data(), combined.size());
  return root;
}

}  // namespace environmental_grib::xtd_internal
