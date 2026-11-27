// SPDX-License-Identifier: MPL-2.0
#include "text/coretext_layout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "text/bitmap_context.h"
#include "text/rtf.h"

namespace engine {

namespace {

template <class T>
struct CF {
    T ref = nullptr;
    CF() = default;
    explicit CF(T r) : ref(r) {}
    ~CF() { if (ref) CFRelease(ref); }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
    operator T() const { return ref; }
};

CFStringRef cfstr(const std::string& s) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), static_cast<CFIndex>(s.size()),
                                   kCFStringEncodingUTF8, false);
}

CTFontRef make_font(const text::CharStyle& st, double size) {
    CF<CFStringRef> name(cfstr(st.font.empty() ? "Helvetica" : st.font));
    CTFontRef font = CTFontCreateWithName(name, size, nullptr);
    CTFontSymbolicTraits want = 0;
    if (st.bold) want |= kCTFontBoldTrait;
    if (st.italic) want |= kCTFontItalicTrait;
    if (want && (CTFontGetSymbolicTraits(font) & want) != want) {
        // The named face lacks the trait (e.g. "Avenir-Book" + \b): ask its family.
        if (CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(font, size, nullptr, want, want)) {
            CFRelease(font);
            font = styled;
        }
    }
    return font;
}

CGColorRef make_color(const text::Rgba& c) {
    CF<CGColorSpaceRef> srgb(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    const CGFloat comps[4] = {c.r, c.g, c.b, c.a};
    return CGColorCreate(srgb, comps);
}

CTParagraphStyleRef make_paragraph(const text::ParaStyle& p, double font_scale) {
    CTTextAlignment align = kCTTextAlignmentLeft;
    switch (p.align) {
    case text::Align::Center: align = kCTTextAlignmentCenter; break;
    case text::Align::Right: align = kCTTextAlignmentRight; break;
    case text::Align::Justified: align = kCTTextAlignmentJustified; break;
    default: break;
    }
    CGFloat before = p.space_before_pt * font_scale, after = p.space_after_pt * font_scale;
    CGFloat min_h = 0, max_h = 0, multiple = 0;
    if (p.line_spacing_multiple && p.line_spacing_twips > 0) {
        multiple = p.line_spacing_twips / 240.0;
    } else if (p.line_spacing_twips < 0) {  // exactly
        min_h = max_h = -p.line_spacing_twips / 20.0 * font_scale;
    } else if (p.line_spacing_twips > 0) {  // at least
        min_h = p.line_spacing_twips / 20.0 * font_scale;
    }
    CTParagraphStyleSetting settings[] = {
        {kCTParagraphStyleSpecifierAlignment, sizeof(align), &align},
        {kCTParagraphStyleSpecifierParagraphSpacingBefore, sizeof(before), &before},
        {kCTParagraphStyleSpecifierParagraphSpacing, sizeof(after), &after},
        {kCTParagraphStyleSpecifierMinimumLineHeight, sizeof(min_h), &min_h},
        {kCTParagraphStyleSpecifierMaximumLineHeight, sizeof(max_h), &max_h},
        {kCTParagraphStyleSpecifierLineHeightMultiple, sizeof(multiple), &multiple},
    };
    return CTParagraphStyleCreate(settings, sizeof(settings) / sizeof(settings[0]));
}

// Builds the attributed string at `font_scale` (shrink-to-fit scales fonts).
CFMutableAttributedStringRef build_string(const text::StyledText& st, double font_scale) {
    CFMutableAttributedStringRef s = CFAttributedStringCreateMutable(nullptr, 0);
    CFAttributedStringBeginEditing(s);
    for (size_t pi = 0; pi < st.paragraphs.size(); ++pi) {
        const auto& para = st.paragraphs[pi];
        const CFIndex para_start = CFAttributedStringGetLength(s);
        for (size_t ri = 0; ri < para.runs.size(); ++ri) {
            const auto& run = para.runs[ri];
            std::string t = run.text;
            // \line is a line break inside the paragraph: U+2028 to Core Text.
            for (size_t k; (k = t.find('\n')) != std::string::npos;) t.replace(k, 1, "\xE2\x80\xA8");
            const bool last_run = ri + 1 == para.runs.size();
            if (last_run && pi + 1 < st.paragraphs.size()) t += "\n";
            if (t.empty()) continue;
            CF<CFStringRef> str(cfstr(t));
            CF<CFMutableStringRef> text_case(CFStringCreateMutableCopy(nullptr, 0, str));
            if (run.style.caps) CFStringUppercase(text_case, nullptr);
            const CFIndex at = CFAttributedStringGetLength(s);
            CFAttributedStringReplaceString(s, CFRangeMake(at, 0), text_case);
            const CFRange r = CFRangeMake(at, CFStringGetLength(text_case));

            CF<CTFontRef> font(make_font(run.style, run.style.size_pt * font_scale));
            CF<CGColorRef> color(make_color(run.style.color));
            CFAttributedStringSetAttribute(s, r, kCTFontAttributeName, font);
            CFAttributedStringSetAttribute(s, r, kCTForegroundColorAttributeName, color);
            if (run.style.underline) {
                const int32_t style = kCTUnderlineStyleSingle;
                CF<CFNumberRef> n(CFNumberCreate(nullptr, kCFNumberSInt32Type, &style));
                CFAttributedStringSetAttribute(s, r, kCTUnderlineStyleAttributeName, n);
            }
            if (run.style.tracking_pt != 0) {
                const CGFloat kern = run.style.tracking_pt * font_scale;
                CF<CFNumberRef> n(CFNumberCreate(nullptr, kCFNumberCGFloatType, &kern));
                CFAttributedStringSetAttribute(s, r, kCTKernAttributeName, n);
            }
        }
        const CFIndex para_end = CFAttributedStringGetLength(s);
        if (para_end > para_start) {
            CF<CTParagraphStyleRef> ps(make_paragraph(para.style, font_scale));
            CFAttributedStringSetAttribute(s, CFRangeMake(para_start, para_end - para_start),
                                           kCTParagraphStyleAttributeName, ps);
        }
    }
    CFAttributedStringEndEditing(s);
    return s;
}

// Path in box coordinates (y down) -> Core Graphics (y up, origin bottom-left).
CGMutablePathRef to_cg_path(const v1::Path& path, double box_h) {
    CGMutablePathRef p = CGPathCreateMutable();
    auto pt = [&](const v1::Vec2& v) { return CGPointMake(v.x(), box_h - v.y()); };
    for (const auto& c : path.commands()) {
        if (c.has_move_to()) CGPathMoveToPoint(p, nullptr, pt(c.move_to()).x, pt(c.move_to()).y);
        else if (c.has_line_to()) CGPathAddLineToPoint(p, nullptr, pt(c.line_to()).x, pt(c.line_to()).y);
        else if (c.has_quad_to()) {
            const auto a = pt(c.quad_to().control()), b = pt(c.quad_to().to());
            CGPathAddQuadCurveToPoint(p, nullptr, a.x, a.y, b.x, b.y);
        } else if (c.has_cubic_to()) {
            const auto a = pt(c.cubic_to().control1()), b = pt(c.cubic_to().control2()), d = pt(c.cubic_to().to());
            CGPathAddCurveToPoint(p, nullptr, a.x, a.y, b.x, b.y, d.x, d.y);
        } else if (c.has_close()) {
            CGPathCloseSubpath(p);
        }
    }
    return p;
}

struct Laid {
    CTFrameRef frame = nullptr;
    bool fits = false;
    double text_top = 0, text_bottom = 0;  // in CG coordinates
    ~Laid() { if (frame) CFRelease(frame); }
};

void lay_out(const text::StyledText& st, double font_scale, CGPathRef container, CFDictionaryRef frame_attrs,
             Laid& out) {
    // Line origins are relative to the container's bounding box, not the bitmap.
    const CGFloat origin_y = CGPathGetBoundingBox(container).origin.y;
    CF<CFMutableAttributedStringRef> s(build_string(st, font_scale));
    CF<CTFramesetterRef> fs(CTFramesetterCreateWithAttributedString(s));
    out.frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), container, frame_attrs);
    const CFRange visible = CTFrameGetVisibleStringRange(out.frame);
    out.fits = visible.location + visible.length >= CFAttributedStringGetLength(s);
    CFArrayRef lines = CTFrameGetLines(out.frame);
    const CFIndex n = CFArrayGetCount(lines);
    if (n == 0) return;
    CGPoint first, last;
    CTFrameGetLineOrigins(out.frame, CFRangeMake(0, 1), &first);
    CTFrameGetLineOrigins(out.frame, CFRangeMake(n - 1, 1), &last);
    CGFloat asc = 0, desc = 0, lead = 0;
    CTLineGetTypographicBounds(static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, 0)), &asc, &desc, &lead);
    out.text_top = origin_y + first.y + asc;
    CTLineGetTypographicBounds(static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, n - 1)), &asc, &desc, &lead);
    out.text_bottom = origin_y + last.y - desc;
}

}  // namespace

bool CoreTextRasterizer::rasterize(const v1::TextElement& text, double box_w, double box_h, double scale,
                                   HalfBitmap& out, std::string& error) {
    if (box_w <= 0 || box_h <= 0) {
        error = "empty text box";
        return false;
    }
    const text::StyledText st = text::styled_text_of(text);

    // Container: the element's path, or the box inset by padding.
    CF<CGMutablePathRef> container;
    CGRect bounds;
    if (text.has_container()) {
        container.ref = to_cg_path(text.container(), box_h);
        bounds = CGPathGetBoundingBox(container);
    } else {
        const auto& pad = text.padding();
        bounds = CGRectMake(pad.left(), pad.bottom(), box_w - pad.left() - pad.right(), box_h - pad.top() - pad.bottom());
        container.ref = CGPathCreateMutable();
        CGPathAddRect(container, nullptr, bounds);
    }

    // Exclusions: areas text wraps around.
    CF<CFMutableArrayRef> clips(CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks));
    for (const auto& ex : text.exclusions()) {
        CF<CGMutablePathRef> p(to_cg_path(ex, box_h));
        const void* keys[] = {kCTFramePathClippingPathAttributeName};
        const void* vals[] = {p.ref};
        CF<CFDictionaryRef> d(CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks));
        CFArrayAppendValue(clips, d);
    }
    const void* fkeys[] = {kCTFrameClippingPathsAttributeName};
    const void* fvals[] = {clips.ref};
    CF<CFDictionaryRef> frame_attrs(CFDictionaryCreate(nullptr, fkeys, fvals, 1, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks));

    auto laid = std::make_unique<Laid>();
    lay_out(st, 1.0, container, frame_attrs, *laid);
    if (!laid->fits && text.fit() == v1::TEXT_FIT_SHRINK_TO_FIT) {
        // Largest uniform font scale that fits; a few bisection steps are plenty.
        double lo = 0.05, hi = 1.0;
        for (int i = 0; i < 10; ++i) {
            const double mid = (lo + hi) / 2;
            Laid probe;
            lay_out(st, mid, container, frame_attrs, probe);
            (probe.fits ? lo : hi) = mid;
        }
        laid = std::make_unique<Laid>();
        lay_out(st, lo, container, frame_attrs, *laid);
    }

    // Vertical alignment within the container's bounds; unspecified centers,
    // as presentation text usually does. Core Graphics is y-up, so moving the
    // block down is a negative translation.
    const double used = laid->text_top - laid->text_bottom;
    const double room = bounds.size.height;
    const double top_gap = (bounds.origin.y + room) - laid->text_top;
    double dy = 0;
    switch (text.vertical_align()) {
    case v1::VERTICAL_ALIGN_TOP: dy = top_gap; break;
    case v1::VERTICAL_ALIGN_BOTTOM: dy = top_gap - (room - used); break;
    default: dy = top_gap - (room - used) / 2; break;
    }

    BitmapContext ctx(static_cast<uint32_t>(std::ceil(box_w * scale)), static_cast<uint32_t>(std::ceil(box_h * scale)));
    if (!ctx.ok()) {
        error = "bitmap context failed";
        return false;
    }
    CGContextScaleCTM(ctx.ref(), scale, scale);
    CGContextTranslateCTM(ctx.ref(), 0, dy);
    CTFrameDraw(laid->frame, ctx.ref());
    ctx.copy_to(out);
    return true;
}

bool ImageIODecoder::decode(const std::string& path, HalfBitmap& out, std::string& error) {
    return decode_image_file(path, out, error);
}

}  // namespace engine
