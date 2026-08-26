/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstdint>
#include <string>

#include "third_party/catch/include/catch.hpp"
#include "third_party/crypto/sha256.h"

namespace xe::test {

TEST_CASE("SHA-256 matches standard known vectors", "[sha256]") {
  sha256::SHA256 hash;
  REQUIRE(hash(std::string()) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(hash(std::string("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256 hashes owned bytes incrementally and returns raw bytes",
          "[sha256]") {
  sha256::SHA256 hash;
  constexpr std::array<uint8_t, 3> input = {'a', 'b', 'c'};
  hash.add(input.data(), 1);
  hash.add(input.data() + 1, input.size() - 1);

  std::array<unsigned char, sha256::SHA256::HashBytes> digest = {};
  hash.getHash(digest.data());
  constexpr std::array<unsigned char, sha256::SHA256::HashBytes> expected = {
      0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA, 0x41, 0x41, 0x40,
      0xDE, 0x5D, 0xAE, 0x22, 0x23, 0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17,
      0x7A, 0x9C, 0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
  };
  REQUIRE(digest == expected);
}

}  // namespace xe::test
