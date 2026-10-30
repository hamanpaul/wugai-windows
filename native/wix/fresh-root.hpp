#pragma once
#include <windows.h>
#include <filesystem>
namespace boshiamy::installer::msi_registration {
inline std::filesystem::path normalized_root(const std::wstring& value) {
    if(value.empty()||value.find(L'\0')!=std::wstring::npos)return {};
    auto result=std::filesystem::path(value).lexically_normal();
    while(result.has_relative_path()&&result.filename().empty())result=result.parent_path();
    return result;
}
// A fresh MSI must not adopt an existing directory, file or reparse point.
// This is a preflight check, not a lock against a concurrent external writer.
inline HRESULT require_absent_root(const std::filesystem::path& root) {
    if(!root.is_absolute()||root.empty()||root!=root.lexically_normal()||root.filename().empty())return E_INVALIDARG;
    const DWORD attrs=GetFileAttributesW(root.c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    const DWORD error=GetLastError();
    return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?S_OK:HRESULT_FROM_WIN32(error);
}
}
