#ifndef LIUKAI_COM_REGISTRATION_HPP
#define LIUKAI_COM_REGISTRATION_HPP
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <cstring>
#include <string>
#include <vector>
namespace boshiamy {
  class RegistryValueOperations {
    public:
    virtual ~RegistryValueOperations()=default;
    virtual LONG query(HKEY key,const wchar_t* name,DWORD* type,BYTE* data,DWORD* bytes)=0;
    virtual LONG set(HKEY key,const wchar_t* name,DWORD type,const BYTE* data,DWORD bytes)=0;
  };
  class WindowsRegistryValueOperations final:public RegistryValueOperations {
    public:
    LONG query(HKEY key,const wchar_t* name,DWORD* type,BYTE* data,DWORD* bytes)override{return RegQueryValueExW(key,name,nullptr,type,data,bytes);
    }
    LONG set(HKEY key,const wchar_t* name,DWORD type,const BYTE* data,DWORD bytes)override{return RegSetValueExW(key,name,0,type,data,bytes);
    }
  };
  class RegistryKey {
    HKEY key_=nullptr;
    public:
    RegistryKey()=default;
    RegistryKey(const RegistryKey&)=delete;
    RegistryKey& operator=(const RegistryKey&)=delete;
    ~RegistryKey(){if(key_)RegCloseKey(key_);
    }
    HKEY get()const{return key_;
    }HKEY* put(){return &key_;
    }
  };
  class ComServerRegistration {
    HKEY root_;
    CLSID clsid_;
    std::wstring dll_,path_;
    static constexpr const wchar_t* owner_=L"LiuKai-TSF-v1";
    static LONG write(HKEY key,const wchar_t* name,const std::wstring& value){return RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    }
    static bool equal(HKEY key,const wchar_t* name,const std::wstring& expected,bool ignore_case=false){
      DWORD type=0,bytes=0;
      if(RegQueryValueExW(key,name,nullptr,&type,nullptr,&bytes)!=ERROR_SUCCESS||type!=REG_SZ||bytes<sizeof(wchar_t)||bytes>65536||bytes%sizeof(wchar_t))return false;
      std::vector<wchar_t> value(bytes/sizeof(wchar_t));
      if(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(value.data()),&bytes)!=ERROR_SUCCESS||type!=REG_SZ||bytes/sizeof(wchar_t)!=value.size()||value.back()!=L'\0')return false;
      std::wstring actual(value.data(),value.size()-1);
      return ignore_case?_wcsicmp(actual.c_str(),expected.c_str())==0&&actual.size()==expected.size():actual==expected;
    }
    struct RawValue{DWORD type=0;
      std::vector<BYTE> bytes;
    };
    static LONG read_raw(HKEY key,RawValue& value,RegistryValueOperations& operations){
      DWORD type=0,bytes=0;
      LONG status=operations.query(key,nullptr,&type,nullptr,&bytes);
      if(status!=ERROR_SUCCESS)return status;
      if(bytes==0||bytes>65536)return ERROR_INVALID_DATA;
      value.bytes.resize(bytes);
      status=operations.query(key,nullptr,&type,value.bytes.data(),&bytes);
      if(status!=ERROR_SUCCESS)return status;
      value.type=type;
      value.bytes.resize(bytes);
      return ERROR_SUCCESS;
    }
    static bool raw_string_equals(const RawValue& value,const std::wstring& expected,bool ignore_case){
      if(value.type!=REG_SZ||value.bytes.size()<sizeof(wchar_t)||value.bytes.size()%sizeof(wchar_t))return false;
      std::wstring actual(value.bytes.size()/sizeof(wchar_t),L'\0');
      std::memcpy(actual.data(),value.bytes.data(),value.bytes.size());
      if(actual.back()!=L'\0')return false;
      actual.pop_back();
      return ignore_case?_wcsicmp(actual.c_str(),expected.c_str())==0&&actual.size()==expected.size():actual==expected;
    }
    static HRESULT restore_raw(HKEY key,const RawValue& value,RegistryValueOperations& operations){
      LONG status=operations.set(key,nullptr,value.type,value.bytes.data(),static_cast<DWORD>(value.bytes.size()));
      if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
      RawValue check;
      status=read_raw(key,check,operations);
      if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
      return check.type==value.type&&check.bytes==value.bytes?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    static bool absolute_path(const std::wstring& path){
      if(path.empty()||path.size()>=static_cast<size_t>(MAXDWORD/sizeof(wchar_t))||path.find(L'\0')!=std::wstring::npos)return false;
      try{return std::filesystem::path(path).is_absolute();
      }catch(...){return false;
      }
    }
    bool matches_key(HKEY key)const{
      if(!equal(key,L"LiuKaiOwner",owner_))return false;
      RegistryKey server;
      if(RegOpenKeyExW(key,L"InprocServer32",0,KEY_READ,server.put())!=ERROR_SUCCESS)return false;
      return equal(server.get(),nullptr,dll_,true)&&equal(server.get(),L"ThreadingModel",L"Apartment");
    }
    public:
    struct UpgradeResult{HRESULT operation=S_OK;
      HRESULT cleanup=S_OK;
    };
    ComServerRegistration(HKEY root,REFCLSID clsid,std::wstring dll):root_(root),clsid_(clsid),dll_(std::move(dll)){
      wchar_t id[40]{};
      if(!StringFromGUID2(clsid_,id,40))return;
      path_=L"Software\\Classes\\CLSID\\";
      path_+=id;
    }
    const std::wstring& key_path()const{return path_;
    }
    HRESULT matches()const{
      if(path_.empty()||dll_.empty())return E_INVALIDARG;
      RegistryKey key;
      LONG status=RegOpenKeyExW(root_,path_.c_str(),0,KEY_READ,key.put());
      if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)return S_FALSE;
      if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
      return matches_key(key.get())?S_OK:HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    // Machine COM is a separate lifecycle resource. An independent reader must
    // bind its ownership to the installing SID and exact managed root as well as
    // the usual class owner, DLL and Apartment values.
    HRESULT matches_machine_identity(const std::wstring& sid,const std::wstring& managed_root)const{
      if(sid.empty()||sid.find(L'\0')!=std::wstring::npos||!absolute_path(managed_root))return E_INVALIDARG;
      HRESULT status=matches();
      if(status!=S_OK)return status;
      RegistryKey key;
      LONG opened=RegOpenKeyExW(root_,path_.c_str(),0,KEY_READ,key.put());
      if(opened!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(opened);
      return equal(key.get(),L"LiuKaiInstallSid",sid)&&equal(key.get(),L"LiuKaiInstallRoot",managed_root,true)
      ?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    HRESULT add(){
      if(path_.empty()||dll_.empty())return E_INVALIDARG;
      DWORD disposition=0;
      LONG status;
      {RegistryKey key;
        status=RegCreateKeyExW(root_,path_.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,key.put(),&disposition);
        if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
        if(disposition!=REG_CREATED_NEW_KEY)return matches_key(key.get())?S_FALSE:HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        status=write(key.get(),L"LiuKaiOwner",owner_);
        if(status==ERROR_SUCCESS)status=write(key.get(),nullptr,L"嘸．改");
        RegistryKey server;
        if(status==ERROR_SUCCESS)status=RegCreateKeyExW(key.get(),L"InprocServer32",0,nullptr,0,KEY_WRITE,nullptr,server.put(),nullptr);
        if(status==ERROR_SUCCESS)status=write(server.get(),nullptr,dll_);
        if(status==ERROR_SUCCESS)status=write(server.get(),L"ThreadingModel",L"Apartment");
      }
      if(status!=ERROR_SUCCESS){RegDeleteTreeW(root_,path_.c_str());
        return HRESULT_FROM_WIN32(status);
      }return S_OK;
    }
    HRESULT remove(){
      HRESULT status=matches();
      if(status!=S_OK)return status;
      LONG result=RegDeleteTreeW(root_,path_.c_str());
      return result==ERROR_SUCCESS?S_OK:HRESULT_FROM_WIN32(result);
    }
    UpgradeResult upgrade_to(const std::wstring& new_dll)const{WindowsRegistryValueOperations operations;
      return upgrade_to(new_dll,operations);
    }
    UpgradeResult upgrade_to(const std::wstring& new_dll,RegistryValueOperations& operations)const{
      UpgradeResult result{};
      if(path_.empty()||dll_.empty()||!absolute_path(new_dll))return {E_INVALIDARG,S_OK};
      RegistryKey class_key;
      LONG status=RegOpenKeyExW(root_,path_.c_str(),0,KEY_READ|KEY_WRITE,class_key.put());
      if(status!=ERROR_SUCCESS)return {HRESULT_FROM_WIN32(status),S_OK};
      if(!matches_key(class_key.get()))return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),S_OK};
      RegistryKey server;
      status=RegOpenKeyExW(class_key.get(),L"InprocServer32",0,KEY_QUERY_VALUE|KEY_SET_VALUE,server.put());
      if(status!=ERROR_SUCCESS)return {HRESULT_FROM_WIN32(status),S_OK};
      RawValue original;
      status=read_raw(server.get(),original,operations);
      if(status!=ERROR_SUCCESS)return {HRESULT_FROM_WIN32(status),S_OK};
      if(!raw_string_equals(original,dll_,true))return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),S_OK};
      if(raw_string_equals(original,new_dll,true))return {S_FALSE,S_OK};

      status=operations.set(server.get(),nullptr,REG_SZ,reinterpret_cast<const BYTE*>(new_dll.c_str()),static_cast<DWORD>((new_dll.size()+1)*sizeof(wchar_t)));
      if(status!=ERROR_SUCCESS){result.operation=HRESULT_FROM_WIN32(status);
        result.cleanup=restore_raw(server.get(),original,operations);
        return result;
      }
      RawValue written;
      status=read_raw(server.get(),written,operations);
      if(status==ERROR_SUCCESS&&raw_string_equals(written,new_dll,true))return {S_OK,S_OK};
      result.operation=status==ERROR_SUCCESS?HRESULT_FROM_WIN32(ERROR_INVALID_DATA):HRESULT_FROM_WIN32(status);
      result.cleanup=restore_raw(server.get(),original,operations);
      return result;
    }
  };
}
#endif
