// The shared text library, part 1: RTF subset -> styled paragraphs.
//
// RTF is the source of truth for styled text (CONTRACTS.md, "Text content and
// layout"). This parser handles exactly the supported subset; other control
// words are ignored when rendering. It's portable C++ so the engine, the
// native editor and (via WebAssembly) the web UI all read RTF the same way.
// Layout lives behind the platform seam (coretext_layout.h on macOS).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace engine::v1 {
class TextElement;
}

namespace engine::text {

struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;  // sRGB, straight alpha
    bool operator==(const Rgba&) const = default;
};

struct CharStyle {
    std::string font;          // font table name: a PostScript or family name
    float size_pt = 12;        // RTF default \fs24
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool caps = false;
    Rgba color;
    bool has_highlight = false;
    Rgba highlight;
    float tracking_pt = 0;     // \expnd (quarter points) or \expndtw (twips)
    bool operator==(const CharStyle&) const = default;
};

enum class Align { Left, Center, Right, Justified };

struct ParaStyle {
    Align align = Align::Left;
    // \slN: N > 0 "at least N twips", N < 0 "exactly -N twips"; with \slmult1,
    // N/240 is a multiple of single spacing.
    int line_spacing_twips = 0;
    bool line_spacing_multiple = false;
    float space_before_pt = 0;
    float space_after_pt = 0;
    bool operator==(const ParaStyle&) const = default;
};

struct Run {
    std::string text;          // UTF-8; "\n" inside a paragraph is a line break (\line)
    CharStyle style;
};

struct Paragraph {
    ParaStyle style;
    std::vector<Run> runs;
};

struct StyledText {
    std::vector<Paragraph> paragraphs;
    std::string plain() const;  // paragraphs joined with "\n"
};

StyledText parse_rtf(std::string_view rtf);

// The element's text: its RTF when present, otherwise its runs.
StyledText styled_text_of(const engine::v1::TextElement& element);

}  // namespace engine::text
