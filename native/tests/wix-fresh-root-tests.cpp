#include "../wix/fresh-root.hpp"
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;namespace m=boshiamy::installer::msi_registration;
#define CHECK(x) do{if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<'\n';return 1;}}while(0)
int main(){
    auto root=fs::absolute(fs::path(L"build")/L"tests"/(L"wix-root-"+std::to_wstring(GetCurrentProcessId())));
    CHECK(m::normalized_root(root.wstring()+L"\\")==root);
    CHECK(m::require_absent_root(root)==S_OK);
    CHECK(fs::create_directory(root));
    CHECK(m::require_absent_root(root)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(fs::remove(root));
    {std::ofstream file(root);CHECK(file.good());file<<"preserve";}
    CHECK(m::require_absent_root(root)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(fs::file_size(root)==8);CHECK(fs::remove(root));
    CHECK(m::require_absent_root(fs::path(L"relative"))==E_INVALIDARG);
    std::cout<<"fresh_root_preflight_tests=passed\n";
}
