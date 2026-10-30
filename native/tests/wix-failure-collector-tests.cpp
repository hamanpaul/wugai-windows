#include "../wix/failure-collector.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){
 using namespace liukai::wix_diagnostics;
 if(quote_argument(L"C:\\space here\\")!=L"\"C:\\space here\\\\\"")return 1;
 std::wstring sid,root,log;
 if(!parse_data(L"S-1-5-21|C:\\Program Files\\LiuKai|C:\\Temp\\a.log",sid,root,log)||sid!=L"S-1-5-21"||log!=L"C:\\Temp\\a.log")return 2;
 if(parse_data(L"bad|data|too|many",sid,root,log))return 3;
 if(argc!=5)return 4;
 const std::wstring hashW=argv[4];const std::string hash(hashW.begin(),hashW.end());
 DWORD code=run(argv[1],argv[2],argv[3],hash);std::cout<<"collector_exit="<<code<<"\n";return code;
}