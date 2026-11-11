#pragma once
// Making text that should be UTF-8 safe to hand to a strict UTF-8 reader (nlohmann's parser).

#include <cstddef>
#include <string>
#include <string_view>

namespace omenkbd {

/// `text` with every ill-formed UTF-8 sequence replaced by U+FFFD (one per maximal ill-formed
/// subpart, as Unicode recommends). Well-formed means Unicode's table 3-7: no overlong forms, no
/// surrogates (U+D800..U+DFFF), nothing past U+10FFFF, no lone or missing continuation bytes.
/// Valid text, ASCII included, comes back unchanged.
inline std::string scrub_utf8(std::string_view text) {
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const auto in = [](unsigned char b, unsigned char lo, unsigned char hi) {
        return b >= lo && b <= hi;
    };
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = byte(i);
        if (lead < 0x80) {
            out.push_back(static_cast<char>(lead));
            ++i;
            continue;
        }
        // How many bytes the sequence has, and the range its second byte must fall in.
        std::size_t length = 0;
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        if (in(lead, 0xC2, 0xDF)) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            lo = 0xA0;
        } else if (in(lead, 0xE1, 0xEC) || in(lead, 0xEE, 0xEF)) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            hi = 0x9F;
        } else if (lead == 0xF0) {
            length = 4;
            lo = 0x90;
        } else if (in(lead, 0xF1, 0xF3)) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            hi = 0x8F;
        }
        // Count the bytes that still fit a well-formed sequence after the lead.
        std::size_t good = 1;
        if (length > 0 && i + 1 < text.size() && in(byte(i + 1), lo, hi)) {
            good = 2;
            while (good < length && i + good < text.size() && in(byte(i + good), 0x80, 0xBF)) {
                ++good;
            }
        }
        if (length > 0 && good == length) {
            out.append(text.substr(i, length));
        } else {
            out.append(kReplacement);
        }
        i += good;
    }
    return out;
}

} // namespace omenkbd
