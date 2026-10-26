#pragma once

#include "profile_display_plan.hpp"
#include "file_deployment.hpp"
#include <shlwapi.h>
#include <functional>

namespace boshiamy::installer::profile_display_health {
using Snapshot = profile_display_state::DisplaySnapshot;
namespace resource_name = profile_display::resource_name;

namespace detail {
// Test boundary only. The public production entry always supplies Windows operations.
struct Operations {
    std::function<HRESULT()> validate_module;
    std::function<HRESULT(std::string*)> hash;
    std::function<HRESULT()> resource;
    std::function<HRESULT(const std::wstring&, std::wstring*)> resolve;
};

inline HRESULT mismatch() {
    return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
}

inline HRESULT validate_target(const Snapshot& snapshot, const std::wstring& module,
                               const std::string& expected_hash) {
    if (!files::detail::valid_hash(expected_hash)) return E_INVALIDARG;
    Snapshot planned;
    const HRESULT status = profile_display_plan::make_target(snapshot, module, &planned);
    if (status != S_OK) return status;
    // Includes exact API/registry name and both raw Description and binding values.
    return profile_display_state::equal(snapshot, planned) ? S_OK : mismatch();
}

inline HRESULT verify_with_operations(const Snapshot& snapshot, const std::wstring& module,
                                      const std::string& expected_hash, const Operations& operations) {
    HRESULT status = validate_target(snapshot, module, expected_hash);
    if (status != S_OK) return status;
    if (!operations.validate_module || !operations.hash || !operations.resource || !operations.resolve)
        return E_INVALIDARG;
    status = operations.validate_module();
    if (status != S_OK) return status;
    std::string hash;
    status = operations.hash(&hash);
    if (status != S_OK) return status;
    if (hash != expected_hash) return mismatch();
    status = operations.resource();
    if (status != S_OK) return status;
    for (const auto* raw : {&snapshot.display_binding.registry64, &snapshot.display_binding.registry32}) {
        std::wstring binding;
        if (!resource_name::raw_value_text(*raw, &binding))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::wstring resolved;
        status = operations.resolve(binding, &resolved);
        if (status != S_OK) return status;
        if (resolved != profile_display::kDisplayDescription) return mismatch();
    }
    hash.clear();
    status = operations.hash(&hash);
    if (status != S_OK) return status;
    if (hash != expected_hash) return mismatch();
    return operations.validate_module();
}

inline HRESULT validate_module(const std::filesystem::path& module) {
    std::error_code error;
    if (!files::detail::absolute_no_reparse(module, true)) return E_ACCESSDENIED;
    if (!std::filesystem::is_regular_file(module, error) || error)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (!files::detail::resource_only_branding(module) ||
        !files::detail::architecture(module, true, true))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return S_OK;
}

inline HRESULT validate_resource_201(const std::wstring& module_path) {
    // No code execution or DllMain: this is exclusively a resource mapping.
    HMODULE module = LoadLibraryExW(module_path.c_str(), nullptr,
                                   LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t text[64]{};
    const int length = LoadStringW(module, resource_name::kDisplayNameResourceId,
                                  text, static_cast<int>(std::size(text)));
    const bool exact = length > 0 &&
        std::wstring(text, static_cast<size_t>(length)) == profile_display::kDisplayDescription;
    FreeLibrary(module);
    return exact ? S_OK : HRESULT_FROM_WIN32(ERROR_RESOURCE_NAME_NOT_FOUND);
}

inline HRESULT resolve_binding(const std::wstring& binding, std::wstring* resolved) {
    if (!resolved) return E_POINTER;
    resolved->clear();
    wchar_t text[512]{};
    const HRESULT status = SHLoadIndirectString(binding.c_str(), text,
                                               static_cast<UINT>(std::size(text)), nullptr);
    if (status != S_OK) return status;
    *resolved = text;
    return S_OK;
}

struct FileLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~FileLock() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
} // namespace detail

// Read-only health, not an ownership/migration permit. The caller binds this
// expected path/hash to its validated durable package and separately authorizes
// the exact profile hive/view/key. Never derive this configuration from live raw values.
inline HRESULT verify(const Snapshot& snapshot, const std::wstring& expected_stable_module,
                      const std::string& expected_sha256) {
    try {
        HRESULT status = detail::validate_target(snapshot, expected_stable_module, expected_sha256);
        if (status != S_OK) return status;
        const std::filesystem::path module(expected_stable_module);
        status = detail::validate_module(module);
        if (status != S_OK) return status;
        // Hold the published file against writers/deletion while resolving by path.
        detail::FileLock lock;
        lock.handle = CreateFileW(expected_stable_module.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock.handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        const detail::Operations operations{
            [&] { return detail::validate_module(module); },
            [&](std::string* out) {
                *out = files::detail::sha256_file(module);
                return files::detail::valid_hash(*out) ? S_OK : HRESULT_FROM_WIN32(ERROR_READ_FAULT);
            },
            [&] { return detail::validate_resource_201(expected_stable_module); },
            detail::resolve_binding
        };
        return detail::verify_with_operations(snapshot, expected_stable_module, expected_sha256, operations);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
}
} // namespace boshiamy::installer::profile_display_health
