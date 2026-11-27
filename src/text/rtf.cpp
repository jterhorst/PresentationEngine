// SPDX-License-Identifier: MPL-2.0
#include "text/rtf.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>

#include "engine/v1/document.pb.h"

namespace engine::text {

namespace {

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Windows-1252 0x80..0x9F; the rest of the code page matches Latin-1.
uint32_t cp1252(uint8_t b) {
    static const uint16_t high[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                      0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    return (b >= 0x80 && b <= 0x9F) ? high[b - 0x80] : b;
}

struct State {
    CharStyle ch;
    ParaStyle para;
    int font_index = -1;
    int uc_skip = 1;
    enum class Dest { Text, Skip, FontTable, ColorTable } dest = Dest::Text;
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    StyledText run() {
        out_.paragraphs.emplace_back();
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '{') {
                ++pos_;
                stack_.push_back(st_);
            } else if (c == '}') {
                ++pos_;
                end_group();
            } else if (c == '\\') {
                control();
            } else if (c == '\r' || c == '\n') {
                ++pos_;
            } else {
                ++pos_;
                text_byte(static_cast<uint8_t>(c));
            }
        }
        // Drop the trailing empty paragraph a final \par leaves behind.
        if (out_.paragraphs.size() > 1 && out_.paragraphs.back().runs.empty()) out_.paragraphs.pop_back();
        return out_;
    }

private:
    void end_group() {
        if (st_.dest == State::Dest::FontTable && !font_name_.empty()) finish_font();
        if (stack_.empty()) return;
        // Leaving a font or color table group returns to text; tables persist.
        st_ = stack_.back();
        stack_.pop_back();
    }

    void control() {
        ++pos_;  // backslash
        if (pos_ >= s_.size()) return;
        const char c = s_[pos_];
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            ++pos_;
            switch (c) {
            case '\\': case '{': case '}': text_cp(static_cast<uint8_t>(c)); break;
            case '~': text_cp(0x00A0); break;
            case '_': text_cp(0x2011); break;
            case '\'': {
                if (pos_ + 2 <= s_.size()) {
                    const int v = std::strtol(std::string(s_.substr(pos_, 2)).c_str(), nullptr, 16);
                    pos_ += 2;
                    if (skip_pending_ > 0) --skip_pending_;
                    else text_byte(static_cast<uint8_t>(v));
                }
                break;
            }
            case '*': st_.dest = State::Dest::Skip; break;  // ignorable destination
            case '\n': case '\r': paragraph(); break;     // "\<newline>" is \par
            default: break;                              // \- and others: ignore
            }
            return;
        }
        size_t start = pos_;
        while (pos_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        const std::string_view word = s_.substr(start, pos_ - start);
        bool has_param = false;
        long param = 0;
        if (pos_ < s_.size() && (s_[pos_] == '-' || std::isdigit(static_cast<unsigned char>(s_[pos_])))) {
            size_t ps = pos_;
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
            param = std::strtol(std::string(s_.substr(ps, pos_ - ps)).c_str(), nullptr, 10);
            has_param = true;
        }
        if (pos_ < s_.size() && s_[pos_] == ' ') ++pos_;  // the delimiter space belongs to the word
        word_(word, has_param, param);
    }

    void word_(std::string_view w, bool has, long n) {
        const bool on = !has || n != 0;
        if (w == "fonttbl") { st_.dest = State::Dest::FontTable; return; }
        if (w == "colortbl") { st_.dest = State::Dest::ColorTable; colors_.clear(); pending_ = {}; pending_set_ = false; return; }
        if (w == "stylesheet" || w == "info" || w == "pict" || w == "header" || w == "footer" ||
            w == "expandedcolortbl" || w == "listtable" || w == "listoverridetable") {
            st_.dest = State::Dest::Skip;
            return;
        }
        if (st_.dest == State::Dest::Skip) return;
        if (st_.dest == State::Dest::FontTable) {
            if (w == "f" && has) {
                if (!font_name_.empty()) finish_font();
                font_def_ = static_cast<int>(n);
            }
            return;
        }
        if (st_.dest == State::Dest::ColorTable) {
            if (w == "red") { pending_.r = n / 255.0f; pending_set_ = true; }
            else if (w == "green") { pending_.g = n / 255.0f; pending_set_ = true; }
            else if (w == "blue") { pending_.b = n / 255.0f; pending_set_ = true; }
            return;
        }
        if (w == "par") paragraph();
        else if (w == "line") text_cp('\n');
        else if (w == "tab") text_cp('\t');
        else if (w == "pard") st_.para = ParaStyle{};
        else if (w == "plain") { st_.ch = CharStyle{}; apply_font(st_.font_index); }
        else if (w == "b") st_.ch.bold = on;
        else if (w == "i") st_.ch.italic = on;
        else if (w == "ul") st_.ch.underline = on;
        else if (w == "ulnone") st_.ch.underline = false;
        else if (w == "strike") st_.ch.strike = on;
        else if (w == "caps") st_.ch.caps = on;
        else if (w == "f" && has) apply_font(static_cast<int>(n));
        else if (w == "fs" && has) st_.ch.size_pt = n / 2.0f;
        else if (w == "cf" && has) st_.ch.color = color(n);
        else if ((w == "highlight" || w == "cb") && has) {
            st_.ch.has_highlight = n > 0;
            st_.ch.highlight = color(n);
        } else if (w == "ql") st_.para.align = Align::Left;
        else if (w == "qc") st_.para.align = Align::Center;
        else if (w == "qr") st_.para.align = Align::Right;
        else if (w == "qj") st_.para.align = Align::Justified;
        else if (w == "sl" && has) st_.para.line_spacing_twips = static_cast<int>(n);
        else if (w == "slmult" && has) st_.para.line_spacing_multiple = n != 0;
        else if (w == "sb" && has) st_.para.space_before_pt = n / 20.0f;
        else if (w == "sa" && has) st_.para.space_after_pt = n / 20.0f;
        else if (w == "expnd" && has) st_.ch.tracking_pt = n / 4.0f;
        else if (w == "expndtw" && has) st_.ch.tracking_pt = n / 20.0f;
        else if (w == "uc" && has) st_.uc_skip = static_cast<int>(n);
        else if (w == "u" && has) {
            text_cp(static_cast<uint32_t>(n < 0 ? n + 65536 : n));
            skip_pending_ = st_.uc_skip;
        }
        // Everything else (\rtf1, \ansi, \cocoartf, \pardirnatural, ...) is ignored.
    }

    Rgba color(long index) const {
        if (index <= 0 || index >= static_cast<long>(colors_.size())) return Rgba{};
        return colors_[index];
    }

    void apply_font(int index) {
        st_.font_index = index;
        auto it = fonts_.find(index);
        if (it != fonts_.end()) st_.ch.font = it->second;
    }

    void finish_font() {
        std::string name = font_name_;
        while (!name.empty() && (name.back() == ';' || name.back() == ' ')) name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (font_def_ >= 0) fonts_[font_def_] = name;
        font_name_.clear();
    }

    void text_byte(uint8_t b) {
        if (st_.dest == State::Dest::FontTable) {
            font_name_ += static_cast<char>(b);
            if (b == ';') finish_font();
            return;
        }
        if (st_.dest == State::Dest::ColorTable) {
            if (b == ';') {
                colors_.push_back(pending_set_ ? pending_ : Rgba{});
                pending_ = {};
                pending_set_ = false;
            }
            return;
        }
        if (skip_pending_ > 0) {
            --skip_pending_;
            return;
        }
        text_cp(cp1252(b));
    }

    void text_cp(uint32_t cp) {
        if (st_.dest != State::Dest::Text) return;
        skip_pending_ = 0;
        Paragraph& p = out_.paragraphs.back();
        if (p.runs.empty()) p.style = st_.para;
        if (p.runs.empty() || !(p.runs.back().style == st_.ch)) p.runs.push_back(Run{{}, st_.ch});
        append_utf8(p.runs.back().text, cp);
    }

    void paragraph() {
        if (st_.dest != State::Dest::Text) return;
        Paragraph& p = out_.paragraphs.back();
        if (p.runs.empty()) {
            // Keep empty paragraphs (blank lines), styled like the current text.
            p.style = st_.para;
            p.runs.push_back(Run{{}, st_.ch});
        }
        out_.paragraphs.emplace_back();
    }

    std::string_view s_;
    size_t pos_ = 0;
    State st_;
    std::vector<State> stack_;
    std::map<int, std::string> fonts_;
    std::string font_name_;
    int font_def_ = -1;
    std::vector<Rgba> colors_;
    Rgba pending_;
    bool pending_set_ = false;
    int skip_pending_ = 0;
    StyledText out_;
};

}  // namespace

std::string StyledText::plain() const {
    std::string s;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (i) s += '\n';
        for (const auto& r : paragraphs[i].runs) s += r.text;
    }
    return s;
}

StyledText parse_rtf(std::string_view rtf) { return Parser(rtf).run(); }

StyledText styled_text_of(const engine::v1::TextElement& element) {
    if (!element.rtf().empty()) return parse_rtf(element.rtf());
    StyledText out;
    out.paragraphs.emplace_back();
    ParaStyle para;
    switch (element.paragraph().align()) {
    case engine::v1::TEXT_ALIGN_CENTER: para.align = Align::Center; break;
    case engine::v1::TEXT_ALIGN_RIGHT: para.align = Align::Right; break;
    case engine::v1::TEXT_ALIGN_JUSTIFIED: para.align = Align::Justified; break;
    default: break;
    }
    if (element.paragraph().line_height_multiple() > 0) {
        para.line_spacing_multiple = true;
        para.line_spacing_twips = static_cast<int>(element.paragraph().line_height_multiple() * 240);
    }
    para.space_after_pt = element.paragraph().paragraph_spacing_pt();
    out.paragraphs.back().style = para;
    for (const auto& r : element.runs()) {
        CharStyle cs;
        const auto& st = r.style();
        cs.font = !st.font_postscript_name().empty() ? st.font_postscript_name() : st.font_family();
        cs.size_pt = st.size_pt() > 0 ? st.size_pt() : 12;
        cs.bold = st.weight() >= 600;
        cs.italic = st.italic();
        cs.underline = st.underline();
        cs.caps = st.text_case() == engine::v1::TEXT_CASE_UPPER;
        if (st.has_fill()) cs.color = {st.fill().r(), st.fill().g(), st.fill().b(), st.fill().a()};
        cs.tracking_pt = st.tracking() / 1000.0f * cs.size_pt;
        // Runs split into paragraphs at '\n'.
        size_t start = 0;
        const std::string& t = r.text();
        while (true) {
            size_t nl = t.find('\n', start);
            out.paragraphs.back().runs.push_back(Run{t.substr(start, nl - start), cs});
            if (nl == std::string::npos) break;
            out.paragraphs.emplace_back();
            out.paragraphs.back().style = para;
            start = nl + 1;
        }
    }
    return out;
}

}  // namespace engine::text
