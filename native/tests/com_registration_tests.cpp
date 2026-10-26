#include <windows.h>
#include <objbase.h>
#include <iostream>
#include <string>
#include "../tsf/com_registration.hpp"
#define CHECK(x) do{if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<"\n";return 1;}}while(0)
struct FaultOperations final:boshiamy::RegistryValueOperations{
 int query_calls=0,set_calls=0,fail_query_call=0,fail_set_call=0;
 LONG query(HKEY key,const wchar_t* name,DWORD* type,BYTE* data,DWORD* bytes)override{if(++query_calls==fail_query_call)return ERROR_ACCESS_DENIED;return RegQueryValueExW(key,name,nullptr,type,data,bytes);}
 LONG set(HKEY key,const wchar_t* name,DWORD type,const BYTE* data,DWORD bytes)override{if(++set_calls==fail_set_call)return ERROR_ACCESS_DENIED;return RegSetValueExW(key,name,0,type,data,bytes);}
};
static int machine_identity_tests(HKEY scope){
 GUID id{};CHECK(SUCCEEDED(CoCreateGuid(&id)));
 const std::wstring sid=L"S-1-5-21-1-2-3-1001",root=L"C:\\LiuKaiManaged";
 boshiamy::ComServerRegistration machine(scope,id,L"C:\\LiuKaiManaged\\versions\\v2\\x64\\BoshiamyTsf.dll");
 CHECK(machine.matches_machine_identity(sid,root)==S_FALSE);
 CHECK(machine.add()==S_OK);
 CHECK(FAILED(machine.matches_machine_identity(sid,root))); // no SID/root claim yet
 CHECK(machine.matches_machine_identity(L"",root)==E_INVALIDARG);
 CHECK(machine.matches_machine_identity(sid,L"relative")==E_INVALIDARG);
 {
  boshiamy::RegistryKey key;
  CHECK(RegOpenKeyExW(scope,machine.key_path().c_str(),0,KEY_READ|KEY_WRITE,key.put())==ERROR_SUCCESS);
  auto text=[&](const wchar_t* name,const std::wstring& value){return RegSetValueExW(key.get(),name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;};
  CHECK(text(L"LiuKaiInstallSid",sid));CHECK(text(L"LiuKaiInstallRoot",root));
  CHECK(machine.matches_machine_identity(sid,root)==S_OK);
  CHECK(FAILED(machine.matches_machine_identity(L"S-1-5-21-1-2-3-1002",root)));
  CHECK(FAILED(machine.matches_machine_identity(sid,L"D:\\OtherManaged")));
  DWORD wrong=1;CHECK(RegSetValueExW(key.get(),L"LiuKaiInstallSid",0,REG_DWORD,reinterpret_cast<const BYTE*>(&wrong),sizeof(wrong))==ERROR_SUCCESS);
  CHECK(FAILED(machine.matches_machine_identity(sid,root)));
  CHECK(text(L"LiuKaiInstallSid",sid));
  const wchar_t unterminated[]={L'x'};
  CHECK(RegSetValueExW(key.get(),L"LiuKaiInstallRoot",0,REG_SZ,reinterpret_cast<const BYTE*>(unterminated),sizeof(unterminated))==ERROR_SUCCESS);
  CHECK(FAILED(machine.matches_machine_identity(sid,root)));
  CHECK(text(L"LiuKaiInstallRoot",root));
  CHECK(machine.matches_machine_identity(sid,root)==S_OK);
  CHECK(machine.matches()==S_OK); // independent readback never changes the registration
 }
 CHECK(machine.remove()==S_OK);CHECK(machine.matches_machine_identity(sid,root)==S_FALSE);
 return 0;
}
int main(){GUID id{};CHECK(SUCCEEDED(CoCreateGuid(&id)));wchar_t name[40]{};CHECK(StringFromGUID2(id,name,40)>0);std::wstring area=L"Software\\LiuKai\\Tests\\"+std::wstring(name);HKEY scope=nullptr;DWORD disposition=0;CHECK(RegCreateKeyExW(HKEY_CURRENT_USER,area.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&scope,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY);
 struct Cleanup{HKEY key;std::wstring area;~Cleanup(){RegCloseKey(key);RegDeleteTreeW(HKEY_CURRENT_USER,area.c_str());}} cleanup{scope,area};
 CHECK(machine_identity_tests(scope)==0);
 boshiamy::ComServerRegistration first(scope,id,L"C:\\LiuKai\\BoshiamyTsf.dll");CHECK(first.remove()==S_FALSE);CHECK(first.add()==S_OK);CHECK(first.matches()==S_OK);CHECK(first.add()==S_FALSE);
 boshiamy::ComServerRegistration conflict(scope,id,L"C:\\Other\\BoshiamyTsf.dll");CHECK(conflict.add()==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));CHECK(conflict.remove()==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));CHECK(first.matches()==S_OK);
 CHECK(first.remove()==S_OK);CHECK(first.matches()==S_FALSE);CHECK(first.remove()==S_FALSE);
 HKEY unrelated=nullptr;CHECK(RegCreateKeyExW(scope,first.key_path().c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&unrelated,&disposition)==ERROR_SUCCESS);const wchar_t sentinel[]=L"other owner";CHECK(RegSetValueExW(unrelated,L"LiuKaiOwner",0,REG_SZ,(const BYTE*)sentinel,sizeof(sentinel))==ERROR_SUCCESS);RegCloseKey(unrelated);CHECK(first.add()==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));CHECK(first.remove()==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));CHECK(RegOpenKeyExW(scope,first.key_path().c_str(),0,KEY_READ,&unrelated)==ERROR_SUCCESS);RegCloseKey(unrelated);
 auto rejected_owner_upgrade=first.upgrade_to(L"C:\\LiuKai\\new.dll");CHECK(rejected_owner_upgrade.operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));CHECK(rejected_owner_upgrade.cleanup==S_OK);
 GUID upgrade_id{};CHECK(SUCCEEDED(CoCreateGuid(&upgrade_id)));
 boshiamy::ComServerRegistration old_version(scope,upgrade_id,L"C:\\LiuKai\\old\\BoshiamyTsf.dll");CHECK(old_version.add()==S_OK);
 HKEY class_key=nullptr;CHECK(RegOpenKeyExW(scope,old_version.key_path().c_str(),0,KEY_READ|KEY_WRITE,&class_key)==ERROR_SUCCESS);
 const wchar_t class_marker[]=L"keep-class-setting";CHECK(RegSetValueExW(class_key,L"CustomClassValue",0,REG_SZ,(const BYTE*)class_marker,sizeof(class_marker))==ERROR_SUCCESS);
 HKEY server_key=nullptr;CHECK(RegOpenKeyExW(class_key,L"InprocServer32",0,KEY_READ|KEY_WRITE,&server_key)==ERROR_SUCCESS);
 const wchar_t server_marker[]=L"keep-server-setting";CHECK(RegSetValueExW(server_key,L"VendorData",0,REG_SZ,(const BYTE*)server_marker,sizeof(server_marker))==ERROR_SUCCESS);RegCloseKey(server_key);RegCloseKey(class_key);
 const std::wstring new_dll=L"D:\\LiuKai\\versions\\v2\\BoshiamyTsf.dll";
 auto upgraded=old_version.upgrade_to(new_dll);CHECK(upgraded.operation==S_OK&&upgraded.cleanup==S_OK);
 boshiamy::ComServerRegistration new_version(scope,upgrade_id,new_dll);CHECK(new_version.matches()==S_OK);
 CHECK(old_version.matches()==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
 CHECK(new_version.upgrade_to(new_dll).operation==S_FALSE);
 CHECK(old_version.upgrade_to(L"E:\\LiuKai\\stale.dll").operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
 boshiamy::ComServerRegistration wrong_old(scope,upgrade_id,L"C:\\LiuKai\\wrong.dll");CHECK(wrong_old.upgrade_to(L"E:\\LiuKai\\other.dll").operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
 CHECK(new_version.upgrade_to(L"relative\\BoshiamyTsf.dll").operation==E_INVALIDARG);
 CHECK(new_version.upgrade_to(L"").operation==E_INVALIDARG);
 std::wstring embedded_nul=L"E:\\LiuKai\\visible.dll";embedded_nul.push_back(L'\0');embedded_nul+=L"hidden.dll";CHECK(new_version.upgrade_to(embedded_nul).operation==E_INVALIDARG);
 CHECK(new_version.matches()==S_OK);
 CHECK(RegOpenKeyExW(scope,new_version.key_path().c_str(),0,KEY_READ,&class_key)==ERROR_SUCCESS);
 wchar_t readback[64]{};DWORD bytes=sizeof(readback),type=0;CHECK(RegQueryValueExW(class_key,L"CustomClassValue",nullptr,&type,(BYTE*)readback,&bytes)==ERROR_SUCCESS&&type==REG_SZ&&std::wstring(readback)==class_marker);
 CHECK(RegOpenKeyExW(class_key,L"InprocServer32",0,KEY_READ,&server_key)==ERROR_SUCCESS);ZeroMemory(readback,sizeof(readback));bytes=sizeof(readback);CHECK(RegQueryValueExW(server_key,L"VendorData",nullptr,&type,(BYTE*)readback,&bytes)==ERROR_SUCCESS&&type==REG_SZ&&std::wstring(readback)==server_marker);RegCloseKey(server_key);RegCloseKey(class_key);
 GUID missing_id{};CHECK(SUCCEEDED(CoCreateGuid(&missing_id)));boshiamy::ComServerRegistration missing(scope,missing_id,L"C:\\LiuKai\\old.dll");CHECK(missing.upgrade_to(L"D:\\LiuKai\\new.dll").operation==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
 GUID failure_id{};CHECK(SUCCEEDED(CoCreateGuid(&failure_id)));boshiamy::ComServerRegistration failure_old(scope,failure_id,L"C:\\LiuKai\\failure\\old.dll");CHECK(failure_old.add()==S_OK);
 FaultOperations write_failure;write_failure.fail_set_call=1;auto failed=failure_old.upgrade_to(L"D:\\LiuKai\\failure\\new.dll",write_failure);CHECK(failed.operation==HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)&&failed.cleanup==S_OK);CHECK(failure_old.matches()==S_OK);
 FaultOperations rollback_failure;rollback_failure.fail_query_call=3;rollback_failure.fail_set_call=2;failed=failure_old.upgrade_to(L"D:\\LiuKai\\failure\\new.dll",rollback_failure);CHECK(failed.operation==HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)&&failed.cleanup==HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
 boshiamy::ComServerRegistration failure_new(scope,failure_id,L"D:\\LiuKai\\failure\\new.dll");CHECK(failure_new.matches()==S_OK);CHECK(failure_new.upgrade_to(L"C:\\LiuKai\\failure\\old.dll").operation==S_OK);
 std::cout<<"COM registration ownership, isolated upgrade, and preservation passed\n";
}
