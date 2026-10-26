#include "../tsf/windows_registration.hpp"
#include <iostream>
#include <stdexcept>
struct Apartment {HRESULT status=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);~Apartment(){if(SUCCEEDED(status))CoUninitialize();}};
static HRESULT validate_service(const std::filesystem::path& dll){
 HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!module)return HRESULT_FROM_WIN32(GetLastError());
 using GetGuid=const GUID*(WINAPI*)();auto clsid=reinterpret_cast<GetGuid>(GetProcAddress(module,"BoshiamyTsf_GetClsid"));auto profile=reinterpret_cast<GetGuid>(GetProcAddress(module,"BoshiamyTsf_GetProfileGuid"));
 HRESULT result=clsid&&profile&&IsEqualGUID(*clsid(),boshiamy::kServiceClsid)&&IsEqualGUID(*profile(),boshiamy::kProfileGuid)?S_OK:E_INVALIDARG;FreeLibrary(module);return result;
}
int wmain(int argc,wchar_t** argv){
 const std::wstring action=argc>1?argv[1]:L"";const bool upgrade=action==L"--upgrade-com";
 if(argc!=(upgrade?4:3)||(!upgrade&&action!=L"--status"&&action!=L"--register"&&action!=L"--unregister"&&action!=L"--enable-user")){std::cerr<<"Usage: LiuKaiRegister --status|--register|--unregister|--enable-user <absolute DLL path> OR --upgrade-com <absolute old DLL> <absolute new DLL>\n";return 2;}
 try{
  std::filesystem::path dll(argv[2]);if(!dll.is_absolute())throw std::runtime_error("DLL path must be absolute");dll=dll.lexically_normal();
  std::filesystem::path replacement;if(upgrade){replacement=argv[3];if(!replacement.is_absolute())throw std::runtime_error("New DLL path must be absolute");replacement=replacement.lexically_normal();}
  Apartment apartment;if(FAILED(apartment.status)){std::cerr<<"COM initialization failed\n";return 1;}
  boshiamy::RegistrationLifecycleLock mutation_lock;
  if(action!=L"--status"){
   const HRESULT locked=mutation_lock.acquire();
   if(locked!=S_OK){std::cerr<<"Shared registration lifecycle lock unavailable: "<<static_cast<unsigned long>(locked)<<"\n";return 1;}
  }
  boshiamy::WindowsRegistration backend(dll);HRESULT result=backend.ready(),cleanup=S_OK;bool validation_failed=false;
  const HRESULT machine_presence=backend.machine_com_presence();
  const HRESULT machine_com=backend.machine_com_status();
  bool lifecycle_required=false;
  if(SUCCEEDED(result)&&(upgrade||action==L"--unregister")&&machine_presence!=S_FALSE){
   result=machine_presence==S_OK?E_PENDING:machine_presence;lifecycle_required=true;
  }
  if(SUCCEEDED(result)&&action==L"--register"&&machine_presence!=S_FALSE&&
     (machine_com!=S_OK||backend.com_status()!=S_OK)){
   result=FAILED(machine_com)?machine_com:E_PENDING;lifecycle_required=true;
  }
  if(SUCCEEDED(result)&&action==L"--register"){result=validate_service(dll);validation_failed=FAILED(result);if(SUCCEEDED(result))result=boshiamy::register_text_service(backend,&cleanup);}
  if(SUCCEEDED(result)&&action==L"--unregister")result=boshiamy::unregister_text_service(backend);
  if(SUCCEEDED(result)&&action==L"--enable-user")result=backend.enable_for_current_user();
  if(SUCCEEDED(result)&&upgrade){result=validate_service(replacement);validation_failed=FAILED(result);if(SUCCEEDED(result)){auto updated=backend.upgrade_com(replacement);result=updated.operation;cleanup=updated.cleanup;}}
  HRESULT com=backend.com_status(),profile=backend.profile_status(),category=backend.category_status(),system_tray_category=backend.system_tray_category_status();
  DWORD profile_caps=0;HRESULT profile_caps_result=backend.profile_caps(&profile_caps);BOOL system_tray_supported=FALSE;HRESULT system_tray_supported_result=backend.system_tray_supported(&system_tray_supported);
  BOOL user_enabled=FALSE;HRESULT user_status=profile==S_OK?backend.user_enabled_status(&user_enabled):S_FALSE;
  const HRESULT immersive=backend.immersive_category_status();
  const HRESULT input_mode=backend.input_mode_category_status();
  std::cout<<"{\"machine_com\":"<<static_cast<unsigned long>(machine_com)<<",\"machine_com_registered\":"<<(machine_com==S_OK?"true":"false")<<",";
  std::cout<<"\"input_mode_category\":"<<static_cast<unsigned long>(input_mode)<<",\"input_mode_supported\":"<<(input_mode==S_OK?"true":"false")<<",";
  std::cout<<"\"immersive_category\":"<<static_cast<unsigned long>(immersive)<<",\"immersive_supported\":"<<(immersive==S_OK?"true":"false")<<",";
  std::cout<<"\"result\":"<<static_cast<unsigned long>(result)<<",\"cleanup_result\":"<<static_cast<unsigned long>(cleanup)<<",\"com\":"<<static_cast<unsigned long>(com)<<",\"profile\":"<<static_cast<unsigned long>(profile)<<",\"keyboard_category\":"<<static_cast<unsigned long>(category)<<",\"registered\":"<<(com==S_OK&&profile==S_OK&&category==S_OK?"true":"false");
  std::cout<<",\"system_tray_category\":"<<static_cast<unsigned long>(system_tray_category)<<",\"profile_caps_result\":"<<static_cast<unsigned long>(profile_caps_result)<<",\"profile_dw_caps\":"<<profile_caps<<",\"system_tray_supported\":"<<(system_tray_supported?"true":"false")<<",\"system_tray_supported_result\":"<<static_cast<unsigned long>(system_tray_supported_result);
  std::cout<<",\"user_enabled\":"<<(user_status==S_OK&&user_enabled?"true":"false")<<",\"user_enabled_result\":"<<static_cast<unsigned long>(user_status);
  std::cout<<",\"failure_stage\":\""<<(lifecycle_required?"machine_com_lifecycle_required":(validation_failed?"validate_service":backend.diagnostics().failure_stage()))<<"\",\"events\":[";
  bool first=true;for(const auto& event:backend.diagnostics().events()){if(!first)std::cout<<",";first=false;std::cout<<"{\"stage\":\""<<event.stage<<"\",\"result\":"<<static_cast<unsigned long>(event.status)<<"}";}std::cout<<"]}\n";
  return FAILED(result)||FAILED(cleanup)||FAILED(machine_com)||FAILED(com)||FAILED(profile)||FAILED(category)||FAILED(system_tray_category)||FAILED(input_mode)||FAILED(profile_caps_result)||FAILED(system_tray_supported_result)||FAILED(user_status)?1:0;
 }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}
