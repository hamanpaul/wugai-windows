#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

// Test helper: load a supplied DLL without registering the input method.
// A bounded lifetime prevents the test from leaving a persistent process.
int wmain(int argc,wchar_t** argv) {
    if (argc != 2 && argc != 3) return 2;
    HMODULE module=LoadLibraryW(argv[1]);
    if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError() << '\n'; return 1; }
    std::cout << "ready pid=" << GetCurrentProcessId() << std::endl;
    if (argc == 3) {
        std::ofstream ready{std::filesystem::path(argv[2])};
        if (!ready || !(ready << "ready pid=" << GetCurrentProcessId() << '\n')) { FreeLibrary(module); return 3; }
    }
    Sleep(45000);
    FreeLibrary(module);
    return 0;
}
