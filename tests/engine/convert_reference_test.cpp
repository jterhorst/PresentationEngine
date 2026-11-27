// The CPU conversion reference itself: legal-range codes and the v210 layout.
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include "render/convert_reference.h"

using namespace engine::convert_ref;

TEST(ConvertReference, BlackAndWhiteHitLegalRange) {
    std::vector<Pixel> px = {{0, 0, 0, 1}, {1, 1, 1, 1}};
    auto uyvy = to_uyvy(px, 2, 1, uyvy_row_words(2));
    // U Y0 V Y1: chroma 128, black 16, white 235.
    EXPECT_EQ(uyvy[0], 128u | (16u << 8) | (128u << 16) | (235u << 24));

    std::vector<Pixel> six(6, {1, 1, 1, 1});
    six[0] = {0, 0, 0, 1};
    auto v210 = to_v210(six, 6, 1, v210_row_words(6));
    EXPECT_EQ(v210[0] & 0x3FF, 512u) << "Cb0 neutral";
    EXPECT_EQ((v210[0] >> 10) & 0x3FF, 64u) << "Y0 black = 64";
    EXPECT_EQ(v210[1] & 0x3FF, 940u) << "Y1 white = 940";
}

TEST(ConvertReference, PureRedChroma) {
    std::vector<Pixel> px = {{1, 0, 0, 1}, {1, 0, 0, 1}};
    auto w = to_uyvy(px, 2, 1, 1)[0];
    EXPECT_EQ((w >> 8) & 0xFF, 63u);   // Y  = 16 + 0.2126 * 219
    EXPECT_EQ(w & 0xFF, 102u);          // Cb
    EXPECT_EQ((w >> 16) & 0xFF, 240u);  // Cr
}

TEST(ConvertReference, V210RowStrideIsDeckLinks) {
    EXPECT_EQ(v210_row_words(1920) * 4, 5120u);  // matches the null module's 1080p row_bytes
    EXPECT_EQ(uyvy_row_words(1920) * 4, 3840u);
}
