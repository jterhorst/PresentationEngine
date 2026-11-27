// Text layout through the macOS text seam (Core Text).
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include "text/coretext_layout.h"

using namespace engine;

namespace {

v1::TextElement block(const char* text, v1::VerticalAlign valign) {
    v1::TextElement t;
    auto* r = t.add_runs();
    r->set_text(text);
    r->mutable_style()->set_font_postscript_name("Menlo-Regular");
    r->mutable_style()->set_size_pt(40);
    r->mutable_style()->mutable_fill()->set_r(1);
    r->mutable_style()->mutable_fill()->set_a(1);
    t.set_vertical_align(valign);
    return t;
}

// First and last rows with any ink (alpha > 0), or -1.
std::pair<int, int> ink_rows(const HalfBitmap& b) {
    int first = -1, last = -1;
    for (uint32_t y = 0; y < b.height; ++y)
        for (uint32_t x = 0; x < b.width; ++x)
            if (b.pixels[(static_cast<size_t>(y) * b.width + x) * 4 + 3] != 0) {
                if (first < 0) first = static_cast<int>(y);
                last = static_cast<int>(y);
                break;
            }
    return {first, last};
}

}  // namespace

TEST(TextLayout, TopAlignedTextStartsBelowThePadding) {
    v1::TextElement t = block("X", v1::VERTICAL_ALIGN_TOP);
    t.mutable_padding()->set_top(60);
    t.mutable_padding()->set_left(10);
    HalfBitmap b;
    std::string error;
    ASSERT_TRUE(CoreTextRasterizer().rasterize(t, 400, 300, 1.0, b, error)) << error;
    const auto [first, last] = ink_rows(b);
    ASSERT_GE(first, 0);
    EXPECT_GE(first, 60) << "ink above the top padding";
    EXPECT_LT(first, 60 + 20) << "top-aligned text should sit just under the padding";
}

TEST(TextLayout, BottomAlignedTextEndsAboveThePadding) {
    v1::TextElement t = block("X", v1::VERTICAL_ALIGN_BOTTOM);
    t.mutable_padding()->set_bottom(50);
    HalfBitmap b;
    std::string error;
    ASSERT_TRUE(CoreTextRasterizer().rasterize(t, 400, 300, 1.0, b, error)) << error;
    const auto [first, last] = ink_rows(b);
    EXPECT_LE(last, 300 - 50) << "ink below the bottom padding";
    EXPECT_GT(last, 300 - 50 - 25);
}

TEST(TextLayout, MiddleIsCentredInsideThePaddedBox) {
    v1::TextElement t = block("X", v1::VERTICAL_ALIGN_MIDDLE);
    t.mutable_padding()->set_top(100);  // centre of the padded box is y = 200
    HalfBitmap b;
    std::string error;
    ASSERT_TRUE(CoreTextRasterizer().rasterize(t, 400, 300, 1.0, b, error)) << error;
    const auto [first, last] = ink_rows(b);
    EXPECT_NEAR((first + last) / 2.0, 200, 12);
}

TEST(TextLayout, ShrinkToFitKeepsAllTheText) {
    v1::TextElement t = block("one two three four five six seven eight nine ten", v1::VERTICAL_ALIGN_TOP);
    t.set_fit(v1::TEXT_FIT_SHRINK_TO_FIT);
    HalfBitmap b;
    std::string error;
    ASSERT_TRUE(CoreTextRasterizer().rasterize(t, 300, 60, 1.0, b, error)) << error;
    const auto [first, last] = ink_rows(b);
    EXPECT_GE(first, 0);
    EXPECT_LT(last, 60);
}
