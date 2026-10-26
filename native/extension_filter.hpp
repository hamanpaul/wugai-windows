#ifndef BOSHIAMY_EXTENSION_FILTER_HPP
#define BOSHIAMY_EXTENSION_FILTER_HPP

#include <cstdint>
#include <string>

namespace boshiamy::extension_filter {

inline bool is_extension_han(std::uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Unified Ideographs Extension A
           (cp >= 0x20000 && cp <= 0x2A6DF) || // Extension B
           (cp >= 0x2A700 && cp <= 0x2B73F) || // Extension C
           (cp >= 0x2B740 && cp <= 0x2B81F);   // Extension D
}

// Invalid UTF-8 bytes are skipped one at a time. Valid supplementary-plane
// scalars decode to one code point, so they are checked against the same ranges.
inline bool contains_extension_han(const std::string& utf8) {
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        std::size_t width = 0;
        if (lead <= 0x7F) { cp = lead; width = 1; }
        else if (lead >= 0xC2 && lead <= 0xDF) { cp = lead & 0x1F; width = 2; }
        else if (lead >= 0xE0 && lead <= 0xEF) { cp = lead & 0x0F; width = 3; }
        else if (lead >= 0xF0 && lead <= 0xF4) { cp = lead & 0x07; width = 4; }
        else { ++i; continue; }

        if (i + width > utf8.size()) { ++i; continue; }
        bool valid = true;
        for (std::size_t j = 1; j < width; ++j) {
            const auto byte = static_cast<unsigned char>(utf8[i + j]);
            if ((byte & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (byte & 0x3F);
        }
        if (!valid || (width == 3 && cp < 0x800) ||
            (width == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            ++i;
            continue;
        }
        if (is_extension_han(cp)) return true;
        i += width;
    }
    return false;
}

} // namespace boshiamy::extension_filter
#endif
