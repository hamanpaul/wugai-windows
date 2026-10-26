#ifndef BOSHIAMY_KEYBOARD_MODES_HPP
#define BOSHIAMY_KEYBOARD_MODES_HPP

#include <optional>
#include <string>

namespace boshiamy {

// Windows-compatible virtual-key values, declared here to keep this header
// usable by portable tests and by hosts that do not include windows.h.
namespace virtual_key {
inline constexpr int back = 0x08;
inline constexpr int tab = 0x09;
inline constexpr int enter = 0x0d;
inline constexpr int shift = 0x10;
inline constexpr int control = 0x11;
inline constexpr int alt = 0x12;
inline constexpr int caps_lock = 0x14;
inline constexpr int escape = 0x1b;
inline constexpr int space = 0x20;

inline constexpr int digit_0 = 0x30;
inline constexpr int digit_1 = 0x31;
inline constexpr int digit_2 = 0x32;
inline constexpr int digit_3 = 0x33;
inline constexpr int digit_4 = 0x34;
inline constexpr int digit_5 = 0x35;
inline constexpr int digit_6 = 0x36;
inline constexpr int digit_7 = 0x37;
inline constexpr int digit_8 = 0x38;
inline constexpr int digit_9 = 0x39;

inline constexpr int a = 0x41;
inline constexpr int b = 0x42;
inline constexpr int c = 0x43;
inline constexpr int d = 0x44;
inline constexpr int e = 0x45;
inline constexpr int f = 0x46;
inline constexpr int g = 0x47;
inline constexpr int h = 0x48;
inline constexpr int i = 0x49;
inline constexpr int j = 0x4a;
inline constexpr int k = 0x4b;
inline constexpr int l = 0x4c;
inline constexpr int m = 0x4d;
inline constexpr int n = 0x4e;
inline constexpr int o = 0x4f;
inline constexpr int p = 0x50;
inline constexpr int q = 0x51;
inline constexpr int r = 0x52;
inline constexpr int s = 0x53;
inline constexpr int t = 0x54;
inline constexpr int u = 0x55;
inline constexpr int v = 0x56;
inline constexpr int w = 0x57;
inline constexpr int x = 0x58;
inline constexpr int y = 0x59;
inline constexpr int z = 0x5a;

inline constexpr int numpad_0 = 0x60;
inline constexpr int numpad_1 = 0x61;
inline constexpr int numpad_2 = 0x62;
inline constexpr int numpad_3 = 0x63;
inline constexpr int numpad_4 = 0x64;
inline constexpr int numpad_5 = 0x65;
inline constexpr int numpad_6 = 0x66;
inline constexpr int numpad_7 = 0x67;
inline constexpr int numpad_8 = 0x68;
inline constexpr int numpad_9 = 0x69;
inline constexpr int numpad_multiply = 0x6a;
inline constexpr int numpad_add = 0x6b;
inline constexpr int numpad_separator = 0x6c;
inline constexpr int numpad_subtract = 0x6d;
inline constexpr int numpad_decimal = 0x6e;
inline constexpr int numpad_divide = 0x6f;

inline constexpr int f1 = 0x70;
inline constexpr int oem_1 = 0xba;
inline constexpr int oem_plus = 0xbb;
inline constexpr int oem_comma = 0xbc;
inline constexpr int oem_minus = 0xbd;
inline constexpr int oem_period = 0xbe;
inline constexpr int oem_2 = 0xbf;
inline constexpr int oem_3 = 0xc0;
inline constexpr int oem_4 = 0xdb;
inline constexpr int oem_5 = 0xdc;
inline constexpr int oem_6 = 0xdd;
inline constexpr int oem_7 = 0xde;
inline constexpr int oem_102 = 0xe2;
} // namespace virtual_key

// These flags are independent host-facing modes; toggling them has no effect
// on an input engine's composition or buffer.
class KeyboardModes {
public:
    bool english() const noexcept { return english_; }
    bool fullwidth() const noexcept { return fullwidth_; }

    void toggle_english() noexcept { english_ = !english_; }
    void toggle_fullwidth() noexcept { fullwidth_ = !fullwidth_; }

private:
    bool english_ = false;
    bool fullwidth_ = false;
};

// Interpret a virtual key using a US keyboard layout. Shift+Caps XOR controls
// letters; Shift selects the upper symbol for the number row and OEM keys.
inline std::optional<char> printable_ascii(int key, bool shift, bool caps) noexcept {
    using namespace virtual_key;

    if (key == space) return ' ';
    if (key >= a && key <= z) {
        const bool upper = shift != caps;
        return static_cast<char>((upper ? 'A' : 'a') + (key - a));
    }

    if (key >= digit_0 && key <= digit_9) {
        static constexpr char plain[] = "0123456789";
        static constexpr char shifted[] = ")!@#$%^&*(";
        const int index = key - digit_0;
        return shift ? shifted[index] : plain[index];
    }

    if (key >= numpad_0 && key <= numpad_9)
        return static_cast<char>('0' + key - numpad_0);

    switch (key) {
    case numpad_multiply: return '*';
    case numpad_add: return '+';
    case numpad_separator: return ',';
    case numpad_subtract: return '-';
    case numpad_decimal: return '.';
    case numpad_divide: return '/';
    case oem_1: return shift ? ':' : ';';
    case oem_plus: return shift ? '+' : '=';
    case oem_comma: return shift ? '<' : ',';
    case oem_minus: return shift ? '_' : '-';
    case oem_period: return shift ? '>' : '.';
    case oem_2: return shift ? '?' : '/';
    case oem_3: return shift ? '~' : '`';
    case oem_4: return shift ? '{' : '[';
    case oem_5: return shift ? '|' : '\\';
    case oem_6: return shift ? '}' : ']';
    case oem_7: return shift ? '"' : '\'';
    case oem_102: return shift ? '>' : '<';
    default: return std::nullopt;
    }
}

// Map printable ASCII to Unicode fullwidth forms (U+3000 for space and
// U+FF01..U+FF5E for ASCII 0x21..0x7E). Unsupported keys return an empty string.
inline std::string fullwidth_utf8(int key, bool shift, bool caps) {
    const auto ascii = printable_ascii(key, shift, caps);
    if (!ascii) return {};
    const unsigned char value = static_cast<unsigned char>(*ascii);
    const unsigned int codepoint = value == 0x20 ? 0x3000 : 0xff01 + value - 0x21;
    std::string result;
    result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    return result;
}

} // namespace boshiamy

#endif
