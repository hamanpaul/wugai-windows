#pragma once
#include "../tsf/profile_display_name_resource.hpp"
#include <new>
namespace boshiamy::installer::profile_display_registry {
using RawRegistryValue = boshiamy::profile_display::resource_name::RawRegistryValue;
enum class DisplayValue { Description, DisplayDescription };
inline constexpr size_t kMaximumBytes = 65536;
// Per-value compare-before-write only: Windows does not make this CAS atomic.
// Caller must authorize/open the exact hive/view/key, serialize writers and supply
// whole-snapshot ownership checks, durable journal, recovery and rollback ordering.
// HKCU COM ownership grants no authority over HKLM metadata. No pair/view atomicity.
inline const wchar_t* value_name(DisplayValue value) {
    switch (value) {
    case DisplayValue::Description: return L"Description";
    case DisplayValue::DisplayDescription: return L"Display Description";
    }
    return nullptr;
}
inline bool valid_raw(const RawRegistryValue& value) {
    return value.status == ERROR_FILE_NOT_FOUND
        ? value.type == 0 && value.bytes.empty()
        : value.status == ERROR_SUCCESS && value.bytes.size() <= kMaximumBytes;
}
inline HRESULT read(HKEY key, DisplayValue value, RawRegistryValue* out) {
    if (!out) return E_POINTER;
    *out = {};
    const wchar_t* name = value_name(value);
    if (!key || !name) return E_INVALIDARG;
    DWORD type = 0, size = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return S_OK;
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    if (size > kMaximumBytes) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    try {
        // Non-null buffer even for zero-byte values: the second call must fetch,
        // rather than act as another size-only query if an external writer grows it.
        std::vector<BYTE> bytes(size ? size : 1);
        DWORD actual = size;
        status = RegQueryValueExW(key, name, nullptr, &type, bytes.data(), &actual);
        if (status == ERROR_FILE_NOT_FOUND) return S_OK;
        if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
        if (actual > size || actual > kMaximumBytes) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        bytes.resize(actual);
        out->status = ERROR_SUCCESS;
        out->type = type;
        out->bytes = std::move(bytes);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
inline HRESULT conditional_write(HKEY key, DisplayValue value,
    const RawRegistryValue& expected, const RawRegistryValue& desired) {
    if (!key || !value_name(value) || !valid_raw(expected) || !valid_raw(desired)) return E_INVALIDARG;
    RawRegistryValue current;
    HRESULT hr = read(key, value, &current);
    if (hr != S_OK) return hr;
    using boshiamy::profile_display::resource_name::same_value;
    if (!same_value(current, expected)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (same_value(current, desired)) return S_OK;
    const LONG status = desired.status == ERROR_FILE_NOT_FOUND
        ? RegDeleteValueW(key, value_name(value))
        : RegSetValueExW(key, value_name(value), 0, desired.type,
            desired.bytes.empty() ? nullptr : desired.bytes.data(), static_cast<DWORD>(desired.bytes.size()));
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    hr = read(key, value, &current);
    if (hr != S_OK) return hr;
    return same_value(current, desired) ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
}
}
