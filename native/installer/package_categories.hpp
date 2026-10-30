#pragma once
#ifdef _WIN32
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "independent_categories.hpp"

namespace boshiamy::installer::package_categories {
inline const CLSID service = {0x5d7c4291,0x2b34,0x4ce8,{0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
inline const GUID tray = {0x25504fb4,0x7bab,0x4bc1,{0x9c,0x69,0xcf,0x81,0x89,0x0f,0x0e,0xf5}};
inline const GUID ui = {0x49d2f9cf,0x1f5e,0x11d7,{0xa6,0xd3,0x00,0x06,0x5b,0x84,0x43,0x5c}};
inline const GUID immersive = {0x13a016df,0x560b,0x46cd,{0x94,0x7a,0x4c,0x3a,0xf1,0xe0,0xe3,0x5d}};
inline const GUID input_mode = {0xccf05dd7,0x4a87,0x11d7,{0xa6,0xe2,0x00,0x06,0x5b,0x84,0x43,0x5c}};
using Categories = std::vector<GUID>;
inline bool contains(const Categories& values,const GUID& id) {
    return std::any_of(values.begin(),values.end(),[&](const GUID& value){return IsEqualGUID(value,id);});
}
inline bool extra(const GUID& id) { return IsEqualGUID(id,immersive)||IsEqualGUID(id,input_mode); }
inline bool known(const GUID& id) {
    return extra(id)||IsEqualGUID(id,GUID_TFCAT_TIP_KEYBOARD)||IsEqualGUID(id,tray)||IsEqualGUID(id,ui);
}
class Store {
public:
    virtual ~Store()=default;
    virtual HRESULT read(Categories*)=0;
    virtual HRESULT remove(const GUID&)=0;
    virtual HRESULT add(const GUID&){return E_NOTIMPL;}
};
inline HRESULT capture(Store& store,Categories* out) {
    if(!out)return E_POINTER;
    out->clear();
    const HRESULT result=store.read(out);
    if(result!=S_OK)return result;
    Categories seen;
    for(const auto& id:*out) {
        if(!known(id)||contains(seen,id))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        seen.push_back(id);
    }
    return S_OK;
}
inline bool same(const Categories& a,const Categories& b) {
    return a.size()==b.size()&&std::all_of(a.begin(),a.end(),[&](const GUID& id){return contains(b,id);});
}
// Remove modern capabilities while owned COM/profile provenance still exists.
// This is explicit uninstall progress, outside the seven-resource rollback
// snapshot. An interrupted cleanup can be retried against that same provenance.
inline HRESULT remove_extras(Store& store,const Categories& captured) {
    Categories remaining;
    HRESULT result=capture(store,&remaining);
    if(result!=S_OK)return result;
    if(!same(remaining,captured))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    for(;;) {
        const auto candidate=std::find_if(remaining.begin(),remaining.end(),extra);
        if(candidate==remaining.end())return S_OK;
        const GUID id=*candidate;
        Categories fresh;
        result=capture(store,&fresh);
        if(result!=S_OK)return result;
        if(!same(fresh,remaining))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        result=store.remove(id);
        if(result!=S_OK)return result;
        remaining.erase(candidate);
        result=capture(store,&fresh);
        if(result!=S_OK)return result;
        if(!same(fresh,remaining))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
}
class WindowsStore final:public Store {
    ITfCategoryMgr* manager_=nullptr;
    HRESULT ready_=E_UNEXPECTED;
public:
    WindowsStore() {ready_=CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,
        IID_ITfCategoryMgr,reinterpret_cast<void**>(&manager_));}
    ~WindowsStore(){if(manager_)manager_->Release();}
    WindowsStore(const WindowsStore&)=delete;
    WindowsStore& operator=(const WindowsStore&)=delete;
    HRESULT read(Categories* out) override {
        if(!out)return E_POINTER;
        out->clear();if(ready_!=S_OK)return ready_;
        return independent_categories::read_fresh(out);
    }
    HRESULT remove(const GUID& id) override {
        if(ready_!=S_OK)return ready_;
        if(!extra(id))return E_INVALIDARG;
        return manager_->UnregisterCategory(service,id,service);
    }
    HRESULT add(const GUID& id) override {
        if(ready_!=S_OK)return ready_;
        if(!extra(id))return E_INVALIDARG;
        return manager_->RegisterCategory(service,id,service);
    }
};
}
#endif
