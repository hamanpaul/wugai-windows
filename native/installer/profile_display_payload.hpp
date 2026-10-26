#pragma once

#include "profile_display_snapshot.hpp"
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace boshiamy::installer::profile_display_payload {

struct Payload {
    std::vector<BYTE> legacy;
    bool has_display = false;
    profile_display_state::DisplaySnapshot display;
};

namespace detail {
inline constexpr size_t kMaxBytes = 1048576;
inline constexpr size_t kHeaderBytes = 16;
inline constexpr BYTE kMagic[4] = {'P', 'D', 'E', '1'};

inline bool has_magic(const std::vector<BYTE>& bytes) {
    return bytes.size() >= sizeof(kMagic) &&
        bytes[0] == kMagic[0] && bytes[1] == kMagic[1] &&
        bytes[2] == kMagic[2] && bytes[3] == kMagic[3];
}

inline void put_u32(std::vector<BYTE>& bytes, uint32_t value) {
    for (unsigned i = 0; i != 4; ++i)
        bytes.push_back(static_cast<BYTE>(value >> (i * 8)));
}

inline uint32_t get_u32(const std::vector<BYTE>& bytes, size_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
        (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}

inline HRESULT invalid_data() { return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
} // namespace detail

inline HRESULT encode(const Payload& value, std::vector<BYTE>* out) {
    if (!out) return E_POINTER;
    out->clear();
    try {
        if (!value.has_display) {
            if (value.legacy.size() > detail::kMaxBytes || detail::has_magic(value.legacy))
                return detail::invalid_data();
            *out = value.legacy;
            return S_OK;
        }

        if (value.legacy.size() > detail::kMaxBytes - detail::kHeaderBytes)
            return detail::invalid_data();
        std::vector<BYTE> display_bytes;
        const HRESULT display_result = profile_display_state::encode(value.display, &display_bytes);
        if (display_result != S_OK) return detail::invalid_data();
        const size_t remaining = detail::kMaxBytes - detail::kHeaderBytes - value.legacy.size();
        if (display_bytes.empty() || display_bytes.size() > remaining ||
            value.legacy.size() > UINT32_MAX || display_bytes.size() > UINT32_MAX)
            return detail::invalid_data();

        std::vector<BYTE> result;
        result.reserve(detail::kHeaderBytes + value.legacy.size() + display_bytes.size());
        result.insert(result.end(), detail::kMagic, detail::kMagic + sizeof(detail::kMagic));
        detail::put_u32(result, 1);
        detail::put_u32(result, static_cast<uint32_t>(value.legacy.size()));
        detail::put_u32(result, static_cast<uint32_t>(display_bytes.size()));
        result.insert(result.end(), value.legacy.begin(), value.legacy.end());
        result.insert(result.end(), display_bytes.begin(), display_bytes.end());
        *out = std::move(result);
        return S_OK;
    } catch (const std::bad_alloc&) {
        out->clear();
        return E_OUTOFMEMORY;
    } catch (...) {
        out->clear();
        return detail::invalid_data();
    }
}

inline HRESULT decode(const std::vector<BYTE>& bytes, Payload* out) {
    if (!out) return E_POINTER;
    *out = {};
    if (bytes.size() > detail::kMaxBytes) return detail::invalid_data();
    try {
        if (!detail::has_magic(bytes)) {
            out->legacy = bytes;
            return S_OK;
        }
        if (bytes.size() < detail::kHeaderBytes || detail::get_u32(bytes, 4) != 1)
            return detail::invalid_data();

        const uint32_t legacy_size = detail::get_u32(bytes, 8);
        const uint32_t display_size = detail::get_u32(bytes, 12);
        const size_t available = bytes.size() - detail::kHeaderBytes;
        if (legacy_size > available || display_size != available - legacy_size || display_size == 0)
            return detail::invalid_data();

        Payload value;
        value.legacy.assign(bytes.begin() + detail::kHeaderBytes,
                            bytes.begin() + detail::kHeaderBytes + legacy_size);
        std::vector<BYTE> display_bytes(bytes.begin() + detail::kHeaderBytes + legacy_size,
                                       bytes.end());
        const HRESULT display_result = profile_display_state::decode(display_bytes, &value.display);
        if (display_result == E_OUTOFMEMORY) return E_OUTOFMEMORY;
        if (display_result != S_OK) return detail::invalid_data();
        value.has_display = true;
        *out = std::move(value);
        return S_OK;
    } catch (const std::bad_alloc&) {
        *out = {};
        return E_OUTOFMEMORY;
    } catch (...) {
        *out = {};
        return detail::invalid_data();
    }
}

} // namespace boshiamy::installer::profile_display_payload
