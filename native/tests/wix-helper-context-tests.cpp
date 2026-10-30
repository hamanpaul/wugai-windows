#include "../installer/independent_categories.hpp"
#include <iostream>
using namespace boshiamy::installer::independent_categories;
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    if(helper_executable_override)return 1;
    {
        ScopedHelperExecutable scoped(argv[1]);
        if(!helper_executable_override||std::wstring(helper_executable_override)!=argv[1])return 1;
        std::vector<GUID> categories;
        const HRESULT hr=read_fresh(&categories);
        if(hr!=S_OK){std::wcerr<<L"independent category query failed: "<<std::hex<<static_cast<unsigned long>(hr)<<L"\n";return 1;}
        {ScopedHelperExecutable nested(L"test nested");if(!helper_executable_override||std::wstring(helper_executable_override)!=L"test nested")return 1;}
        if(!helper_executable_override||std::wstring(helper_executable_override)!=argv[1])return 1;
    }
    if(helper_executable_override)return 1;
    std::cout<<"explicit_helper_query=passed\n";
    return 0;
}
