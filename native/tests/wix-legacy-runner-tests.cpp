#include "../wix/legacy-recovery-runner.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){
 if(argc!=7)return 1;auto narrow=[](const wchar_t* p){std::wstring w(p);return std::string(w.begin(),w.end());};
 const std::string proof=narrow(argv[2]),version=narrow(argv[3]),manifest=narrow(argv[4]),helper=narrow(argv[5]);
 std::map<std::string,std::string> parsed;if(!liukai::wix_legacy_recovery::decode_proof(proof,parsed))return 2;
 if(liukai::wix_legacy_recovery::decode_proof(proof+";support/foreign="+std::string(64,'a'),parsed))return 3;
 {liukai::wix_legacy_recovery::LockedFiles locks;if(!liukai::wix_legacy_recovery::verify_helpers(argv[1],version,manifest,helper,proof,locks))return 4;}
 {liukai::wix_legacy_recovery::LockedFiles locks;if(liukai::wix_legacy_recovery::verify_helpers(argv[1],version,manifest,std::string(64,'0'),proof,locks))return 5;}
 const auto logDir=std::filesystem::path(argv[1])/L"test-logs";std::wstring report;
 DWORD code=liukai::wix_legacy_recovery::run_script(argv[1],L"test-sid",L"test-v1",manifest,helper,std::string(64,'a'),proof,logDir,&report);
 if(code!=0||report.empty())return 6;
 std::ifstream log(std::filesystem::path(report),std::ios::binary);std::string text((std::istreambuf_iterator<char>(log)),std::istreambuf_iterator<char>());
 if(text.find("recovery-fixture-ok")==std::string::npos)return 7;
 HKEY sandbox=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,argv[6],0,KEY_ALL_ACCESS|KEY_WOW64_64KEY,&sandbox)!=ERROR_SUCCESS)return 8;
 bool found=true;if(liukai::wix_legacy_recovery::orphan_present(sandbox,found)!=S_OK||found){RegCloseKey(sandbox);return 9;}
 HKEY key=nullptr;const wchar_t* profile=L"Software\\Microsoft\\CTF\\TIP\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\LanguageProfile\\0x00000404\\{7254A21F-6EB1-4217-A42C-C27301175D90}";
 const wchar_t* marker=L"Software\\LiuKai\\Installer\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\Profile";
 if(RegCreateKeyExW(sandbox,profile,0,nullptr,0,KEY_ALL_ACCESS|KEY_WOW64_64KEY,nullptr,&key,nullptr)!=ERROR_SUCCESS){RegCloseKey(sandbox);return 10;}RegCloseKey(key);
 if(liukai::wix_legacy_recovery::orphan_present(sandbox,found)!=HRESULT_FROM_WIN32(ERROR_INVALID_STATE)){RegCloseKey(sandbox);return 11;}
 if(RegCreateKeyExW(sandbox,marker,0,nullptr,0,KEY_ALL_ACCESS|KEY_WOW64_64KEY,nullptr,&key,nullptr)!=ERROR_SUCCESS){RegCloseKey(sandbox);return 12;}RegCloseKey(key);
 if(liukai::wix_legacy_recovery::orphan_present(sandbox,found)!=S_OK||!found){RegCloseKey(sandbox);return 13;}RegCloseKey(sandbox);
 std::cout<<"legacy_recovery_runner_tests_passed\n";return 0;
}