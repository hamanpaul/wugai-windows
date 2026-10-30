#include <windows.h>
#include <string>
#ifdef LIUKAI_TEST_OLD_TLS
thread_local std::wstring old_value;
extern "C" __declspec(dllexport) int TestScope(){old_value=L"test";old_value.clear();return 1;}
#else
#include "../installer/independent_categories.hpp"
extern "C" __declspec(dllexport) int TestScope(){boshiamy::installer::independent_categories::ScopedHelperExecutable scope(L"test");return 1;}
#endif
