#ifndef LIUKAI_WINDOWS_REGISTRATION_HPP
#define LIUKAI_WINDOWS_REGISTRATION_HPP
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <sddl.h>
#include <filesystem>
#include <string>
#include "registration_plan.hpp"
#include "com_registration.hpp"
#include "registration_diagnostics.hpp"
#include "../user_profile_enable.hpp"
namespace boshiamy {
  inline const CLSID kServiceClsid={0x5d7c4291,0x2b34,0x4ce8,{0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
  inline const GUID kProfileGuid={0x7254a21f,0x6eb1,0x4217,{0xa4,0x2c,0xc2,0x73,0x01,0x17,0x5d,0x90}};
  // MinGW's bundled msctf.h omits this SDK symbol. Microsoft documents the
  // symbolic category and declares it in Msctf.h; this GUID value is mirrored
  // from Mozc's TSF registrar compatibility definition.
  inline const GUID kTipCapabilitySysTray={0x25504fb4,0x7bab,0x4bc1,{0x9c,0x69,0xcf,0x81,0x89,0x0f,0x0e,0xf5}};
  // Microsoft SDK GUID_TFCAT_TIPCAP_UIELEMENTENABLED; absent from bundled MinGW.
  inline const GUID kTipCapabilityUiElement={0x49d2f9cf,0x1f5e,0x11d7,{0xa6,0xd3,0x00,0x06,0x5b,0x84,0x43,0x5c}};
  // GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, required for Windows Store app hosts.
  inline const GUID kTipCapabilityImmersive={0x13a016df,0x560b,0x46cd,{0x94,0x7a,0x4c,0x3a,0xf1,0xe0,0xe3,0x5d}};
  // SDK GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT (Microsoft windows-rs 0.58.0).
  inline const GUID kTipCapabilityInputMode={0xccf05dd7,0x4a87,0x11d7,{0xa6,0xe2,0x00,0x06,0x5b,0x84,0x43,0x5c}};
  inline constexpr DWORD kProfileCapsSysTraySupport=0x00020000;
  inline constexpr DWORD kProfileCapsUiElementEnabled=0x00000004;
  inline constexpr LANGID kProfileLanguage=0x0404;
  template<class T> class ComReference {
    T* pointer_=nullptr;
    public:~ComReference(){if(pointer_)pointer_->Release();
    }T* get()const{return pointer_;
    }T** put(){return &pointer_;
    }T* operator->()const{return pointer_;
    }
    ComReference()=default;
    ComReference(const ComReference&)=delete;
    ComReference& operator=(const ComReference&)=delete;
  };
  // Standalone helper mutations share the lifecycle writer lock with package
  // deployment, recovery and profile metadata repair. A presence guard alone is
  // insufficient: migration could otherwise create machine COM before a legacy
  // command changes the user half. The Windows mutex is reentrant on one thread.
  class RegistrationLifecycleLock {
    HANDLE handle_=nullptr;
    bool owned_=false;
    public:
    RegistrationLifecycleLock()=default;
    RegistrationLifecycleLock(const RegistrationLifecycleLock&)=delete;
    RegistrationLifecycleLock& operator=(const RegistrationLifecycleLock&)=delete;
    ~RegistrationLifecycleLock(){if(owned_)ReleaseMutex(handle_);
      if(handle_)CloseHandle(handle_);
    }
    HRESULT acquire(){
      if(handle_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
      handle_=CreateMutexW(nullptr,FALSE,L"Global\\LiuKai-7254A21F-ProfileDisplayMetadata");
      if(!handle_)return HRESULT_FROM_WIN32(GetLastError());
      const DWORD status=WaitForSingleObject(handle_,0);
      if(status==WAIT_ABANDONED){owned_=true;
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
      }
      if(status==WAIT_OBJECT_0){owned_=true;
        return S_OK;
      }
      if(status==WAIT_TIMEOUT)return HRESULT_FROM_WIN32(ERROR_BUSY);
      const DWORD error=GetLastError();
      return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
    }
  };
  class WindowsRegistration final:public RegistrationBackend {
    std::filesystem::path dll_;
    ComServerRegistration com_;
    ComReference<ITfInputProcessorProfileMgr> profiles_;
    ComReference<ITfCategoryMgr> categories_;
    HRESULT ready_=E_UNEXPECTED;
    RegistrationDiagnostics diagnostics_;
    public:
    explicit WindowsRegistration(std::filesystem::path dll):dll_(std::move(dll)),com_(HKEY_CURRENT_USER,kServiceClsid,dll_.wstring()){
      ready_=diagnostics_.record("create.profile_manager",CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(profiles_.put())));
      if(SUCCEEDED(ready_))ready_=diagnostics_.record("create.category_manager",CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfCategoryMgr,reinterpret_cast<void**>(categories_.put())));
    }
    const RegistrationDiagnostics& diagnostics()const{return diagnostics_;
    }
    HRESULT ready()const{return ready_;
    }
    // Presence is intentionally independent of ownership: a legacy command
    // may not erase or diverge a machine registration belonging to any account.
    HRESULT machine_com_presence()const{
      RegistryKey key;
      LONG status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,com_.key_path().c_str(),0,KEY_READ|KEY_WOW64_64KEY,key.put());
      if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)return S_FALSE;
      return status==ERROR_SUCCESS?S_OK:HRESULT_FROM_WIN32(status);
    }
    HRESULT machine_com_status()const{
      HRESULT present=machine_com_presence();
      if(present!=S_OK)return present;
      const auto architecture=dll_.parent_path();
      const auto versions=architecture.parent_path().parent_path();
      if(_wcsicmp(dll_.filename().c_str(),L"BoshiamyTsf.dll")!=0||
      _wcsicmp(architecture.filename().c_str(),L"x64")!=0||
      _wcsicmp(versions.filename().c_str(),L"versions")!=0)return E_INVALIDARG;
      struct TokenHandle{HANDLE value=nullptr;
        ~TokenHandle(){if(value)CloseHandle(value);
      }} token;
      if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return HRESULT_FROM_WIN32(GetLastError());
      DWORD bytes=0;
      GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);
      if(!bytes)return HRESULT_FROM_WIN32(GetLastError());
      std::vector<BYTE> data(bytes);
      if(!GetTokenInformation(token.value,TokenUser,data.data(),bytes,&bytes))return HRESULT_FROM_WIN32(GetLastError());
      struct SidText{LPWSTR value=nullptr;
        ~SidText(){if(value)LocalFree(value);
      }} sid;
      if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid.value))return HRESULT_FROM_WIN32(GetLastError());
      ComServerRegistration machine(HKEY_LOCAL_MACHINE,kServiceClsid,dll_.wstring());
      return machine.matches_machine_identity(sid.value,versions.parent_path().lexically_normal().make_preferred().wstring());
    }
    ComServerRegistration::UpgradeResult upgrade_com(const std::filesystem::path& replacement){
      RegistrationLifecycleLock lock;
      HRESULT locked=lock.acquire();
      if(locked!=S_OK)return {locked,S_OK};
      HRESULT machine=machine_com_presence();
      if(machine!=S_FALSE)return {machine==S_OK?E_PENDING:machine,S_OK};
      auto result=com_.upgrade_to(replacement.wstring());
      diagnostics_.record("com.upgrade",result.operation);
      diagnostics_.record("com.upgrade.cleanup",result.cleanup);
      if(SUCCEEDED(result.operation)&&SUCCEEDED(result.cleanup)){dll_=replacement;
        com_=ComServerRegistration(HKEY_CURRENT_USER,kServiceClsid,dll_.wstring());
      }
      return result;
    }
    HRESULT user_enabled_status(BOOL* enabled){
      if(!enabled)return E_POINTER;
      *enabled=FALSE;
      if(FAILED(ready_))return ready_;
      ComReference<ITfInputProcessorProfiles> manager;
      HRESULT hr=diagnostics_.record("query.legacy_profiles",profiles_->QueryInterface(IID_ITfInputProcessorProfiles,reinterpret_cast<void**>(manager.put())));
      if(FAILED(hr))return hr;
      return diagnostics_.record("IsEnabledLanguageProfile",manager->IsEnabledLanguageProfile(kServiceClsid,kProfileLanguage,kProfileGuid,enabled));
    }
    HRESULT enable_for_current_user(){
      HRESULT ownership=com_status();
      if(ownership!=S_OK)return FAILED(ownership)?ownership:E_PENDING;
      HRESULT registered=profile_status();
      if(registered!=S_OK)return FAILED(registered)?registered:E_PENDING;
      ComReference<ITfInputProcessorProfiles> manager;
      HRESULT hr=diagnostics_.record("query.legacy_profiles",profiles_->QueryInterface(IID_ITfInputProcessorProfiles,reinterpret_cast<void**>(manager.put())));
      if(FAILED(hr))return hr;
      struct LoggedManager {
        ITfInputProcessorProfiles* manager;
        RegistrationDiagnostics& diagnostics;
        HRESULT IsEnabledLanguageProfile(REFCLSID clsid,LANGID language,REFGUID profile,BOOL* enabled){return diagnostics.record("IsEnabledLanguageProfile",manager->IsEnabledLanguageProfile(clsid,language,profile,enabled));
        }
        HRESULT EnableLanguageProfile(REFCLSID clsid,LANGID language,REFGUID profile,BOOL enabled){return diagnostics.record("EnableLanguageProfile",manager->EnableLanguageProfile(clsid,language,profile,enabled));
        }
      } logged{manager.get(),diagnostics_};
      return user_profile::ensure_enabled_for_current_user(logged,kServiceClsid,kProfileLanguage,kProfileGuid);
    }
    HRESULT com_status(){return diagnostics_.record("com.status",com_.matches());
    }
    HRESULT profile_status(){
      if(FAILED(ready_))return ready_;
      ComReference<IEnumTfInputProcessorProfiles> enumeration;
      HRESULT hr=diagnostics_.record("EnumProfiles",profiles_->EnumProfiles(0,enumeration.put()));
      if(FAILED(hr))return hr;
      for(;;){TF_INPUTPROCESSORPROFILE row{};
        ULONG count=0;
        hr=enumeration->Next(1,&row,&count);
        if(FAILED(hr))return diagnostics_.record("profile.Next",hr);
        if(!count)return S_FALSE;
        if(row.dwProfileType==TF_PROFILETYPE_INPUTPROCESSOR&&row.langid==kProfileLanguage&&IsEqualCLSID(row.clsid,kServiceClsid)&&IsEqualGUID(row.guidProfile,kProfileGuid))return S_OK;
      }
    }
    HRESULT profile_caps(DWORD* caps){
      if(!caps)return E_POINTER;
      *caps=0;
      if(FAILED(ready_))return ready_;
      TF_INPUTPROCESSORPROFILE profile{};
      HRESULT hr=diagnostics_.record("GetProfile",profiles_->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR,kProfileLanguage,kServiceClsid,kProfileGuid,nullptr,&profile));
      if(SUCCEEDED(hr))*caps=profile.dwCaps;
      return hr;
    }
    HRESULT category_status(){
      if(FAILED(ready_))return ready_;
      ComReference<IEnumGUID> enumeration;
      HRESULT hr=diagnostics_.record("EnumCategoriesInItem",categories_->EnumCategoriesInItem(kServiceClsid,enumeration.put()));
      if(FAILED(hr))return hr;
      for(;;){GUID id{};
        ULONG count=0;
        hr=enumeration->Next(1,&id,&count);
        if(FAILED(hr))return diagnostics_.record("category.Next",hr);
        if(!count)return S_FALSE;
        if(IsEqualGUID(id,GUID_TFCAT_TIP_KEYBOARD))return S_OK;
      }
    }
    HRESULT system_tray_category_status(){
      if(FAILED(ready_))return ready_;
      ComReference<IEnumGUID> enumeration;
      HRESULT hr=diagnostics_.record("EnumCategoriesInItem.systray",categories_->EnumCategoriesInItem(kServiceClsid,enumeration.put()));
      if(FAILED(hr))return hr;
      for(;;){GUID id{};
        ULONG count=0;
        hr=enumeration->Next(1,&id,&count);
        if(FAILED(hr))return diagnostics_.record("systray_category.Next",hr);
        if(!count)return S_FALSE;
        if(IsEqualGUID(id,kTipCapabilitySysTray))return S_OK;
      }
    }
    HRESULT ui_element_category_status(){
      if(FAILED(ready_))return ready_;
      ComReference<IEnumGUID> enumeration;
      HRESULT hr=diagnostics_.record("EnumCategoriesInItem.ui_element",categories_->EnumCategoriesInItem(kServiceClsid,enumeration.put()));
      if(FAILED(hr))return hr;
      for(;;){GUID id{};
        ULONG count=0;
        hr=enumeration->Next(1,&id,&count);
        if(FAILED(hr))return diagnostics_.record("ui_element_category.Next",hr);
        if(!count)return S_FALSE;
        if(IsEqualGUID(id,kTipCapabilityUiElement))return S_OK;
      }
    }
    HRESULT system_tray_supported(BOOL* supported){
      if(!supported)return E_POINTER;
      *supported=FALSE;
      DWORD caps=0;
      HRESULT profile=profile_caps(&caps);
      if(FAILED(profile))return profile;
      HRESULT category=system_tray_category_status();
      if(FAILED(category))return category;
      *supported=(category==S_OK&&(caps&kProfileCapsSysTraySupport)!=0)?TRUE:FALSE;
      return category==S_OK?S_OK:S_FALSE;
    }
    HRESULT immersive_category_status(){
      if(FAILED(ready_))return ready_;
      ComReference<IEnumGUID> enumeration;
      HRESULT hr=diagnostics_.record("EnumCategoriesInItem.immersive",categories_->EnumCategoriesInItem(kServiceClsid,enumeration.put()));
      if(FAILED(hr))return hr;
      for(;;){GUID id{};
        ULONG count=0;
        hr=enumeration->Next(1,&id,&count);
        if(FAILED(hr))return diagnostics_.record("immersive_category.Next",hr);
        if(!count)return S_FALSE;
        if(IsEqualGUID(id,kTipCapabilityImmersive))return S_OK;
      }
    }
    HRESULT input_mode_category_status(){
      if(FAILED(ready_))return ready_;
      ComReference<IEnumGUID> enumeration;
      HRESULT hr=diagnostics_.record("EnumCategoriesInItem.input_mode",categories_->EnumCategoriesInItem(kServiceClsid,enumeration.put()));
      if(FAILED(hr))return hr;
      for(;;){GUID id{};
        ULONG count=0;
        hr=enumeration->Next(1,&id,&count);
        if(FAILED(hr))return diagnostics_.record("input_mode_category.Next",hr);
        if(!count)return S_FALSE;
        if(IsEqualGUID(id,kTipCapabilityInputMode))return S_OK;
      }
    }
    HRESULT register_input_mode_category()override{
      HRESULT hr=input_mode_category_status();
      if(hr!=S_FALSE)return hr==S_OK?S_FALSE:hr;
      return diagnostics_.record("RegisterCategory.input_mode",categories_->RegisterCategory(kServiceClsid,kTipCapabilityInputMode,kServiceClsid));
    }
    HRESULT unregister_input_mode_category()override{
      return unregister_category_if_present(
      [&]{return input_mode_category_status();},
      [&]{return diagnostics_.record("unregister_input_mode_category.ownership",com_.matches());},
      [&]{return diagnostics_.record("UnregisterCategory.input_mode",categories_->UnregisterCategory(kServiceClsid,kTipCapabilityInputMode,kServiceClsid));});
    }
    HRESULT register_immersive_category()override{
      HRESULT hr=immersive_category_status();
      if(hr!=S_FALSE)return hr==S_OK?S_FALSE:hr;
      return diagnostics_.record("RegisterCategory.immersive",categories_->RegisterCategory(kServiceClsid,kTipCapabilityImmersive,kServiceClsid));
    }
    HRESULT unregister_immersive_category()override{
      return unregister_category_if_present(
      [&]{return immersive_category_status();},
      [&]{return diagnostics_.record("unregister_immersive_category.ownership",com_.matches());},
      [&]{return diagnostics_.record("UnregisterCategory.immersive",categories_->UnregisterCategory(kServiceClsid,kTipCapabilityImmersive,kServiceClsid));});
    }
    HRESULT register_com()override{if(FAILED(ready_))return ready_;
      return diagnostics_.record("com.add",com_.add());
    }
    HRESULT register_profile()override{
      HRESULT hr=profile_status();
      if(hr!=S_FALSE)return hr==S_OK?S_FALSE:hr;
      const std::wstring description=L"嘸．改";
      const std::wstring path=dll_.wstring();
      return diagnostics_.record("RegisterProfile",profiles_->RegisterProfile(kServiceClsid,kProfileLanguage,kProfileGuid,description.c_str(),static_cast<ULONG>(description.size()),path.c_str(),static_cast<ULONG>(path.size()),0,nullptr,0,TRUE,0));
    }
    HRESULT register_keyboard_category()override{
      HRESULT hr=category_status();
      if(hr!=S_FALSE)return hr==S_OK?S_FALSE:hr;
      return diagnostics_.record("RegisterCategory",categories_->RegisterCategory(kServiceClsid,GUID_TFCAT_TIP_KEYBOARD,kServiceClsid));
    }
    HRESULT register_system_tray_category()override{
      HRESULT hr=system_tray_category_status();
      if(hr!=S_FALSE)return hr==S_OK?S_FALSE:hr;
      return diagnostics_.record("RegisterCategory.systray",categories_->RegisterCategory(kServiceClsid,kTipCapabilitySysTray,kServiceClsid));
    }
    HRESULT register_ui_element_category()override{
      HRESULT hr=ui_element_category_status();
      if(hr!=S_FALSE)return hr==S_OK?S_FALSE:hr;
      return diagnostics_.record("RegisterCategory.ui_element",categories_->RegisterCategory(kServiceClsid,kTipCapabilityUiElement,kServiceClsid));
    }
    HRESULT unregister_profile()override{
      HRESULT ownership=diagnostics_.record("unregister_profile.ownership",com_.matches());
      if(FAILED(ownership))return ownership;
      HRESULT hr=profile_status();
      if(hr!=S_OK)return hr;
      return diagnostics_.record("UnregisterProfile",profiles_->UnregisterProfile(kServiceClsid,kProfileLanguage,kProfileGuid,0));
    }
    HRESULT unregister_keyboard_category()override{
      HRESULT ownership=diagnostics_.record("unregister_category.ownership",com_.matches());
      if(FAILED(ownership))return ownership;
      HRESULT hr=category_status();
      if(hr!=S_OK)return hr;
      return diagnostics_.record("UnregisterCategory",categories_->UnregisterCategory(kServiceClsid,GUID_TFCAT_TIP_KEYBOARD,kServiceClsid));
    }
    HRESULT unregister_system_tray_category()override{
      return unregister_category_if_present(
      [&]{return system_tray_category_status();},
      [&]{return diagnostics_.record("unregister_systray_category.ownership",com_.matches());},
      [&]{return diagnostics_.record("UnregisterCategory.systray",categories_->UnregisterCategory(kServiceClsid,kTipCapabilitySysTray,kServiceClsid));});
    }
    HRESULT unregister_ui_element_category()override{
      return unregister_category_if_present(
      [&]{return ui_element_category_status();},
      [&]{return diagnostics_.record("unregister_ui_element_category.ownership",com_.matches());},
      [&]{return diagnostics_.record("UnregisterCategory.ui_element",categories_->UnregisterCategory(kServiceClsid,kTipCapabilityUiElement,kServiceClsid));});
    }
    HRESULT unregister_com()override{
      HRESULT profile=profile_status();
      if(profile==S_OK)return E_PENDING;
      if(FAILED(profile))return profile;
      return diagnostics_.record("com.remove",com_.remove());
    }
  };
}
#endif
