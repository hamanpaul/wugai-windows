#pragma once
#include "../tsf/profile_display_name_resource.hpp"
#include <limits>
namespace boshiamy::installer::profile_display_state {
namespace resource_name = boshiamy::profile_display::resource_name;
struct DisplaySnapshot { profile_display::ProfileSnapshot profile; resource_name::DisplayNameBinding descriptions; resource_name::DisplayNameBinding display_binding; };
inline bool equal(const DisplaySnapshot& a,const DisplaySnapshot& b) { return profile_display::detail::same_snapshot(a.profile,b.profile)&&resource_name::same_binding(a.descriptions,b.descriptions)&&resource_name::same_binding(a.display_binding,b.display_binding); }
// Serialization only: these bounds and checks are not an ownership verifier.
inline constexpr size_t kMaxBytes=1048576, kMaxStringUnits=65536, kMaxCategories=256, kMaxRawBytes=262144;
namespace detail {
struct Codec {
 std::vector<BYTE>& bytes; bool reading; size_t pos=0; bool ok=true;
 void number(uint64_t& value,size_t n){if(!ok)return;if(reading){if(n>bytes.size()-pos){ok=false;return;}value=0;for(size_t i=0;i<n;++i)value|=uint64_t(bytes[pos++])<<(i*8);}else{if(n>kMaxBytes-bytes.size()){ok=false;return;}for(size_t i=0;i<n;++i)bytes.push_back(BYTE(value>>(i*8)));}}
 template<class T> void scalar(T& v,size_t n=sizeof(T)){uint64_t x=static_cast<uint64_t>(v);number(x,n);if(reading&&ok)v=static_cast<T>(x);}
 void guid(GUID& g){scalar(g.Data1);scalar(g.Data2);scalar(g.Data3);for(auto& b:g.Data4)scalar(b);}
 void string(std::wstring& s){uint64_t n=s.size();number(n,4);if(!ok||n>kMaxStringUnits){ok=false;return;}if(reading){if(n>(bytes.size()-pos)/2){ok=false;return;}s.resize(size_t(n));}for(auto& c:s)scalar(c,2);}
 void raw(resource_name::RawRegistryValue& r){scalar(r.status,4);scalar(r.type,4);uint64_t n=r.bytes.size();number(n,4);if(!ok||n>kMaxRawBytes){ok=false;return;}if(reading){if(n>bytes.size()-pos){ok=false;return;}r.bytes.resize(size_t(n));}for(auto& b:r.bytes)scalar(b);if(r.status!=ERROR_SUCCESS&&(r.status!=ERROR_FILE_NOT_FOUND||r.type!=0||!r.bytes.empty()))ok=false;}
 void binding(resource_name::DisplayNameBinding& b){raw(b.registry64);raw(b.registry32);}
 void snapshot(DisplaySnapshot& s){auto& p=s.profile;
 scalar(p.profile_type);scalar(p.language);guid(p.service);guid(p.profile);guid(p.category);scalar(p.capabilities);scalar(p.flags);scalar(p.substitute_hkl,8);scalar(p.hkl,8);string(p.api_description);string(p.registry.description);string(p.registry.icon_file);scalar(p.registry.icon_index);scalar(p.registry.enabled_by_default);scalar(p.registry.hidden_in_settings);scalar(p.registry.subitem_in_settings);scalar(p.enabled,4);scalar(p.active_result,4);scalar(p.active_language);guid(p.active_profile);scalar(p.default_result,4);guid(p.default_service);guid(p.default_profile);
 uint64_t n=p.categories.size();number(n,4);if(!ok||n>kMaxCategories){ok=false;return;}if(reading){if(n>(bytes.size()-pos)/16){ok=false;return;}p.categories.resize(size_t(n));}for(auto& g:p.categories)guid(g);binding(s.descriptions);binding(s.display_binding);
 // These are boolean profile fields, not arbitrary DWORD enumerations.
 if(p.enabled!=FALSE&&p.enabled!=TRUE)ok=false;
 if(p.registry.enabled_by_default>1||p.registry.hidden_in_settings>1||p.registry.subitem_in_settings>1)ok=false;
 if(p.profile_type!=0&&p.profile_type!=TF_PROFILETYPE_INPUTPROCESSOR&&p.profile_type!=TF_PROFILETYPE_KEYBOARDLAYOUT)ok=false;
 }
};
}
inline HRESULT encode(const DisplaySnapshot& value,std::vector<BYTE>* out){if(!out)return E_POINTER;out->clear();try{const auto& p=value.profile; if(p.api_description.size()>kMaxStringUnits||p.registry.description.size()>kMaxStringUnits||p.registry.icon_file.size()>kMaxStringUnits||p.categories.size()>kMaxCategories||value.descriptions.registry64.bytes.size()>kMaxRawBytes||value.descriptions.registry32.bytes.size()>kMaxRawBytes||value.display_binding.registry64.bytes.size()>kMaxRawBytes||value.display_binding.registry32.bytes.size()>kMaxRawBytes)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);DisplaySnapshot copy=value;std::vector<BYTE> result;detail::Codec c{result,false};uint64_t magic=0x31534450,version=1;c.number(magic,4);c.number(version,4);c.snapshot(copy);if(!c.ok)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);*out=std::move(result);return S_OK;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}}
inline HRESULT decode(const std::vector<BYTE>& bytes,DisplaySnapshot* out){if(!out)return E_POINTER;*out={};if(bytes.size()>kMaxBytes)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);try{auto input=bytes;DisplaySnapshot value;detail::Codec c{input,true};uint64_t magic=0,version=0;c.number(magic,4);c.number(version,4);if(!c.ok||magic!=0x31534450||version!=1)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);c.snapshot(value);if(!c.ok||c.pos!=input.size())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);*out=std::move(value);return S_OK;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}}
}
