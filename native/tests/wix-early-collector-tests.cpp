#include "../wix/early-failure-collector.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){
 if(argc!=7)return 2;MSIHANDLE db=0;UINT status=MsiOpenDatabaseW(argv[1],MSIDBOPEN_READONLY,&db);if(status!=ERROR_SUCCESS)return status;
 const std::wstring hw=argv[6];const std::string hash(hw.begin(),hw.end());
 DWORD result=liukai::wix_diagnostics::run_binary(db,argv[2],argv[3],argv[4],argv[5],hash,L"prepare-root",L"2147942487");MsiCloseHandle(db);
 std::cout<<"early_collector_exit="<<result<<"\n";return result;
}