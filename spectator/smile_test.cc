#include "smile.h"
#include <gtest/gtest.h>

namespace {
using spectator::gzip_uncompress;
using spectator::SmilePayload;

TEST(SmilePayload, Encode) {
  SmilePayload payload;
  payload.Init();

  std::string a(100, 'a');
  static constexpr auto kMax = 1000 * 1000 * 1000;
  for (auto i = 0; i > 0 && i < kMax; i *= 4) {
    payload.Append(i);
  }
  payload.Append("");     // empty string
  payload.Append("abc");  // small string
  payload.Append(a);      // large string
  payload.Append(42.42);  // double
  auto res = payload.Result();

  uint8_t uncompressed[32768];
  size_t dest_len = sizeof(uncompressed);
  gzip_uncompress(uncompressed, &dest_len, res.data, res.size);

  // generated using the jackson-smile generator
  // SmileFactory.builder().configure(SmileGenerator.Feature.CHECK_SHARED_NAMES,
  // false).build();
  std::vector<uint8_t> expected = {
      0x3A, 0x29, 0xA,  0x0,  0xF8, 0x20, 0x42, 0x61, 0x62, 0x63, 0xE0, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
      0x61, 0x61, 0x61, 0xFC, 0x29, 0x0,  0x40, 0x22, 0x4D, 0x38, 0x28, 0x7A,
      0x70, 0x51, 0x76, 0xF9};
  EXPECT_TRUE(memcmp(expected.data(), uncompressed, res.size) == 0);
}
}  // namespace