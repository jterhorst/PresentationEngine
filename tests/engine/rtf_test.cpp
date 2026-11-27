// The supported RTF subset (CONTRACTS.md, "Text content and layout").
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include "text/rtf.h"

using namespace engine::text;

TEST(Rtf, CocoaStyleDocument) {
    const char* rtf =
        "{\\rtf1\\ansi\\ansicpg1252\\cocoartf2822\n"
        "\\cocoatextscaling0\\cocoaplatform0{\\fonttbl\\f0\\fswiss\\fcharset0 Helvetica-Bold;\\f1\\fnil Avenir-Book;}\n"
        "{\\colortbl;\\red255\\green255\\blue255;\\red255\\green200\\blue0;}\n"
        "{\\*\\expandedcolortbl;;\\cssrgb\\c100000\\c100000\\c100000;}\n"
        "\\pard\\tx560\\pardirnatural\\qc\\partightenfactor0\n"
        "\\f0\\b\\fs144 \\cf1 Amazing \\cf2 Grace\\par\n"
        "\\pard\\qc\\sl-1440\\slmult0\\f1\\b0\\fs72 \\cf1 how sweet\\line the sound}";
    StyledText t = parse_rtf(rtf);
    ASSERT_EQ(t.paragraphs.size(), 2u);

    const Paragraph& p0 = t.paragraphs[0];
    EXPECT_EQ(p0.style.align, Align::Center);
    ASSERT_EQ(p0.runs.size(), 2u);
    EXPECT_EQ(p0.runs[0].text, "Amazing ");
    EXPECT_EQ(p0.runs[0].style.font, "Helvetica-Bold");
    EXPECT_FLOAT_EQ(p0.runs[0].style.size_pt, 72);
    EXPECT_TRUE(p0.runs[0].style.bold);
    EXPECT_FLOAT_EQ(p0.runs[0].style.color.r, 1);
    EXPECT_EQ(p0.runs[1].text, "Grace");
    EXPECT_NEAR(p0.runs[1].style.color.g, 200 / 255.0f, 1e-6);

    const Paragraph& p1 = t.paragraphs[1];
    EXPECT_EQ(p1.style.line_spacing_twips, -1440) << "exactly 72 pt";
    EXPECT_FALSE(p1.style.line_spacing_multiple);
    ASSERT_EQ(p1.runs.size(), 1u);
    EXPECT_EQ(p1.runs[0].text, "how sweet\nthe sound");
    EXPECT_EQ(p1.runs[0].style.font, "Avenir-Book");
    EXPECT_FALSE(p1.runs[0].style.bold);
}

TEST(Rtf, UnicodeEscapesAndCodePage) {
    StyledText t = parse_rtf("{\\rtf1\\ansi\\uc1 caf\\u233 e \\'93quoted\\'94 \\u-10179?\\u-8704?}");
    // \u233 = é, its fallback "e" is skipped; \'93 \'94 are cp1252 curly quotes;
    // a surrogate pair arrives as two \u escapes (kept as-is in this prototype).
    const std::string s = t.plain();
    EXPECT_EQ(s.substr(0, 5), "caf\xC3\xA9");
    EXPECT_NE(s.find("\xE2\x80\x9Cquoted\xE2\x80\x9D"), std::string::npos);
}

TEST(Rtf, StylingControlWords) {
    StyledText t = parse_rtf(
        "{\\rtf1{\\colortbl;\\red10\\green20\\blue30;}\\pard\\qr\\sb240\\sa120 "
        "\\i\\ul\\strike\\caps\\expnd4\\highlight1 x\\i0\\ulnone\\strike0\\caps0\\expnd0\\highlight0 y\\tab z}");
    ASSERT_EQ(t.paragraphs.size(), 1u);
    const Paragraph& p = t.paragraphs[0];
    EXPECT_EQ(p.style.align, Align::Right);
    EXPECT_FLOAT_EQ(p.style.space_before_pt, 12);
    EXPECT_FLOAT_EQ(p.style.space_after_pt, 6);
    ASSERT_EQ(p.runs.size(), 2u);
    const CharStyle& a = p.runs[0].style;
    EXPECT_TRUE(a.italic && a.underline && a.strike && a.caps && a.has_highlight);
    EXPECT_FLOAT_EQ(a.tracking_pt, 1);
    EXPECT_NEAR(a.highlight.b, 30 / 255.0f, 1e-6);
    EXPECT_FALSE(p.runs[1].style.italic || p.runs[1].style.has_highlight);
    EXPECT_EQ(p.runs[1].text, "y\tz");
}

TEST(Rtf, GroupsRestoreCharacterState) {
    StyledText t = parse_rtf("{\\rtf1 a{\\b b}c}");
    const auto& runs = t.paragraphs[0].runs;
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_FALSE(runs[0].style.bold);
    EXPECT_TRUE(runs[1].style.bold);
    EXPECT_FALSE(runs[2].style.bold);
}

TEST(Rtf, IgnorableDestinationsAndUnknownWordsAreSkipped) {
    StyledText t = parse_rtf("{\\rtf1{\\*\\generator Writer;}{\\info{\\title T}}\\unknownword12 visible}");
    EXPECT_EQ(t.plain(), "visible");
}

TEST(Rtf, BlankLinesSurviveAsEmptyParagraphs) {
    StyledText t = parse_rtf("{\\rtf1 one\\par\\par three}");
    ASSERT_EQ(t.paragraphs.size(), 3u);
    EXPECT_EQ(t.plain(), "one\n\nthree");
}
