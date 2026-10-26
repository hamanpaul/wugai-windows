#pragma once

// Exact, identity-bound snapshot sidecar for the durable installer journal.
// This format is deliberately separate from journal intent/observation records.
#include "dual_architecture_transaction.hpp"
#include "durable_journal.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace boshiamy::installer::durable_snapshot {
namespace fs = std::filesystem;
namespace journal = boshiamy::installer::durable_journal;
namespace jd = boshiamy::installer::durable_journal::detail;
constexpr size_t resource_count = static_cast<size_t>(Resource::count);
using ResourceStates = std::array<ResourceState, resource_count>;
static constexpr char snapshot_magic[] = "LKTSNP01";
// Schema 2 remains the exact six-resource current-user format. Schema 3 binds
// seven resources to the explicit machine scope; never replay schema 2 into it.
static constexpr uint32_t snapshot_schema = 3;
static constexpr uint32_t legacy_snapshot_schema = 2;
static constexpr size_t legacy_resource_count = 6;
inline uint32_t schema_for(const journal::Identity& identity) {
    return jd::machine_com_scope(identity)?snapshot_schema:legacy_snapshot_schema;
}
inline size_t count_for(const journal::Identity& identity) {
    return jd::machine_com_scope(identity)?resource_count:legacy_resource_count;
}
static constexpr size_t max_snapshot_size = 16u * 1024u * 1024u;
static constexpr size_t max_string_units = 32768;
static constexpr size_t max_private_data = 1024u * 1024u;
enum class FaultPoint {
    after_partial_write, before_flush, after_flush_before_replace, before_replace,
    after_publish, before_readback
};
struct FaultInjector { std::function<bool(FaultPoint)> fail; };

namespace detail {
inline void put_wstring(std::string* out,const std::wstring& value) {
    jd::put_u32(out,static_cast<uint32_t>(value.size()));
    for(wchar_t c:value) {
        const uint16_t unit=static_cast<uint16_t>(c);
        out->push_back(static_cast<char>(unit&0xff));
        out->push_back(static_cast<char>((unit>>8)&0xff));
    }
}
inline bool valid_utf16(const std::wstring& value) {
    for(size_t i=0;i<value.size();++i) {
        const uint16_t c=static_cast<uint16_t>(value[i]);
        if(c>=0xd800&&c<=0xdbff) {
            if(++i>=value.size())return false;
            const uint16_t low=static_cast<uint16_t>(value[i]);
            if(low<0xdc00||low>0xdfff)return false;
        } else if(c>=0xdc00&&c<=0xdfff)return false;
    }
    return true;
}
inline bool get_wstring(const std::string& in,size_t* at,std::wstring* value) {
    if(!value)return false;
    uint32_t length=0;
    if(!jd::get_u32(in,at,&length)||length>max_string_units||
       *at>in.size()||in.size()-*at<static_cast<size_t>(length)*2)return false;
    std::wstring decoded;decoded.reserve(length);
    for(uint32_t i=0;i<length;++i) {
        const uint16_t low=static_cast<uint8_t>(in[(*at)++]);
        const uint16_t high=static_cast<uint8_t>(in[(*at)++]);
        decoded.push_back(static_cast<wchar_t>(low|(high<<8)));
    }
    if(!valid_utf16(decoded))return false;
    *value=std::move(decoded);return true;
}
inline bool encode(const journal::Identity& identity,const ResourceStates& states,std::string* out) {
    if(!out||!jd::valid_identity(identity))return false;
    // A legacy scope cannot carry, capture or fingerprint an HKLM resource.
    if(!jd::machine_com_scope(identity)&&states[static_cast<size_t>(Resource::machine_com_x64)]!=ResourceState{})return false;
    const std::string identity_payload=jd::header_payload(identity);
    if(identity_payload.empty()||identity_payload.size()>65536)return false;
    std::string image(snapshot_magic,sizeof(snapshot_magic)-1);
    jd::put_u32(&image,schema_for(identity));
    jd::put_u32(&image,static_cast<uint32_t>(identity_payload.size()));
    image.append(identity_payload);
    jd::put_u32(&image,static_cast<uint32_t>(count_for(identity)));
    for(size_t i=0;i<count_for(identity);++i) {
        const auto& state=states[i];
        if(!valid_utf16(state.owner)||!valid_utf16(state.path)||!valid_utf16(state.threading_model)||
           state.owner.size()>max_string_units||state.path.size()>max_string_units||
           state.threading_model.size()>max_string_units||state.private_data.size()>max_private_data)return false;
        jd::put_u32(&image,static_cast<uint32_t>(i));
        image.push_back(state.present?1:0);
        image.push_back(state.created_by_lifecycle?1:0);
        put_wstring(&image,state.owner);put_wstring(&image,state.path);put_wstring(&image,state.threading_model);
        jd::put_u32(&image,static_cast<uint32_t>(state.private_data.size()));
        if(!state.private_data.empty())
            image.append(reinterpret_cast<const char*>(state.private_data.data()),state.private_data.size());
        if(image.size()>max_snapshot_size-32)return false;
    }
    jd::Digest digest{};
    if(!jd::sha256(image,&digest))return false;
    image.append(jd::digest_bytes(digest));
    if(image.size()>max_snapshot_size)return false;
    *out=std::move(image);return true;
}
inline HRESULT decode(const std::string& image,const journal::Identity& expected,
                      ResourceStates* output,std::string* fingerprint) {
    if(!output)return E_POINTER;
    *output={};if(fingerprint)fingerprint->clear();
    if(image.size()>max_snapshot_size||image.size()<8+4+4+4+32||
       std::memcmp(image.data(),snapshot_magic,sizeof(snapshot_magic)-1)!=0)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const size_t digest_at=image.size()-32;
    jd::Digest actual{},stored{};
    if(!jd::sha256(reinterpret_cast<const uint8_t*>(image.data()),digest_at,&actual))
        return E_FAIL;
    std::memcpy(stored.data(),image.data()+digest_at,stored.size());
    if(!jd::digest_equal(actual,stored))return HRESULT_FROM_WIN32(ERROR_CRC);
    size_t at=sizeof(snapshot_magic)-1;uint32_t schema=0,identity_size=0,count=0;
    if(!jd::get_u32(image,&at,&schema)||schema!=schema_for(expected)||
       !jd::get_u32(image,&at,&identity_size)||identity_size>65536||
       at>digest_at||digest_at-at<static_cast<size_t>(identity_size)+4)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const std::string identity_payload=image.substr(at,identity_size);at+=identity_size;
    size_t identity_at=0;uint32_t identity_schema=0;
    std::string owner,token,root,version,manifest,storage;
    if(!jd::get_u32(identity_payload,&identity_at,&identity_schema)||
       !jd::get_string(identity_payload,&identity_at,&owner,128)||
       !jd::get_string(identity_payload,&identity_at,&token,128)||
       !jd::get_string(identity_payload,&identity_at,&root,32768)||
       !jd::get_string(identity_payload,&identity_at,&version,128)||
       !jd::get_string(identity_payload,&identity_at,&manifest,128)||
       (identity_schema>=2&&!jd::get_string(identity_payload,&identity_at,&storage,32768))||
       identity_at!=identity_payload.size())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    journal::Identity found;found.schema_version=identity_schema;found.owner=std::move(owner);
    found.token=std::move(token);found.package_version=std::move(version);
    found.package_manifest_sha256=std::move(manifest);
    if(!jd::wide(root,&found.install_root)||
       (identity_schema>=2&&!jd::wide(storage,&found.storage_root))||!jd::valid_identity(found))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if(!jd::same_identity(found,expected))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if(!jd::get_u32(image,&at,&count)||count!=count_for(expected))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    ResourceStates decoded{};
    for(size_t i=0;i<count_for(expected);++i) {
        uint32_t resource_id=0;
        if(!jd::get_u32(image,&at,&resource_id)||resource_id!=i||at>digest_at||digest_at-at<2)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const uint8_t present=static_cast<uint8_t>(image[at++]);
        const uint8_t created=static_cast<uint8_t>(image[at++]);
        if(present>1||created>1)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        auto& state=decoded[i];state.present=present!=0;state.created_by_lifecycle=created!=0;
        if(!get_wstring(image,&at,&state.owner)||!get_wstring(image,&at,&state.path)||
           !get_wstring(image,&at,&state.threading_model))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        uint32_t private_size=0;
        if(!jd::get_u32(image,&at,&private_size)||private_size>max_private_data||
           at>digest_at||digest_at-at<private_size)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        state.private_data.assign(image.begin()+static_cast<std::ptrdiff_t>(at),
                                  image.begin()+static_cast<std::ptrdiff_t>(at+private_size));
        at+=private_size;
    }
    if(at!=digest_at)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::string hex;static constexpr char digits[]="0123456789abcdef";hex.reserve(64);
    for(uint8_t byte:actual){hex.push_back(digits[byte>>4]);hex.push_back(digits[byte&15]);}
    *output=std::move(decoded);if(fingerprint)*fingerprint=std::move(hex);
    return S_OK;
}
} // namespace detail

class Store {
public:
    Store(fs::path snapshot_path,journal::Identity identity,
          const FaultInjector* faults=nullptr,DWORD lock_timeout_ms=30000)
        :path_(std::move(snapshot_path)),identity_(std::move(identity)),
         storage_root_(identity_.schema_version>=2?fs::path(identity_.storage_root):fs::path(identity_.install_root)),
         faults_(faults),lock_timeout_ms_(lock_timeout_ms){}
    Store(fs::path snapshot_path,journal::Identity identity,fs::path storage_root,
          const FaultInjector* faults=nullptr,DWORD lock_timeout_ms=30000)
        :path_(std::move(snapshot_path)),identity_(std::move(identity)),storage_root_(std::move(storage_root)),
         faults_(faults),lock_timeout_ms_(lock_timeout_ms){}
    HRESULT create(const ResourceStates& states) {
        if(!jd::valid_identity(identity_)||!jd::path_is_safe(path_,identity_,storage_root_))
            return E_INVALIDARG;
        jd::DirectoryGuard directory(storage_root_.wstring());
        if(FAILED(directory.status()))return directory.status();
        jd::PathLock lock(path_,lock_timeout_ms_);
        if(FAILED(lock.status()))return lock.status();
        std::string image;
        if(!detail::encode(identity_,states,&image))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        journal::FaultInjector journal_faults;
        journal_faults.fail=[&](journal::FaultPoint point) {
            if(!faults_||!faults_->fail)return false;
            FaultPoint mapped=FaultPoint::before_replace;
            switch(point) {
            case journal::FaultPoint::after_partial_write:mapped=FaultPoint::after_partial_write;break;
            case journal::FaultPoint::before_flush:mapped=FaultPoint::before_flush;break;
            case journal::FaultPoint::after_flush_before_replace:mapped=FaultPoint::after_flush_before_replace;break;
            case journal::FaultPoint::before_replace:mapped=FaultPoint::before_replace;break;
            }
            try{return faults_->fail(mapped);}catch(...){return true;}
        };
        const HRESULT published=jd::write_image(path_,image,false,nullptr,faults_?&journal_faults:nullptr);
        if(FAILED(published))return published;
        if(inject(FaultPoint::after_publish))return E_FAIL;
        if(inject(FaultPoint::before_readback))return E_FAIL;
        std::string observed;
        if(!jd::read_file(path_,&observed))return jd::win32_error();
        if(observed!=image)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        ResourceStates verified{};std::string fingerprint;
        const HRESULT verified_status=detail::decode(observed,identity_,&verified,&fingerprint);
        if(FAILED(verified_status))return verified_status;
        if(verified!=states)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return S_OK;
    }
    HRESULT read(const journal::Identity& expected,ResourceStates* out,
                 std::string* fingerprint=nullptr)const {
        if(!out)return E_POINTER;
        *out={};if(fingerprint)fingerprint->clear();
        if(!jd::valid_identity(expected)||!jd::path_is_safe(path_,identity_,storage_root_))
            return E_INVALIDARG;
        jd::DirectoryGuard directory(storage_root_.wstring());
        if(FAILED(directory.status()))return directory.status();
        jd::PathLock lock(path_,lock_timeout_ms_);
        if(FAILED(lock.status()))return lock.status();
        std::string image;
        if(!jd::read_file(path_,&image))return jd::win32_error();
        return detail::decode(image,expected,out,fingerprint);
    }
private:
    bool inject(FaultPoint point)const {
        try{return faults_&&faults_->fail&&faults_->fail(point);}catch(...){return true;}
    }
    fs::path path_;journal::Identity identity_;fs::path storage_root_;const FaultInjector* faults_=nullptr;
    DWORD lock_timeout_ms_=30000;
};
} // namespace boshiamy::installer::durable_snapshot
