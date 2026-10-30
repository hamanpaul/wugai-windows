#include "../wix/retired-files.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    using namespace liukai::retired_files;
    if (argc != 2) return 2;
    const fs::path dir(argv[1]), original=dir/L"original.dll", retired=dir/L"retired.dll",
        second=dir/L"second.dll", secondRetired=dir/L"second-retired.dll";
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH) || !CreateDirectoryW(dir.c_str(), nullptr) ||
        !CopyFileW((fs::path(system)/L"version.dll").c_str(), original.c_str(), TRUE)) return 3;
    const auto hash=sha256_file(original);
    HMODULE module=LoadLibraryExW(original.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    bool ok=module && !hash.empty();
    if (ok) ok = FAILED(retire(original, retired, std::string(64, '0'))) && verified(original,hash) && absent(retired);
    if (ok) ok = retire(original, retired, hash)==S_OK && absent(original) && verified(retired,hash);
    if (ok) ok = FAILED(retire(original, retired, hash));
    if (ok) ok = restore(original, retired, hash)==S_OK && verified(original,hash) && absent(retired);
    if (ok) ok = restore(original, retired, hash)==S_OK;
    // An unrelated destination must never be overwritten.
    if (ok) ok = CopyFileW(original.c_str(),retired.c_str(),TRUE)!=FALSE;
    if (ok) ok = FAILED(retire(original,retired,hash)) && verified(original,hash) && verified(retired,hash);
    if (ok) ok = DeleteFileW(retired.c_str())!=FALSE;
    // Simulate failure after the first of the two file moves.
    if (ok) ok = CopyFileW(original.c_str(),second.c_str(),TRUE)!=FALSE;
    if (ok) ok = retire(original,retired,hash)==S_OK;
    if (ok) ok = FAILED(retire(second,secondRetired,std::string(64,'0')));
    if (ok) ok = restore(second,secondRetired,hash)==S_OK && restore(original,retired,hash)==S_OK;
    if (module) FreeLibrary(module);
    DeleteFileW(original.c_str()); DeleteFileW(retired.c_str()); DeleteFileW(second.c_str());
    DeleteFileW(secondRetired.c_str()); RemoveDirectoryW(dir.c_str());
    if (!ok) { std::cerr << "retired file test failed\n"; return 1; }
    std::cout << "retired file checks passed\n";
    return 0;
}
