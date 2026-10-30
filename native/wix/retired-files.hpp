#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include "../installer/file_deployment.hpp"

// The caller supplies paths constructed from the canonical Program Files root
// and an unguessable transaction token. Retired names are never reused as
// installation paths, including after a reboot has been requested.
namespace liukai::retired_files {
namespace fs = std::filesystem;
using boshiamy::installer::files::detail::absolute_no_reparse;
using boshiamy::installer::files::detail::sha256_file;

inline bool absent(const fs::path& p) {
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
inline bool verified(const fs::path& p, const std::string& hash) {
    return absolute_no_reparse(p, true) && sha256_file(p) == hash;
}
inline HRESULT retire(const fs::path& original, const fs::path& retired, const std::string& hash) {
    if (!verified(original, hash) || !absent(retired)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (!MoveFileExW(original.c_str(), retired.c_str(), 0)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
inline HRESULT restore(const fs::path& original, const fs::path& retired, const std::string& hash) {
    if (absent(retired)) return verified(original, hash) ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (!verified(retired, hash) || !absent(original)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (!MoveFileExW(retired.c_str(), original.c_str(), 0)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
// Call only after the MSI installation script has succeeded. The scheduled
// name is transaction-specific and cannot name a future installed file.
inline HRESULT schedule_retired(const fs::path& retired, const std::string& hash) {
    if (!verified(retired, hash)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (!MoveFileExW(retired.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
}
