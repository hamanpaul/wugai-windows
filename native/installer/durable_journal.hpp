#pragma once

// Durable transaction-journal foundation; not yet wired into deployment_coordinator.hpp.
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace boshiamy::installer::durable_journal {
namespace fs = std::filesystem;
enum class FaultPoint { after_partial_write, before_flush, after_flush_before_replace, before_replace };
struct FaultInjector { std::function<bool(FaultPoint)> fail; };
struct Identity {
    // Schemas 1/2 are current-user COM; schema 3 explicitly includes HKLM64.
    uint32_t schema_version = 1;
    std::string owner;
    std::string token;
    std::wstring install_root;
    // Optional dedicated external ledger root; empty preserves legacy in-root storage.
    std::wstring storage_root;
    std::string package_version;
    std::string package_manifest_sha256;
};
struct Intent {
    uint64_t sequence = 0;
    std::string resource;
    std::string action;
    std::string expected_before_fingerprint;
    std::string desired_fingerprint;
};
struct Observation {
    uint64_t sequence = 0;
    HRESULT operation_status = E_FAIL;
    std::string observed_fingerprint;
    bool mutation_may_have_occurred = false;
};
struct Event {
    enum class Kind : uint8_t { intent = 1, observation = 2, commit = 3 } kind = Kind::intent;
    Intent intent;
    Observation observation;
    std::string commit_fingerprint;
};
struct Document {
    Identity identity;
    std::vector<Event> events;
    uint64_t last_complete_sequence = 0;
    bool committed = false;
    bool all_operations_succeeded = true;
    // Invalid input returns failure and retains the validated prefix here.
};

namespace detail {
using Digest = std::array<uint8_t, 32>;
static constexpr char magic[] = "LKJRN001";
static constexpr size_t max_journal_size = 16u * 1024u * 1024u;
static constexpr size_t max_record_size = 1024u * 1024u;
inline HRESULT win32_error(DWORD error = GetLastError()) {
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
inline bool safe_token(const std::string& value) {
    if (value.empty() || value.size() > 64 || value == "." || value == ".." ||
        value.find("..") != std::string::npos || value.back() == '.') return false;
    for (unsigned char c : value)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
inline bool valid_hash(const std::string& value) {
    if (value.size() != 64) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool same_path(const std::wstring& a,const std::wstring& b);
inline bool utf8(const std::wstring& value, std::string* out) {
    if (!out || value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
    if (value.empty()) { out->clear(); return true; }
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return false;
    out->resize(static_cast<size_t>(needed));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), out->data(), needed, nullptr, nullptr) == needed;
}
inline bool wide(const std::string& value, std::wstring* out) {
    if (!out || value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
    if (value.empty()) { out->clear(); return true; }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return false;
    out->resize(static_cast<size_t>(needed));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), out->data(), needed) == needed;
}
inline bool sha256(const uint8_t* data, size_t size, Digest* out) {
    if (!out || size > static_cast<size_t>((std::numeric_limits<ULONG>::max)())) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    std::vector<uint8_t> object;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &got, 0) >= 0;
    if (ok) {
        object.resize(object_size);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
    }
    if (ok && size) ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, out->data(), static_cast<ULONG>(out->size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}
inline bool sha256(const std::string& data, Digest* out) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
}
inline void put_u32(std::string* out, uint32_t value) {
    for (unsigned i=0;i<4;++i) out->push_back(static_cast<char>((value>>(i*8))&0xff));
}
inline void put_u64(std::string* out, uint64_t value) {
    for (unsigned i=0;i<8;++i) out->push_back(static_cast<char>((value>>(i*8))&0xff));
}
inline void put_string(std::string* out, const std::string& value) {
    put_u32(out, static_cast<uint32_t>(value.size())); out->append(value);
}
inline bool get_u32(const std::string& in, size_t* at, uint32_t* value) {
    if (!at || !value || *at > in.size() || in.size()-*at < 4) return false;
    *value=0; for(unsigned i=0;i<4;++i) *value |= static_cast<uint32_t>(static_cast<uint8_t>(in[(*at)++]))<<(i*8);
    return true;
}
inline bool get_u64(const std::string& in, size_t* at, uint64_t* value) {
    if (!at || !value || *at > in.size() || in.size()-*at < 8) return false;
    *value=0; for(unsigned i=0;i<8;++i) *value |= static_cast<uint64_t>(static_cast<uint8_t>(in[(*at)++]))<<(i*8);
    return true;
}
inline bool get_string(const std::string& in,size_t* at,std::string* value,size_t max=65536) {
    uint32_t length=0; if(!get_u32(in,at,&length)||length>max||*at>in.size()||in.size()-*at<length)return false;
    value->assign(in.data()+*at,length); *at+=length; return true;
}
inline std::string digest_bytes(const Digest& value) {
    return std::string(reinterpret_cast<const char*>(value.data()),value.size());
}
inline bool digest_equal(const Digest& left,const Digest& right) {
    uint8_t difference=0; for(size_t i=0;i<left.size();++i) difference|=left[i]^right[i]; return difference==0;
}
inline bool machine_com_scope(const Identity& id) { return id.schema_version==3; }
inline const wchar_t* ledger_namespace(bool machine_com_x64) {
    return machine_com_x64?L".liukai-installer-ledger-machine-com-x64":L".liukai-installer-ledger";
}
inline const wchar_t* ledger_namespace(const Identity& id) {
    return ledger_namespace(machine_com_scope(id));
}
inline bool valid_identity(const Identity& id) {
    if((id.schema_version!=1&&id.schema_version!=2&&id.schema_version!=3)||!safe_token(id.owner)||!safe_token(id.token)||
       !safe_token(id.package_version)||!valid_hash(id.package_manifest_sha256)||
       id.install_root.empty())return false;
    try {
        if(!fs::path(id.install_root).is_absolute())return false;
        if(id.schema_version==1)return id.storage_root.empty();
        if(id.storage_root.empty()||!fs::path(id.storage_root).is_absolute())return false;
        const fs::path expected=fs::path(id.install_root).lexically_normal().parent_path() /
            ledger_namespace(id) / std::wstring(id.owner.begin(),id.owner.end()) /
            std::wstring(id.token.begin(),id.token.end());
        return same_path(id.storage_root,expected.wstring());
    } catch(...) { return false; }
}
inline bool same_path(const std::wstring& a,const std::wstring& b) {
    try {
        const auto left=fs::path(a).lexically_normal().wstring();
        const auto right=fs::path(b).lexically_normal().wstring();
        return _wcsicmp(left.c_str(),right.c_str())==0;
    } catch(...) { return false; }
}
inline bool directory_chain_is_safe(const fs::path& path,bool allow_missing_final=false) {
    try {
        if(!path.is_absolute())return false;
        fs::path current=path.root_path();
        for(const auto& part:path.relative_path()) {
            if(part.empty()||part==L".")continue;
            if(part==L"..")return false;
            current/=part;
            const DWORD attr=GetFileAttributesW(current.c_str());
            if(attr==INVALID_FILE_ATTRIBUTES) {
                const DWORD error=GetLastError();
                if(allow_missing_final&&same_path(current.wstring(),path.lexically_normal().wstring())&&
                   (error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND))return true;
                return false;
            }
            if((attr&FILE_ATTRIBUTE_DIRECTORY)==0||(attr&FILE_ATTRIBUTE_REPARSE_POINT)!=0)return false;
        }
        return true;
    } catch(...) { return false; }
}
inline fs::path effective_storage_root(const Identity& id,const fs::path& supplied={}) {
    const std::wstring expected=id.schema_version==1?id.install_root:id.storage_root;
    if(expected.empty()||(!supplied.empty()&&!same_path(supplied.wstring(),expected)))return {};
    if(id.schema_version==1)return fs::path(expected);
    return fs::path(id.storage_root);
}
inline bool path_is_safe(const fs::path& path,const Identity& id,const fs::path& supplied={}) {
    try {
        const fs::path root=effective_storage_root(id,supplied);
        if(root.empty()||!path.is_absolute()||!same_path(path.parent_path().wstring(),root.wstring())||
           !directory_chain_is_safe(root))return false;
        if(id.schema_version>=2) {
            const fs::path install=fs::path(id.install_root).lexically_normal();
            const fs::path ledger=root.lexically_normal();
            if(same_path(install.wstring(),ledger.wstring())||
               same_path(install.parent_path().wstring(),ledger.wstring())||
               !directory_chain_is_safe(install,true))return false;
        }
        const DWORD attr=GetFileAttributesW(path.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES) { const DWORD error=GetLastError(); if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return false; }
        else if((attr&FILE_ATTRIBUTE_REPARSE_POINT)!=0)return false;
        return !path.filename().empty();
    } catch(...) { return false; }
}
inline bool same_identity(const Identity& a,const Identity& b) {
    return a.schema_version==b.schema_version&&a.owner==b.owner&&a.token==b.token&&
        same_path(a.install_root,b.install_root)&&same_path(a.storage_root,b.storage_root)&&a.package_version==b.package_version&&
        a.package_manifest_sha256==b.package_manifest_sha256;
}
inline std::string header_payload(const Identity& id) {
    std::string root,storage;
    if(!utf8(id.install_root,&root)||(id.schema_version>=2&&!utf8(id.storage_root,&storage)))return {};
    std::string out;
    put_u32(&out,id.schema_version); put_string(&out,id.owner); put_string(&out,id.token);
    put_string(&out,root); put_string(&out,id.package_version); put_string(&out,id.package_manifest_sha256);
    if(id.schema_version>=2)put_string(&out,storage);
    return out;
}
inline bool parse_header(const std::string& image,size_t* at,Identity* id,Digest* chain) {
    if(!at||!id||!chain||image.size()<sizeof(magic)-1+4+32||
       std::memcmp(image.data(),magic,sizeof(magic)-1)!=0)return false;
    *at=sizeof(magic)-1; uint32_t length=0;
    if(!get_u32(image,at,&length)||length>max_record_size||image.size()-*at<static_cast<size_t>(length)+32)return false;
    const size_t payload_at=*at; const std::string payload=image.substr(*at,length); *at+=length;
    Digest actual{}; if(!sha256(image.substr(0,*at),&actual))return false;
    Digest stored{}; std::memcpy(stored.data(),image.data()+*at,stored.size()); *at+=stored.size();
    if(!digest_equal(actual,stored))return false;
    size_t cursor=0; uint32_t schema=0; std::string owner,token,root,version,manifest,storage;
    if(!get_u32(payload,&cursor,&schema)||!get_string(payload,&cursor,&owner,128)||
       !get_string(payload,&cursor,&token,128)||!get_string(payload,&cursor,&root,32768)||
       !get_string(payload,&cursor,&version,128)||!get_string(payload,&cursor,&manifest,128)||
       (schema>=2&&!get_string(payload,&cursor,&storage,32768))||cursor!=payload.size())return false;
    id->schema_version=schema; id->owner=std::move(owner); id->token=std::move(token);
    if(!wide(root,&id->install_root))return false;
    if(schema>=2&&!wide(storage,&id->storage_root))return false;
    id->package_version=std::move(version); id->package_manifest_sha256=std::move(manifest);
    if(!valid_identity(*id))return false;
    *chain=stored; (void)payload_at; return true;
}
inline std::string encode_event(const Event& e) {
    std::string out; out.push_back(static_cast<char>(e.kind));
    if(e.kind==Event::Kind::intent) {
        put_u64(&out,e.intent.sequence); put_string(&out,e.intent.resource); put_string(&out,e.intent.action);
        put_string(&out,e.intent.expected_before_fingerprint); put_string(&out,e.intent.desired_fingerprint);
    } else if(e.kind==Event::Kind::observation) {
        put_u64(&out,e.observation.sequence); put_u32(&out,static_cast<uint32_t>(e.observation.operation_status));
        put_string(&out,e.observation.observed_fingerprint);
        out.push_back(e.observation.mutation_may_have_occurred?1:0);
    } else if(e.kind==Event::Kind::commit) put_string(&out,e.commit_fingerprint);
    return out;
}
inline bool decode_event(const std::string& payload,Event* e) {
    if(!e||payload.empty())return false;
    size_t at=1; const uint8_t kind=static_cast<uint8_t>(payload[0]);
    if(kind==static_cast<uint8_t>(Event::Kind::intent)) {
        e->kind=Event::Kind::intent;
        if(!get_u64(payload,&at,&e->intent.sequence)||!get_string(payload,&at,&e->intent.resource,128)||
           !get_string(payload,&at,&e->intent.action,128)||
           !get_string(payload,&at,&e->intent.expected_before_fingerprint,128)||
           !get_string(payload,&at,&e->intent.desired_fingerprint,128)||at!=payload.size())return false;
        if(!safe_token(e->intent.resource)||!safe_token(e->intent.action)||
           !valid_hash(e->intent.expected_before_fingerprint)||!valid_hash(e->intent.desired_fingerprint))return false;
    } else if(kind==static_cast<uint8_t>(Event::Kind::observation)) {
        e->kind=Event::Kind::observation; uint32_t status=0;
        if(!get_u64(payload,&at,&e->observation.sequence)||!get_u32(payload,&at,&status)||
           !get_string(payload,&at,&e->observation.observed_fingerprint,128)||at>=payload.size())return false;
        const uint8_t mutated=static_cast<uint8_t>(payload[at++]);
        if(mutated>1||at!=payload.size()||!valid_hash(e->observation.observed_fingerprint))return false;
        e->observation.operation_status=static_cast<HRESULT>(status);
        e->observation.mutation_may_have_occurred=mutated!=0;
    } else if(kind==static_cast<uint8_t>(Event::Kind::commit)) {
        e->kind=Event::Kind::commit;
        if(!get_string(payload,&at,&e->commit_fingerprint,128)||at!=payload.size()||
           !valid_hash(e->commit_fingerprint))return false;
    } else return false;
    return true;
}
inline bool apply_event(Document* d,const Event& e,bool* has_pending,Intent* pending) {
    if(!d||!has_pending||!pending||d->committed)return false;
    if(e.kind==Event::Kind::intent) {
        if(e.intent.resource=="com-machine-x64"&&!machine_com_scope(d->identity))return false;
        if(*has_pending||e.intent.sequence!=d->last_complete_sequence+1)return false;
        *pending=e.intent; *has_pending=true;
    } else if(e.kind==Event::Kind::observation) {
        if(!*has_pending||e.observation.sequence!=pending->sequence)return false;
        if(FAILED(e.observation.operation_status)||e.observation.observed_fingerprint!=pending->desired_fingerprint)
            d->all_operations_succeeded=false;
        *has_pending=false; d->last_complete_sequence=e.observation.sequence;
    } else if(e.kind==Event::Kind::commit) {
        if(*has_pending||d->last_complete_sequence==0||!d->all_operations_succeeded)return false;
        d->committed=true;
    } else return false;
    d->events.push_back(e); return true;
}
inline std::string make_header(const Identity& id,Digest* chain) {
    const std::string payload=header_payload(id);
    if(payload.empty()||payload.size()>max_record_size)return {};
    std::string image(magic,sizeof(magic)-1); put_u32(&image,static_cast<uint32_t>(payload.size()));
    image.append(payload); if(!sha256(image,chain))return {};
    image.append(digest_bytes(*chain)); return image;
}
inline bool append_hashed_event(std::string* image,const Event& event,Digest* chain) {
    if(!image||!chain)return false;
    const std::string payload=encode_event(event);
    if(payload.empty()||payload.size()>max_record_size)return false;
    std::string material=digest_bytes(*chain); material.append(payload);
    Digest next{}; if(!sha256(material,&next))return false;
    put_u32(image,static_cast<uint32_t>(payload.size())); image->append(payload); image->append(digest_bytes(next));
    *chain=next; return true;
}
inline bool read_file(const fs::path& path,std::string* out) {
    if(!out)return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if(!GetFileInformationByHandleEx(file,FileAttributeTagInfo,&tag,sizeof(tag))||
       (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0) {
        CloseHandle(file);SetLastError(ERROR_ACCESS_DENIED);return false;
    }
    LARGE_INTEGER size{}; bool ok=GetFileSizeEx(file,&size)!=0&&size.QuadPart>=0&&
        static_cast<uint64_t>(size.QuadPart)<=max_journal_size;
    if(ok) {
        out->resize(static_cast<size_t>(size.QuadPart)); size_t offset=0;
        while(offset<out->size()) {
            const DWORD chunk=static_cast<DWORD>(std::min<size_t>(out->size()-offset,1u<<20));
            DWORD got=0; if(!ReadFile(file,out->data()+offset,chunk,&got,nullptr)||got!=chunk){ok=false;break;} offset+=got;
        }
    }
    const DWORD error=ok?ERROR_SUCCESS:GetLastError(); CloseHandle(file); if(!ok)SetLastError(error); return ok;
}
inline std::atomic<uint64_t> temp_counter{0};
inline bool inject(const FaultInjector* faults,FaultPoint point) {
    try { return faults&&faults->fail&&faults->fail(point); } catch(...) { return true; }
}
class DirectoryGuard {
public:
    explicit DirectoryGuard(const std::wstring& root) {
        handle_=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle_==INVALID_HANDLE_VALUE){handle_=nullptr;status_=win32_error();return;}
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if(!GetFileInformationByHandleEx(handle_,FileAttributeTagInfo,&tag,sizeof(tag))||
           (tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)==0||
           (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0) {
            status_=HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);CloseHandle(handle_);handle_=nullptr;return;
        }
        // This denies rename/removal during the operation. The install-root ACL
        // must also prevent untrusted writers from replacing children.
        status_=S_OK;
    }
    ~DirectoryGuard(){if(handle_)CloseHandle(handle_);}
    HRESULT status()const{return status_;}
private:
    HANDLE handle_=nullptr;HRESULT status_=E_FAIL;
};
class PathLock {
public:
    explicit PathLock(const fs::path& path,DWORD timeout_ms) {
        std::string utf;
        Digest hash{};
        std::wstring normalized=path.lexically_normal().wstring();
        if(!normalized.empty())CharLowerBuffW(normalized.data(),static_cast<DWORD>(normalized.size()));
        if(!utf8(normalized,&utf)||!sha256(utf,&hash)){status_=E_FAIL;return;}
        static constexpr wchar_t hex[]=L"0123456789abcdef";
        std::wstring name=L"Local\\LiuKaiJournal-";
        for(uint8_t b:hash){name.push_back(hex[b>>4]);name.push_back(hex[b&15]);}
        handle_=CreateMutexW(nullptr,FALSE,name.c_str());
        if(!handle_){status_=win32_error();return;}
        const DWORD wait=WaitForSingleObject(handle_,timeout_ms);
        if(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED){held_=true;status_=S_OK;}
        else if(wait==WAIT_TIMEOUT)status_=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else status_=win32_error();
    }
    ~PathLock(){if(held_)ReleaseMutex(handle_);if(handle_)CloseHandle(handle_);}
    HRESULT status()const{return status_;}
private: HANDLE handle_=nullptr; bool held_=false; HRESULT status_=E_FAIL;
};
inline HRESULT write_image(const fs::path& path,const std::string& image,bool replace,
                           const std::string* expected_old,const FaultInjector* faults) {
    fs::path temp;
    HANDLE file=INVALID_HANDLE_VALUE;
    for(unsigned attempt=0;attempt<64;++attempt) {
        const auto serial=temp_counter.fetch_add(1);
        // Keep the atomic sibling short even when the final proof filename
        // nearly reaches MAX_PATH. CREATE_NEW plus PID/serial remains exclusive.
        temp=path.parent_path()/(L".lkj-"+std::to_wstring(GetCurrentProcessId())+
            L"-"+std::to_wstring(serial)+L".tmp");
        file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
        if(file!=INVALID_HANDLE_VALUE)break;
        if(GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)return win32_error();
    }
    if(file==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    HRESULT result=S_OK; bool ok=true;
    auto cleanup=[&](HRESULT error) {
        CloseHandle(file); file=INVALID_HANDLE_VALUE;
        if(!DeleteFileW(temp.c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)return win32_error();
        return error;
    };
    if(inject(faults,FaultPoint::after_partial_write)) {
        const DWORD half=static_cast<DWORD>(image.size()/2); DWORD written=0;
        if(half&&!WriteFile(file,image.data(),half,&written,nullptr))return cleanup(win32_error());
        if(half&&written!=half)return cleanup(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        return cleanup(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
    }
    size_t offset=0;
    while(offset<image.size()) {
        const DWORD chunk=static_cast<DWORD>(std::min<size_t>(image.size()-offset,1u<<20)); DWORD written=0;
        if(!WriteFile(file,image.data()+offset,chunk,&written,nullptr)||written!=chunk){ok=false;result=win32_error();break;}
        offset+=written;
    }
    if(!ok)return cleanup(result);
    if(inject(faults,FaultPoint::before_flush))return cleanup(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
    if(!FlushFileBuffers(file))return cleanup(win32_error());
    CloseHandle(file); file=INVALID_HANDLE_VALUE;
    if(inject(faults,FaultPoint::after_flush_before_replace)||inject(faults,FaultPoint::before_replace))
        return DeleteFileW(temp.c_str())?HRESULT_FROM_WIN32(ERROR_WRITE_FAULT):win32_error();
    if(replace) {
        std::string current;
        if(!expected_old||!read_file(path,&current)) {
            const HRESULT read_error=win32_error();
            return DeleteFileW(temp.c_str())?read_error:win32_error();
        }
        if(current!=*expected_old)
            return DeleteFileW(temp.c_str())?HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH):win32_error();
    }
    const DWORD flags=MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0);
    if(!MoveFileExW(temp.c_str(),path.c_str(),flags)) {
        const HRESULT move_error=win32_error();
        return DeleteFileW(temp.c_str())?move_error:win32_error();
    }
    return S_OK;
}
inline bool parse_image(const std::string& image,Document* doc,Digest* last_hash) {
    if(!doc||!last_hash||image.size()>max_journal_size)return false;
    Document parsed; size_t at=0; Digest chain{};
    if(!parse_header(image,&at,&parsed.identity,&chain))return false;
    bool pending=false; Intent pending_intent;
    while(at<image.size()) {
        if(parsed.committed||image.size()-at<4)return false;
        uint32_t length=0; if(!get_u32(image,&at,&length)||length==0||length>max_record_size||
                              image.size()-at<static_cast<size_t>(length)+chain.size())return false;
        const std::string payload=image.substr(at,length); at+=length;
        std::string material=digest_bytes(chain); material.append(payload);
        Digest computed{}; if(!sha256(material,&computed))return false;
        Digest recorded{}; std::memcpy(recorded.data(),image.data()+at,recorded.size()); at+=recorded.size();
        if(!digest_equal(computed,recorded))return false;
        Event event; if(!decode_event(payload,&event)||!apply_event(&parsed,event,&pending,&pending_intent))return false;
        chain=recorded;
    }
    *doc=std::move(parsed); *last_hash=chain; return true;
}
inline HRESULT parse_result(const std::string& image,const Identity& expected,Document* out,Digest* last_hash) {
    if(!out)return E_POINTER;
    *out={}; size_t at=0; Digest chain{}; Identity found;
    // Populate a valid event prefix even when the tail is malformed.
    if(image.size()>max_journal_size||!parse_header(image,&at,&found,&chain))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    out->identity=found;
    if(!same_identity(found,expected))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    bool pending=false; Intent pending_intent;
    while(at<image.size()) {
        const size_t record_start=at;
        if(out->committed||image.size()-at<4)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        uint32_t length=0;
        if(!get_u32(image,&at,&length)||length==0||length>max_record_size||
           image.size()-at<static_cast<size_t>(length)+chain.size())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const std::string payload=image.substr(at,length); at+=length;
        std::string material=digest_bytes(chain); material.append(payload);
        Digest computed{}; if(!sha256(material,&computed))return E_FAIL;
        Digest recorded{}; std::memcpy(recorded.data(),image.data()+at,recorded.size()); at+=recorded.size();
        if(!digest_equal(computed,recorded))return HRESULT_FROM_WIN32(ERROR_CRC);
        Event event; if(!decode_event(payload,&event)||!apply_event(out,event,&pending,&pending_intent))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        chain=recorded;
        if(event.kind==Event::Kind::commit && at!=image.size())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        (void)record_start;
    }
    if(last_hash)*last_hash=chain;
    return S_OK;
}
} // namespace detail

class Store {
public:
    Store(fs::path journal_path,Identity identity,const FaultInjector* faults=nullptr,DWORD lock_timeout_ms=30000)
        :path_(std::move(journal_path)),identity_(std::move(identity)),storage_root_(identity_.schema_version>=2?fs::path(identity_.storage_root):fs::path(identity_.install_root)),faults_(faults),lock_timeout_ms_(lock_timeout_ms){}
    Store(fs::path journal_path,Identity identity,fs::path storage_root,
          const FaultInjector* faults=nullptr,DWORD lock_timeout_ms=30000)
        :path_(std::move(journal_path)),identity_(std::move(identity)),storage_root_(std::move(storage_root)),
         faults_(faults),lock_timeout_ms_(lock_timeout_ms){}
    HRESULT create() {
        if(!detail::valid_identity(identity_)||!detail::path_is_safe(path_,identity_,storage_root_))return E_INVALIDARG;
        detail::DirectoryGuard directory(storage_root_.wstring());
        if(FAILED(directory.status()))return directory.status();
        detail::PathLock lock(path_,lock_timeout_ms_); if(FAILED(lock.status()))return lock.status();
        detail::Digest chain{};
        std::string image=detail::make_header(identity_,&chain);
        if(image.empty())return E_FAIL;
        return detail::write_image(path_,image,false,nullptr,faults_);
    }
    HRESULT read(const Identity& expected,Document* out)const {
        if(!out)return E_POINTER;
        *out={};
        if(!detail::path_is_safe(path_,identity_,storage_root_))return E_ACCESSDENIED;
        if(!detail::valid_identity(expected))return E_INVALIDARG;
        detail::DirectoryGuard directory(storage_root_.wstring());
        if(FAILED(directory.status()))return directory.status();
        detail::PathLock lock(path_,lock_timeout_ms_); if(FAILED(lock.status()))return lock.status();
        std::string image;
        if(!detail::read_file(path_,&image))return detail::win32_error();
        return detail::parse_result(image,expected,out,nullptr);
    }
    HRESULT append_intent(const std::string& resource,const std::string& action,
        const std::string& before,const std::string& desired,uint64_t* sequence) {
        if(!sequence)return E_POINTER;
        *sequence=0;
        if(resource=="com-machine-x64"&&!detail::machine_com_scope(identity_))return E_INVALIDARG;
        if(!detail::safe_token(resource)||!detail::safe_token(action)||!detail::valid_hash(before)||!detail::valid_hash(desired))return E_INVALIDARG;
        Event event; event.kind=Event::Kind::intent;
        event.intent.resource=resource; event.intent.action=action;
        event.intent.expected_before_fingerprint=before; event.intent.desired_fingerprint=desired;
        return append(event,sequence);
    }
    HRESULT append_observation(uint64_t sequence,HRESULT operation_status,const std::string& observed,bool may_mutate) {
        if(!detail::valid_hash(observed))return E_INVALIDARG;
        Event event; event.kind=Event::Kind::observation;
        event.observation.sequence=sequence; event.observation.operation_status=operation_status;
        event.observation.observed_fingerprint=observed; event.observation.mutation_may_have_occurred=may_mutate;
        return append(event,nullptr);
    }
    HRESULT append_commit(const std::string& final_fingerprint) {
        if(!detail::valid_hash(final_fingerprint))return E_INVALIDARG;
        Event event; event.kind=Event::Kind::commit; event.commit_fingerprint=final_fingerprint;
        return append(event,nullptr);
    }
private:
    fs::path path_; Identity identity_; fs::path storage_root_; const FaultInjector* faults_=nullptr; DWORD lock_timeout_ms_=30000;
    HRESULT append(Event event,uint64_t* assigned_sequence) {
        if(assigned_sequence)*assigned_sequence=0;
        if(!detail::valid_identity(identity_)||!detail::path_is_safe(path_,identity_,storage_root_))return E_INVALIDARG;
        detail::DirectoryGuard directory(storage_root_.wstring());
        if(FAILED(directory.status()))return directory.status();
        detail::PathLock lock(path_,lock_timeout_ms_); if(FAILED(lock.status()))return lock.status();
        std::string image; if(!detail::read_file(path_,&image))return detail::win32_error();
        Document doc; detail::Digest chain{};
        const HRESULT loaded=detail::parse_result(image,identity_,&doc,&chain);
        if(FAILED(loaded))return loaded;
        bool pending=false; Intent pending_intent;
        for(const Event& prior:doc.events) {
            if(prior.kind==Event::Kind::intent){pending=true;pending_intent=prior.intent;}
            else if(prior.kind==Event::Kind::observation)pending=false;
        }
        if(event.kind==Event::Kind::intent) {
            if(doc.committed||pending)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            event.intent.sequence=doc.last_complete_sequence+1;
        } else if(event.kind==Event::Kind::observation) {
            if(doc.committed||!pending||event.observation.sequence!=pending_intent.sequence)
                return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        } else if(event.kind==Event::Kind::commit) {
            if(doc.committed||pending||doc.last_complete_sequence==0||!doc.all_operations_succeeded)
                return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        }
        std::string updated=image;
        if(!detail::append_hashed_event(&updated,event,&chain))return E_FAIL;
        if(updated.size()>detail::max_journal_size)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        const HRESULT written=detail::write_image(path_,updated,true,&image,faults_);
        if(SUCCEEDED(written)&&assigned_sequence)*assigned_sequence=event.intent.sequence;
        return written;
    }
};
} // namespace boshiamy::installer::durable_journal
