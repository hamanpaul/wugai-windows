#include "lookup_window.hpp"
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <vector>
using namespace boshiamy;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        MessageBoxW(nullptr, L"無法解析命令列參數。", L"嘸蝦米查碼", MB_OK | MB_ICONERROR);
        return 2;
    }
    struct ArgvGuard {
        LPWSTR* value;
        ~ArgvGuard() { if (value) LocalFree(value); }
    } argv_guard{argv};

    try {
        std::filesystem::path path;
        if (argc > 1) {
            path = argv[1];
        } else {
            std::vector<wchar_t> executable(32768);
            DWORD length = GetModuleFileNameW(instance, executable.data(), static_cast<DWORD>(executable.size()));
            if (!length || length >= executable.size())
                throw std::runtime_error("could not locate executable directory");
            path = std::filesystem::path(std::wstring(executable.data(), length)).parent_path() / L"dictionary.tsv";
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            const std::wstring message = L"找不到字表：\n" + path.wstring();
            MessageBoxW(nullptr, message.c_str(), L"嘸蝦米查碼", MB_OK | MB_ICONERROR);
            return 2;
        }

        Dictionary dictionary = load_dictionary(input);
        if (!dictionary.entries) throw std::runtime_error("dictionary contains no entries");
        RecommendedCodes recommendations;
        std::ifstream metadata(path.parent_path()/L"recommended-codes.tsv",std::ios::binary);
        if(metadata){try{recommendations=RecommendedCodes::parse(metadata);}catch(...){/* No inferred recommended markers on missing/broken metadata. */}}
        LookupWindow window(instance, dictionary, {}, [recommendations](const std::string& text,Mode mode){return recommendations.get(text,mode);}, [] { PostQuitMessage(0); });
        if (!window.create()) {
            MessageBoxW(nullptr, L"查碼視窗建立失敗。", L"嘸蝦米查碼", MB_OK | MB_ICONERROR);
            return 3;
        }
        window.show();

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        const std::string value = error.what();
        int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring message(static_cast<std::size_t>(length), L'\0');
        if (length)
            MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), message.data(), length);
        MessageBoxW(nullptr, message.c_str(), L"嘸蝦米查碼：字表錯誤", MB_OK | MB_ICONERROR);
        return 2;
    }
}
