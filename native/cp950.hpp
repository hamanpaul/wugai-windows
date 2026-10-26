#ifndef BOSHIAMY_CP950_HPP
#define BOSHIAMY_CP950_HPP

#ifndef _WIN32
#error Strict CP950 eligibility requires the Windows NLS API.
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>
#include <string>

namespace boshiamy {

// Exact UTF-8 -> Windows CP950 -> UTF-16 roundtrip. A default-character or
// best-fit substitution is not eligible for the official legacy text list.
inline bool strict_cp950_roundtrip_utf8(const std::string& utf8) {
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return false;
    const int size = static_cast<int>(utf8.size());
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), size, nullptr, 0);
    if (wide_size <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size,
            &wide[0], wide_size) != wide_size) return false;

    BOOL used_default = FALSE;
    const int encoded_size = WideCharToMultiByte(950, WC_NO_BEST_FIT_CHARS,
        wide.data(), wide_size, nullptr, 0, nullptr, &used_default);
    if (encoded_size <= 0 || used_default) return false;
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    used_default = FALSE;
    if (WideCharToMultiByte(950, WC_NO_BEST_FIT_CHARS, wide.data(), wide_size,
            &encoded[0], encoded_size, nullptr, &used_default) != encoded_size || used_default)
        return false;

    const int decoded_size = MultiByteToWideChar(950, MB_ERR_INVALID_CHARS,
        encoded.data(), encoded_size, nullptr, 0);
    if (decoded_size <= 0) return false;
    std::wstring decoded(static_cast<std::size_t>(decoded_size), L'\0');
    return MultiByteToWideChar(950, MB_ERR_INVALID_CHARS, encoded.data(),
        encoded_size, &decoded[0], decoded_size) == decoded_size && decoded == wide;
}

} // namespace boshiamy

#endif
