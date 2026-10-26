#pragma once

#include <windows.h>
#include <objbase.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace boshiamy::installer::files {
namespace fs = std::filesystem;

struct DeploymentFaults {
    // Test seam called after a chunk has actually been copied. Returning false
    // simulates an I/O failure after partial destination mutation.
    std::function<bool(const std::string&, size_t)> after_copy_chunk;
    // Test seam for post-copy tampering; publication still re-hashes the full tree.
    std::function<bool(const std::string&, const std::filesystem::path&)> after_copy_verified;
    std::function<bool(const std::string&, const std::filesystem::path&, const std::filesystem::path&)> before_rename;
    std::function<bool(const std::string&, const std::filesystem::path&)> after_rename;
};

struct DeploymentRequest {
    fs::path package_root;
    fs::path install_root;
    std::string owner;
    std::string expected_old_version;
    std::string expected_old_cli_sha256;
    std::string expected_old_branding_sha256;
    const DeploymentFaults* faults = nullptr;
    bool include_machine_com_x64 = false;
};

struct DeploymentPaths {
    std::string version;
    std::string manifest_sha256;
    std::string installer_cli_sha256;
    std::string branding_sha256;
    fs::path version_root;
    fs::path x64_dll;
    fs::path x86_dll;
    bool has_x86 = true;
    fs::path branding_dll;
    fs::path installer_cli;
};
struct DeploymentSnapshot {
    std::string version;
    std::string manifest_sha256;
    std::string installer_cli_sha256;
    std::string branding_sha256;
    fs::path x64_dll;
    fs::path x86_dll;
    bool has_x86 = true;
    fs::path branding_dll;
    fs::path installer_cli;
};

namespace detail {
struct Json {
    enum class Kind { null_value, boolean, number, string, array, object } kind = Kind::null_value;
    bool boolean = false;
    uint64_t number = 0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string input) : input_(std::move(input)) {
        if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xef &&
            static_cast<unsigned char>(input_[1]) == 0xbb && static_cast<unsigned char>(input_[2]) == 0xbf) input_.erase(0, 3);
    }
    bool parse(Json* value) { skip(); if (!value || !read_value(value, 0)) return false; skip(); return at_ == input_.size(); }
private:
    std::string input_;
    size_t at_ = 0;
    void skip() { while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\n' || input_[at_] == '\r' || input_[at_] == '\t')) ++at_; }
    bool string(std::string* out) {
        if (at_ >= input_.size() || input_[at_++] != '"') return false;
        out->clear();
        while (at_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[at_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out->push_back(static_cast<char>(c)); continue; }
            if (at_ >= input_.size()) return false;
            const char e = input_[at_++];
            switch (e) {
            case '"': case '\\': case '/': out->push_back(e); break;
            case 'b': out->push_back('\b'); break; case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break; case 'r': out->push_back('\r'); break; case 't': out->push_back('\t'); break;
            case 'u': {
                if (at_ + 4 > input_.size()) return false;
                unsigned value = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    const char h = input_[at_++]; value <<= 4;
                    if (h >= '0' && h <= '9') value |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') value |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') value |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                if (value > 0x7f) return false; // Package metadata names and paths are ASCII.
                out->push_back(static_cast<char>(value)); break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool read_value(Json* out, unsigned depth) {
        if (depth > 32) return false;
        skip(); if (at_ >= input_.size()) return false;
        const char c = input_[at_];
        if (c == '"') { out->kind = Json::Kind::string; return string(&out->string); }
        if (c == '{') {
            ++at_; out->kind = Json::Kind::object; skip(); if (at_ < input_.size() && input_[at_] == '}') { ++at_; return true; }
            for (;;) {
                skip(); std::string key; if (!string(&key)) return false; skip(); if (at_ >= input_.size() || input_[at_++] != ':') return false;
                Json value; if (!read_value(&value, depth + 1) || !out->object.emplace(key, std::move(value)).second) return false;
                skip(); if (at_ >= input_.size()) return false; const char delimiter = input_[at_++];
                if (delimiter == '}') return true;
                if (delimiter != ',') return false;
            }
        }
        if (c == '[') {
            ++at_; out->kind = Json::Kind::array; skip(); if (at_ < input_.size() && input_[at_] == ']') { ++at_; return true; }
            for (;;) { Json value; if (!read_value(&value, depth + 1)) return false; out->array.push_back(std::move(value)); skip(); if (at_ >= input_.size()) return false; const char delimiter = input_[at_++]; if (delimiter == ']') return true; if (delimiter != ',') return false; }
        }
        if (input_.compare(at_, 4, "true") == 0) { at_ += 4; out->kind = Json::Kind::boolean; out->boolean = true; return true; }
        if (input_.compare(at_, 5, "false") == 0) { at_ += 5; out->kind = Json::Kind::boolean; return true; }
        if (input_.compare(at_, 4, "null") == 0) { at_ += 4; return true; }
        if (c < '0' || c > '9') return false;
        if(c=='0'&&at_+1<input_.size()&&input_[at_+1]>='0'&&input_[at_+1]<='9')return false;
        out->kind = Json::Kind::number; out->number = 0;
        while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') {
            const unsigned digit = static_cast<unsigned>(input_[at_++] - '0');
            if (out->number > (UINT64_MAX - digit) / 10) return false;
            out->number = out->number * 10 + digit;
        }
        return true;
    }
};

inline const Json* member(const Json& value, const char* key) {
    if (value.kind != Json::Kind::object) return nullptr;
    const auto found = value.object.find(key); return found == value.object.end() ? nullptr : &found->second;
}
inline bool string_member(const Json& value, const char* key, std::string* out) {
    const Json* item = member(value, key); if (!item || item->kind != Json::Kind::string) return false; *out = item->string; return true;
}
inline bool number_member(const Json& value, const char* key, uint64_t* out) {
    const Json* item = member(value, key); if (!item || item->kind != Json::Kind::number) return false; *out = item->number; return true;
}
inline bool safe_label(const std::string& value) {
    if (value.empty() || value.size() > 64 || value == "." || value == ".." || value.back()=='.' || value.find("..") != std::string::npos) return false;
    for (unsigned char c : value) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    std::string device=value.substr(0,value.find('.'));for(char& c:device)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
    if(device=="CON"||device=="PRN"||device=="AUX"||device=="NUL")return false;
    if(device.size()==4&&(device.rfind("COM",0)==0||device.rfind("LPT",0)==0)&&device[3]>='1'&&device[3]<='9')return false;
    return true;
}
inline bool valid_hash(const std::string& value) {
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool read_bytes(const fs::path& path, std::vector<uint8_t>* bytes) {
    std::ifstream in(path, std::ios::binary); if (!in) return false;
    in.seekg(0, std::ios::end); const auto end = in.tellg(); if (end < 0 || static_cast<uint64_t>(end) > 64ull * 1024 * 1024) return false;
    bytes->resize(static_cast<size_t>(end)); in.seekg(0); if (!bytes->empty()) in.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    return in.good() || in.eof();
}
inline uint16_t u16le(const std::vector<uint8_t>& data,size_t at) {
    return static_cast<uint16_t>(data[at])|static_cast<uint16_t>(static_cast<uint16_t>(data[at+1])<<8);
}
inline uint32_t u32le(const std::vector<uint8_t>& data,size_t at) {
    return static_cast<uint32_t>(data[at])|(static_cast<uint32_t>(data[at+1])<<8)|(static_cast<uint32_t>(data[at+2])<<16)|(static_cast<uint32_t>(data[at+3])<<24);
}
inline std::string sha256_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); if (!in) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD object_size = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::vector<uint8_t> object; std::array<uint8_t, 32> digest{};
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &got, 0) >= 0;
    if (ok) { object.resize(object_size); ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0; }
    std::array<char, 128 * 1024> buffer{};
    while (ok && in) { in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())); const auto count = in.gcount(); if (count > 0) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) >= 0; }
    ok = ok && in.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    static constexpr char hex[] = "0123456789abcdef"; std::string result; result.reserve(64);
    for (uint8_t b : digest) { result.push_back(hex[b >> 4]); result.push_back(hex[b & 15]); } return result;
}
inline std::string sha256_bytes(const std::string& data) {
    if (data.size() > static_cast<size_t>((std::numeric_limits<ULONG>::max)())) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::vector<uint8_t> object; std::array<uint8_t, 32> digest{};
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &got, 0) >= 0;
    if (ok) { object.resize(object_size); ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0; }
    if (ok && !data.empty()) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
        static_cast<ULONG>(data.size()), 0) >= 0;
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    static constexpr char hex[] = "0123456789abcdef"; std::string result; result.reserve(64);
    for (uint8_t b : digest) { result.push_back(hex[b >> 4]); result.push_back(hex[b & 15]); }
    return result;
}
inline bool reparse(const fs::path& path);
inline std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); if (!in) return {};
    in.seekg(0,std::ios::end);const auto end=in.tellg();if(end<0||static_cast<uint64_t>(end)>16ull*1024*1024)return {};
    in.seekg(0,std::ios::beg);std::string value(static_cast<size_t>(end),'\0');if(!value.empty())in.read(value.data(),static_cast<std::streamsize>(value.size()));
    return in.good()||in.eof()?value:std::string{};
}
inline bool write_text_new(const fs::path& path, const std::string& text) {
    if(reparse(path.parent_path()))return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    size_t offset=0;bool ok=true;
    while(offset<text.size()){
        const DWORD chunk=static_cast<DWORD>(std::min<size_t>(text.size()-offset,1u<<20));DWORD written=0;
        if(!WriteFile(file,text.data()+offset,chunk,&written,nullptr)||written!=chunk){ok=false;break;}
        offset+=written;
    }
    if(ok)ok=FlushFileBuffers(file)!=0;
    CloseHandle(file);return ok;
}
inline bool reparse(const fs::path& path) {
    const DWORD attr = GetFileAttributesW(path.c_str()); return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
inline bool absolute_no_reparse(const fs::path& path, bool must_exist) {
    if (!path.is_absolute()) return false;
    fs::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component; std::error_code ec; const auto status = fs::symlink_status(current, ec);
        if (ec) { if (must_exist) return false; continue; }
        if (fs::exists(status) && (fs::is_symlink(status) || reparse(current))) return false;
    }
    return true;
}
inline bool is_within(const fs::path& root, const fs::path& candidate) {
    const auto a = fs::absolute(root).lexically_normal().wstring();
    const auto b = fs::absolute(candidate).lexically_normal().wstring();
    if (b.size() <= a.size() || _wcsnicmp(a.c_str(), b.c_str(), a.size()) != 0) return false;
    return b[a.size()] == L'\\' || b[a.size()] == L'/';
}
inline bool remove_tree_no_reparse(const fs::path& path) {
    if(reparse(path))return false;
    std::error_code ec;
    if(fs::is_directory(path,ec)) {
        if(ec)return false;
        for(fs::directory_iterator it(path,fs::directory_options::none,ec),end;it!=end&&!ec;it.increment(ec)) {
            const auto child=it->path();if(reparse(child))return false;
            if(it->is_directory(ec)){if(ec||!remove_tree_no_reparse(child))return false;}
            else if(it->is_regular_file(ec)){if(ec||!fs::remove(child,ec)||ec)return false;}
            else return false;
        }
        if(ec)return false;
    } else if(!fs::is_regular_file(path,ec)||ec)return false;
    return fs::remove(path,ec)&&!ec;
}
inline bool safe_relative(const std::string& value) {
    if (value.empty() || value.front() == '/' || value.find('\\') != std::string::npos || value.find(':') != std::string::npos) return false;
    size_t at = 0;
    while (at <= value.size()) { const size_t end = value.find('/', at); const std::string part = value.substr(at, end == std::string::npos ? std::string::npos : end - at); if (part.empty() || part == "." || part == "..") return false; if (end == std::string::npos) break; at = end + 1; }
    return true;
}
inline std::wstring widen_ascii(const std::string& value) { return std::wstring(value.begin(), value.end()); }
inline bool pe_info(const fs::path& path, uint16_t* machine, uint16_t* magic, bool* dll, uint32_t* entry, std::vector<std::pair<std::string,uint32_t>>* sections) {
    std::vector<uint8_t> b; if (!read_bytes(path, &b) || b.size() < 64 || b[0] != 'M' || b[1] != 'Z') return false;
    const uint32_t pe = u32le(b,0x3c);
    if (pe > b.size() - 24 || std::memcmp(&b[pe], "PE\0\0", 4) != 0) return false;
    *machine = u16le(b,pe+4); const uint16_t count = u16le(b,pe+6);
    const uint16_t opt = u16le(b,pe+20); const uint16_t flags = u16le(b,pe+22);
    const size_t optional = static_cast<size_t>(pe) + 24; if (opt < 20 || optional + opt > b.size()) return false;
    *magic = u16le(b,optional); *entry = u32le(b,optional+16); *dll = (flags & 0x2000) != 0;
    const size_t offset = optional + opt; if (offset + static_cast<size_t>(count) * 40 > b.size()) return false;
    sections->clear();
    for (unsigned i = 0; i < count; ++i) { const size_t pos = offset + i * 40; std::string name(reinterpret_cast<char*>(&b[pos]), 8); const auto zero = name.find('\0'); if (zero != std::string::npos) name.resize(zero); const uint32_t chars = u32le(b,pos+36); sections->emplace_back(name, chars); }
    return true;
}
inline bool architecture(const fs::path& path, bool x64, bool want_dll) {
    uint16_t machine=0, magic=0; bool dll=false; uint32_t entry=0; std::vector<std::pair<std::string,uint32_t>> sections;
    if (!pe_info(path,&machine,&magic,&dll,&entry,&sections)) return false;
    return machine == (x64 ? 0x8664 : 0x014c) && magic == (x64 ? 0x020b : 0x010b) && dll == want_dll;
}
inline bool resource_only_branding(const fs::path& path) {
    uint16_t machine=0,magic=0;bool dll=false;uint32_t entry=0;std::vector<std::pair<std::string,uint32_t>> sections;
    return pe_info(path,&machine,&magic,&dll,&entry,&sections)&&dll&&entry==0&&sections.size()==1&&sections[0].first==".rsrc"&&
        (sections[0].second&0x20000000)==0&&((machine==0x014c&&magic==0x010b)||(machine==0x8664&&magic==0x020b));
}
inline bool validate_recommendation_provenance(const fs::path& path,const std::string& asset_hash) {
    Json doc; if(!JsonParser(read_text(path)).parse(&doc)||doc.kind!=Json::Kind::object)return false;
    uint64_t schema=0,entries=0;std::string evidence,coverage,asset,scope;
    if(!number_member(doc,"schema",&schema)||schema!=1||!string_member(doc,"evidence",&evidence)||evidence.empty()||
       !string_member(doc,"coverage",&coverage)||coverage.empty()||!number_member(doc,"entries",&entries)||entries==0||
       !string_member(doc,"asset_sha256",&asset)||asset!=asset_hash||!string_member(doc,"scope",&scope)||scope.empty())return false;
    const Json* source=member(doc,"source");if(!source)return false;
    std::string kind,file,hash;uint64_t page=0;
    if(!string_member(*source,"kind",&kind)||kind.empty()||!string_member(*source,"file",&file)||!safe_relative(file)||
       !string_member(*source,"sha256",&hash)||!valid_hash(hash)||!number_member(*source,"page",&page)||!page)return false;
    const Json* runtime=member(doc,"runtime_evidence");const Json* sections=member(doc,"sections");
    if(!runtime||runtime->kind!=Json::Kind::boolean||!sections||sections->kind!=Json::Kind::array||sections->array.empty())return false;
    for(const auto& item:sections->array)if(item.kind!=Json::Kind::string||item.string.empty())return false;
    return true;
}
struct Row { std::string path, hash; uint64_t bytes=0; };
struct Package { std::string version; std::map<std::string, Row> files; std::string cli_hash, branding_hash, manifest_sha256; bool has_x86=true; };
inline const std::vector<std::string>& arch_files() {
    static const std::vector<std::string> names={"BoshiamyTsf.dll","LiuKaiLookup.exe","LiuKaiSettings.exe","dictionary.tsv","readings.tsv","recommended-codes.tsv","recommended-codes-source.json","phonetic-candidates.tsv","phonetic-candidates-source.json","THIRD_PARTY_NOTICES.md"}; return names;
}
inline const std::vector<std::string>& legacy_arch_files() {
    static const std::vector<std::string> names={"BoshiamyTsf.dll","LiuKaiLookup.exe","LiuKaiSettings.exe","dictionary.tsv","readings.tsv","recommended-codes.tsv","recommended-codes-source.json","THIRD_PARTY_NOTICES.md"}; return names;
}
inline std::string version_prefix(const std::string& version, const std::string& arch) { return "versions/" + version + "/" + arch + "/"; }
inline std::string version_owner_contents(const std::string& owner,const std::string& version);
inline bool parse_package(const fs::path& root, Package* package) {
    if (!absolute_no_reparse(root, true) || !fs::is_directory(root)) return false;
    const std::string manifest_image = read_text(root / L"manifest.json");
    const std::string manifest_sha256 = sha256_bytes(manifest_image);
    if (manifest_image.empty() || !valid_hash(manifest_sha256)) return false;
    Json doc; if (!JsonParser(manifest_image).parse(&doc)) return false;
    std::string format, version, cli, branding, scope;
    if (doc.object.size()!=7 || !string_member(doc,"format",&format) || format != "liukai-windows-package-v1" || !string_member(doc,"version",&version) || !safe_label(version) ||
        !string_member(doc,"installer_cli",&cli) || cli != "LiuKaiInstaller.exe" || !string_member(doc,"branding",&branding) || branding != "shared/branding.dll" ||
        !string_member(doc,"manifest_sha256_scope",&scope) || scope != "All staged files except manifest.json itself.") return false;
    const Json* arches = member(doc,"architectures");
    if (!arches || arches->kind != Json::Kind::array || (arches->array.size()!=1 && arches->array.size()!=2) ||
        arches->array[0].kind!=Json::Kind::string || arches->array[0].string!="x64" ||
        (arches->array.size()==2 && (arches->array[1].kind!=Json::Kind::string || arches->array[1].string!="x86"))) return false;
    const bool has_x86=arches->array.size()==2;
    const Json* rows = member(doc,"files"); if (!rows || rows->kind != Json::Kind::array || rows->array.size()!=(has_x86?22u:12u)) return false;
    Package result; result.version=version; result.manifest_sha256=manifest_sha256; result.has_x86=has_x86;
    for (const auto& item : rows->array) {
        Row row; if (item.object.size()!=3 || !string_member(item,"path",&row.path) || !string_member(item,"sha256",&row.hash) || !valid_hash(row.hash) || !number_member(item,"bytes",&row.bytes) || !safe_relative(row.path)) return false;
        if (!result.files.emplace(row.path,row).second) return false;
    }
    std::set<std::string> expected={"LiuKaiInstaller.exe","shared/branding.dll"};
    for (const auto& name : arch_files()) expected.insert(version_prefix(version,"x64")+name);
    if(has_x86)for (const auto& name : arch_files()) expected.insert(version_prefix(version,"x86")+name);
    std::set<std::string> actual; for (const auto& row : result.files) actual.insert(row.first); if (expected != actual) return false;
    for (const auto& pair : result.files) {
        const fs::path candidate = root / widen_ascii(pair.first);
        if (!is_within(root,candidate) || !absolute_no_reparse(candidate,true) || !fs::is_regular_file(candidate)) return false;
        std::error_code ec; if (fs::file_size(candidate,ec) != pair.second.bytes || ec || sha256_file(candidate) != pair.second.hash) return false;
    }
    if (!architecture(root/L"LiuKaiInstaller.exe",true,false)) return false;
    if (!architecture(root/L"versions"/widen_ascii(version)/L"x64"/L"BoshiamyTsf.dll",true,true) ||
        !architecture(root/L"versions"/widen_ascii(version)/L"x64"/L"LiuKaiLookup.exe",true,false) || !architecture(root/L"versions"/widen_ascii(version)/L"x64"/L"LiuKaiSettings.exe",true,false)) return false;
    if(has_x86&&(!architecture(root/L"versions"/widen_ascii(version)/L"x86"/L"BoshiamyTsf.dll",false,true) ||
        !architecture(root/L"versions"/widen_ascii(version)/L"x86"/L"LiuKaiLookup.exe",false,false) || !architecture(root/L"versions"/widen_ascii(version)/L"x86"/L"LiuKaiSettings.exe",false,false)))return false;
    if(!resource_only_branding(root/L"shared"/L"branding.dll"))return false;
    for (const auto& name : {"dictionary.tsv","readings.tsv","recommended-codes.tsv","recommended-codes-source.json","phonetic-candidates.tsv","phonetic-candidates-source.json","THIRD_PARTY_NOTICES.md"}) {
        const auto p64=result.files.at(version_prefix(version,"x64")+name).hash;
        if(has_x86){const auto p86=result.files.at(version_prefix(version,"x86")+name).hash; if (p64 != p86) return false;}
    }
    if(!validate_recommendation_provenance(root/L"versions"/widen_ascii(version)/L"x64"/L"recommended-codes-source.json",result.files.at(version_prefix(version,"x64")+"recommended-codes.tsv").hash))return false;
    // Reject files or directories not described by the exact package manifest.
    std::set<std::string> expected_dirs={"shared","versions","versions/"+version,"versions/"+version+"/x64"};
    if(has_x86)expected_dirs.insert("versions/"+version+"/x86");
    std::set<std::string> seen_files={"manifest.json"};
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root,fs::directory_options::none,ec),end; it!=end && !ec; it.increment(ec)) {
        if (reparse(it->path())) return false;
        const auto rel=it->path().lexically_relative(root).generic_string();
        if (it->is_directory(ec)) { if (!expected_dirs.count(rel)) return false; }
        else if (it->is_regular_file(ec)) { if (rel!="manifest.json" && !result.files.count(rel)) return false; seen_files.insert(rel); }
        else return false;
    }
    if (ec || seen_files.size()!=result.files.size()+1) return false;
    result.cli_hash=result.files.at("LiuKaiInstaller.exe").hash; result.branding_hash=result.files.at("shared/branding.dll").hash;
    *package=std::move(result); return true;
}
inline bool validate_installed_version(const fs::path& version_root,const std::string& owner,const std::string& version,bool* has_x86_out=nullptr) {
    if(!safe_label(version)||!fs::is_directory(version_root)||reparse(version_root)||
       read_text(version_root/L".liukai-version-owner")!=version_owner_contents(owner,version)) return false;
    Json doc; if(!JsonParser(read_text(version_root/L"package-manifest.json")).parse(&doc))return false;
    std::string format,manifest_version,cli,branding,scope;
    if(doc.object.size()!=7||!string_member(doc,"format",&format)||format!="liukai-windows-package-v1"||!string_member(doc,"version",&manifest_version)||manifest_version!=version||
       !string_member(doc,"installer_cli",&cli)||cli!="LiuKaiInstaller.exe"||!string_member(doc,"branding",&branding)||branding!="shared/branding.dll"||
       !string_member(doc,"manifest_sha256_scope",&scope)||scope!="All staged files except manifest.json itself.")return false;
    const Json* arches=member(doc,"architectures");if(!arches||arches->kind!=Json::Kind::array||(arches->array.size()!=1&&arches->array.size()!=2)||arches->array[0].kind!=Json::Kind::string||arches->array[0].string!="x64"||(arches->array.size()==2&&(arches->array[1].kind!=Json::Kind::string||arches->array[1].string!="x86")))return false;
    const bool has_x86=arches->array.size()==2;
    const Json* rows=member(doc,"files");if(!rows||rows->kind!=Json::Kind::array||
       (rows->array.size()!=(has_x86?22u:12u)&&rows->array.size()!=(has_x86?18u:10u)))return false;
    std::set<std::string> expected_current;for(const auto& name:arch_files())expected_current.insert(name);
    std::set<std::string> expected_legacy;for(const auto& name:legacy_arch_files())expected_legacy.insert(name);
    std::set<std::string> expected_rows={"LiuKaiInstaller.exe","shared/branding.dll"};
    const bool modern_layout = rows->array.size()==(has_x86?22u:12u);
    const auto& expected=modern_layout?expected_current:expected_legacy;
    if(!modern_layout&&rows->array.size()!=(has_x86?18u:10u))return false;
    for(const auto& name:expected)expected_rows.insert(version_prefix(version,"x64")+name);
    if(has_x86)for(const auto& name:expected)expected_rows.insert(version_prefix(version,"x86")+name);
    std::set<std::string> seen_rows;std::map<std::string,std::string> hashes;
    for(const auto& item:rows->array){
        std::string path,hash;uint64_t bytes=0;if(item.object.size()!=3||!string_member(item,"path",&path)||!string_member(item,"sha256",&hash)||!valid_hash(hash)||!number_member(item,"bytes",&bytes)||!safe_relative(path))return false;
        if(!seen_rows.insert(path).second||!expected_rows.count(path))return false;
        for(const auto& arch:{std::string("x64"),std::string("x86")}){
            const std::string prefix=version_prefix(version,arch);if(path.rfind(prefix,0)!=0)continue;
            const std::string name=path.substr(prefix.size());if(!expected.count(name)||!hashes.emplace(arch+"/"+name,hash).second)return false;
            const auto file=version_root/widen_ascii(arch)/widen_ascii(name);std::error_code ec;if(!fs::is_regular_file(file)||fs::file_size(file,ec)!=bytes||ec||sha256_file(file)!=hash)return false;
        }
    }
    const size_t files_per_arch=expected.size();
    if(hashes.size()!=(has_x86?files_per_arch*2:files_per_arch)||seen_rows!=expected_rows)return false;
    for(const auto& name:{std::string("BoshiamyTsf.dll"),std::string("LiuKaiLookup.exe"),std::string("LiuKaiSettings.exe")}){
        const bool dll=name=="BoshiamyTsf.dll";
        if(!architecture(version_root/L"x64"/widen_ascii(name),true,dll)||(has_x86&&!architecture(version_root/L"x86"/widen_ascii(name),false,dll)))return false;
    }
    if(has_x86)for(const auto& name:{std::string("dictionary.tsv"),std::string("readings.tsv"),std::string("recommended-codes.tsv"),std::string("recommended-codes-source.json"),std::string("phonetic-candidates.tsv"),std::string("phonetic-candidates-source.json"),std::string("THIRD_PARTY_NOTICES.md")})
        if(expected.count(name)&&hashes.at("x64/"+name)!=hashes.at("x86/"+name))return false;
    if(!validate_recommendation_provenance(version_root/L"x64"/L"recommended-codes-source.json",hashes.at("x64/recommended-codes.tsv")))return false;
    std::set<std::string> allowed={".liukai-version-owner","package-manifest.json","x64"};if(has_x86)allowed.insert("x86");
    std::error_code ec;for(fs::directory_iterator it(version_root,ec),end;it!=end&&!ec;it.increment(ec))if(!allowed.count(it->path().filename().string())||reparse(it->path()))return false;if(ec)return false;
    const std::vector<std::string> architectures=has_x86?std::vector<std::string>{"x64","x86"}:std::vector<std::string>{"x64"};
    for(const auto& arch:architectures){
        const auto dir=version_root/widen_ascii(arch);if(!fs::is_directory(dir)||reparse(dir))return false;
        std::set<std::string> seen;for(fs::directory_iterator it(dir,ec),end;it!=end&&!ec;it.increment(ec)){if(!it->is_regular_file()||reparse(it->path()))return false;seen.insert(it->path().filename().string());}
        if(ec||seen!=expected)return false;
    }
    if(has_x86_out)*has_x86_out=has_x86;
    return true;
}
inline std::string owner_contents(const std::string& owner) { return "liukai-owner-v1\n"+owner+"\n"; }
inline std::string version_owner_contents(const std::string& owner,const std::string& version) { return "liukai-version-owner-v1\n"+owner+"\n"+version+"\n"; }
inline std::string state_contents(const std::string& owner,const std::string& version,const std::string& cli,const std::string& branding) {
    return "liukai-install-state-v1\nowner="+owner+"\ncurrent="+version+"\ncli_sha256="+cli+"\nbranding_sha256="+branding+"\n";
}
inline bool parse_state(const std::string& text,std::string* owner,std::string* version,std::string* cli,std::string* branding) {
    std::istringstream in(text); std::string line; if (!std::getline(in,line)||line!="liukai-install-state-v1") return false;
    std::map<std::string,std::string> fields;
    while (std::getline(in,line)) { if (line.empty()) continue; const auto eq=line.find('='); if (eq==std::string::npos||!fields.emplace(line.substr(0,eq),line.substr(eq+1)).second) return false; }
    if (fields.size()!=4 || !fields.count("owner") || !fields.count("current") || !fields.count("cli_sha256") || !fields.count("branding_sha256")) return false;
    *owner=fields["owner"];*version=fields["current"];*cli=fields["cli_sha256"];*branding=fields["branding_sha256"];
    return safe_label(*version)&&valid_hash(*cli)&&valid_hash(*branding);
}
inline bool safe_remove_tree(const fs::path& root,const fs::path& target,const std::string& marker_name,const std::string& marker_value) {
    const auto parent=fs::absolute(target).lexically_normal().parent_path();
    const auto root_abs=fs::absolute(root).lexically_normal();
    if (!is_within(root,target) || (parent!=root_abs && parent!=root_abs/L"versions")) return false;
    if (read_text(target/widen_ascii(marker_name))!=marker_value || reparse(target)) return false;
    return remove_tree_no_reparse(target);
}
inline std::string unique_token() {
    GUID id{}; if (CoCreateGuid(&id)!=S_OK) return {};
    wchar_t text[40]{}; if (!StringFromGUID2(id,text,40)) return {};
    std::wstring w(text); std::string out; for (wchar_t c:w) if (c!=L'{'&&c!=L'}'&&c!=L'-') out.push_back(static_cast<char>(c));
    for (char& c:out) if (c>='A'&&c<='F') c=static_cast<char>(c-'A'+'a');
    return out;
}
inline bool copy_verified(const fs::path& source,const fs::path& destination,const Row& expected,const std::string& relative,const DeploymentFaults* faults) {
    if (sha256_file(source)!=expected.hash||reparse(source)||reparse(destination.parent_path())) return false;
    std::ifstream in(source,std::ios::binary);if(!in)return false;
    HANDLE output=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(output==INVALID_HANDLE_VALUE)return false;
    std::array<char,128*1024> buffer{}; size_t chunk=0; uint64_t total=0;
    while (in) {
        in.read(buffer.data(),static_cast<std::streamsize>(buffer.size())); const auto count=in.gcount(); if (count<=0) break;
        size_t offset=0;
        while(offset<static_cast<size_t>(count)){DWORD written=0;const DWORD amount=static_cast<DWORD>(static_cast<size_t>(count)-offset);if(!WriteFile(output,buffer.data()+offset,amount,&written,nullptr)||!written){CloseHandle(output);return false;}offset+=written;}
        total+=static_cast<uint64_t>(count); ++chunk;
        if (faults && faults->after_copy_chunk && !faults->after_copy_chunk(relative,chunk)) {CloseHandle(output);return false;}
    }
    const bool read_ok=in.eof();const bool flush_ok=FlushFileBuffers(output)!=0;CloseHandle(output);
    return read_ok&&flush_ok&&total==expected.bytes&&sha256_file(source)==expected.hash&&sha256_file(destination)==expected.hash;
}
inline bool rename_checked(const fs::path& from,const fs::path& to,const std::string& relative,const DeploymentFaults* faults,
                           bool* published=nullptr,const std::string& expected_source_hash={}) {
    if(published)*published=false;
    if (faults && faults->before_rename && !faults->before_rename(relative,from,to)) return false;
    // Fault hooks can model a concurrent writer. Revalidate the staged bytes
    // after that seam and immediately before publication.
    if(!expected_source_hash.empty()&&sha256_file(from)!=expected_source_hash)return false;
    if(reparse(from)||reparse(from.parent_path())||reparse(to)||reparse(to.parent_path())||fs::exists(to))return false;
    std::error_code ec; fs::rename(from,to,ec); if(ec)return false;
    if(published)*published=true;
    return !faults||!faults->after_rename||faults->after_rename(relative,to);
}
} // namespace detail

class PreparedDeployment;
class FileDeployment {
public:
    // Reads the owned deployment's current expected-old tokens. prepare() rechecks
    // the full package and install tree before apply(), so this is not a lock.
    static HRESULT inspect(const fs::path& install_root,const std::string& owner,DeploymentSnapshot* out);
    static HRESULT prepare(const DeploymentRequest& request,std::unique_ptr<PreparedDeployment>* out);
};

class PreparedDeployment {
    friend class FileDeployment;
    DeploymentRequest request_;
    detail::Package package_;
    DeploymentPaths paths_;
    fs::path txn_;
    std::string token_, old_state_, old_cli_hash_, old_brand_hash_;
    bool fresh_=false, applied_=false, recovery_pending_=false, finished_=false, created_version_=false, no_op_=false;
    bool cli_changed_=false,brand_changed_=false,state_changed_=false,state_backup_moved_=false,owner_created_=false;
    std::string result_state_;
    PreparedDeployment(const DeploymentRequest& r,detail::Package p) : request_(r), package_(std::move(p)) {
        paths_.version=package_.version; paths_.manifest_sha256=package_.manifest_sha256;
        paths_.installer_cli_sha256=package_.cli_hash; paths_.branding_sha256=package_.branding_hash;
        paths_.has_x86=package_.has_x86;
        paths_.version_root=request_.install_root/L"versions"/detail::widen_ascii(package_.version);
        paths_.x64_dll=paths_.version_root/L"x64"/L"BoshiamyTsf.dll";
        if(package_.has_x86)paths_.x86_dll=paths_.version_root/L"x86"/L"BoshiamyTsf.dll";
        paths_.branding_dll=request_.install_root/L"shared"/L"branding.dll"; paths_.installer_cli=request_.install_root/L"LiuKaiInstaller.exe";
    }
    HRESULT make_dirs(const fs::path& path) { std::error_code ec; fs::create_directories(path,ec); return ec?HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED):S_OK; }
    HRESULT clean_txn() {
        if (txn_.empty()||!fs::exists(txn_)) return S_OK;
        const std::set<std::wstring> top={L".txn-owner",L"version",L"new-cli",L"new-branding",L"old-cli",L"old-branding",L"old-state",L"new-state",L"new-owner"};
        std::error_code ec;
        for(fs::recursive_directory_iterator it(txn_,fs::directory_options::none,ec),end;it!=end&&!ec;it.increment(ec)){
            if(detail::reparse(it->path()))return E_ACCESSDENIED;
            const auto rel=it->path().lexically_relative(txn_).generic_wstring();
            if(rel.find(L'/')==std::wstring::npos){if(!top.count(rel))return E_ACCESSDENIED;continue;}
            const auto first=rel.substr(0,rel.find(L'/'));
            if(first!=L"version")return E_ACCESSDENIED;
            const auto tail=rel.substr(8); // after "version/"
            if(tail==L".liukai-version-owner"||tail==L"package-manifest.json"||tail==L"x64"||tail==L"x86")continue;
            const auto slash=tail.find(L'/');if(slash==std::wstring::npos)return E_ACCESSDENIED;
            const auto arch=tail.substr(0,slash),name=tail.substr(slash+1);
            if((arch!=L"x64"&&arch!=L"x86")||name.find(L'/')!=std::wstring::npos)return E_ACCESSDENIED;
            bool known=false;for(const auto& item:detail::arch_files())if(name==detail::widen_ascii(item))known=true;
            if(!known||it->is_directory(ec)||ec)return E_ACCESSDENIED;
        }
        if(ec)return E_ACCESSDENIED;
        if (!detail::safe_remove_tree(request_.install_root,txn_,".txn-owner",request_.owner+"\n"+token_+"\n")) return E_ACCESSDENIED;
        return S_OK;
    }
    HRESULT undo_applied() {
        HRESULT result=S_OK;
        auto record_failure=[&](){result=E_ACCESSDENIED;};
        const auto state=request_.install_root/L".liukai-install-state";
        const auto state_backup=txn_/L"old-state";
        if(state_backup_moved_||fs::exists(state_backup)) {
            if(fs::exists(state)) {
                const auto current=detail::read_text(state);
                if(state_changed_&&current==result_state_) { std::error_code ec; fs::remove(state,ec); if(ec)record_failure(); else state_changed_=false; }
                else if(current!=old_state_) record_failure();
            }
            if(!fs::exists(state)&&fs::exists(state_backup)) { if(!detail::rename_checked(state_backup,state,"rollback-state",nullptr))record_failure(); else {state_backup_moved_=false;state_changed_=false;} }
            else if(fs::exists(state)&&detail::read_text(state)==old_state_) {state_backup_moved_=false;state_changed_=false;}
        } else if(state_changed_) {
            if(fs::exists(state)&&detail::read_text(state)==result_state_) { std::error_code ec; fs::remove(state,ec); if(ec)record_failure(); else state_changed_=false; }
            else record_failure();
        }
        const auto cli=request_.install_root/L"LiuKaiInstaller.exe"; const auto cli_new=package_.cli_hash;
        const auto cli_backup=txn_/L"old-cli";
        if(fs::exists(cli_backup)) {
            if(fs::exists(cli)) { const auto hash=detail::sha256_file(cli); if(cli_changed_&&hash==cli_new) {std::error_code ec;fs::remove(cli,ec);if(ec)record_failure();else cli_changed_=false;} else if(hash!=old_cli_hash_)record_failure(); }
            if(!fs::exists(cli)&&!detail::rename_checked(cli_backup,cli,"rollback-cli",nullptr))record_failure();
            else if(fs::exists(cli)&&detail::sha256_file(cli)==old_cli_hash_){cli_changed_=false;}
        } else if(cli_changed_) { if(fs::exists(cli)&&detail::sha256_file(cli)==cli_new) {std::error_code ec;fs::remove(cli,ec);if(ec)record_failure();else cli_changed_=false;} else record_failure(); }
        const auto brand=request_.install_root/L"shared"/L"branding.dll";
        const auto brand_backup=txn_/L"old-branding";
        if(fs::exists(brand_backup)) {
            if(fs::exists(brand)) { const auto hash=detail::sha256_file(brand); if(brand_changed_&&hash==package_.branding_hash) {std::error_code ec;fs::remove(brand,ec);if(ec)record_failure();else brand_changed_=false;} else if(hash!=old_brand_hash_)record_failure(); }
            if(!fs::exists(brand)&&!detail::rename_checked(brand_backup,brand,"rollback-branding",nullptr))record_failure();
            else if(fs::exists(brand)&&detail::sha256_file(brand)==old_brand_hash_)brand_changed_=false;
        } else if(brand_changed_) { if(fs::exists(brand)&&detail::sha256_file(brand)==package_.branding_hash) {std::error_code ec;fs::remove(brand,ec);if(ec)record_failure();else brand_changed_=false;} else record_failure(); }
        if(created_version_&&fs::exists(paths_.version_root)) {
            if(!detail::validate_installed_version(paths_.version_root,request_.owner,package_.version)||!detail::safe_remove_tree(request_.install_root,paths_.version_root,".liukai-version-owner",detail::version_owner_contents(request_.owner,package_.version))) record_failure(); else created_version_=false;
        }
        const auto owner=request_.install_root/L".liukai-owner";
        if(fresh_&&owner_created_&&fs::exists(owner)){if(detail::read_text(owner)==detail::owner_contents(request_.owner)){std::error_code ec;fs::remove(owner,ec);if(ec)record_failure();else owner_created_=false;}else record_failure();}
        const auto shared=request_.install_root/L"shared"; if(fs::is_directory(shared)){std::error_code ec;if(fs::is_empty(shared,ec)&&!ec)fs::remove(shared,ec);if(ec)record_failure();}
        const auto versions=request_.install_root/L"versions"; if(fs::is_directory(versions)){std::error_code ec;if(fs::is_empty(versions,ec)&&!ec)fs::remove(versions,ec);if(ec)record_failure();}
        if(FAILED(result))return result; // Keep backups/journal if any owned-state check refused cleanup.
        const HRESULT cleanup=clean_txn(); if(FAILED(cleanup))return cleanup;
        if(fresh_&&fs::is_directory(request_.install_root)){std::error_code ec;fs::remove(request_.install_root,ec);if(ec)return E_ACCESSDENIED;}
        applied_=false; return S_OK;
    }
    HRESULT abort_apply(HRESULT cause) {
        HRESULT cleanup=E_FAIL;
        try { cleanup=undo_applied(); }
        catch(...) { recovery_pending_=true; return E_ACCESSDENIED; }
        if(FAILED(cleanup)){recovery_pending_=true;return cleanup;} return cause;
    }
public:
    ~PreparedDeployment() = default;
    const DeploymentPaths& paths() const { return paths_; }
    HRESULT bind_transaction_token(const std::string& token) {
        if(applied_||recovery_pending_||finished_||!token_.empty()||!detail::safe_label(token))return E_INVALIDARG;
        token_=token;
        return S_OK;
    }
    HRESULT apply() {
        if(applied_||recovery_pending_||finished_)return E_UNEXPECTED;
        try {
        // Re-read the package and the complete owned tree immediately before mutation.
        std::unique_ptr<PreparedDeployment> rechecked;
        const HRESULT preflight=FileDeployment::prepare(request_,&rechecked);
        if(FAILED(preflight)||!rechecked||rechecked->package_.version!=package_.version||rechecked->package_.cli_hash!=package_.cli_hash||rechecked->package_.branding_hash!=package_.branding_hash||rechecked->package_.manifest_sha256!=package_.manifest_sha256)return FAILED(preflight)?preflight:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        const auto root=request_.install_root;
        std::error_code ec; fresh_=!fs::exists(root);
        if(!fresh_&&request_.expected_old_version==package_.version&&
           request_.expected_old_cli_sha256==package_.cli_hash&&
           request_.expected_old_branding_sha256==package_.branding_hash&&
           detail::sha256_file(paths_.version_root/L"package-manifest.json")==package_.manifest_sha256&&
           detail::validate_installed_version(paths_.version_root,request_.owner,package_.version)) {
            // The caller supplied the exact currently-installed expected-old
            // tokens, and this version tree is the exact package image. Treat
            // this as an idempotent retry: do not create a txn or republish any
            // file, but let the outer coordinator record a no-mutation TSF
            // transaction and durable commit.
            no_op_=true;
            applied_=true;
            return S_OK;
        }
        if(fresh_) { if(!request_.expected_old_version.empty()||!request_.expected_old_cli_sha256.empty()||!request_.expected_old_branding_sha256.empty())return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH); if(!fs::create_directory(root,ec)||ec)return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); }
        else {
            if(!fs::is_directory(root)||detail::read_text(root/L".liukai-owner")!=detail::owner_contents(request_.owner))return E_ACCESSDENIED;
            std::string owner,version,cli,brand; old_state_=detail::read_text(root/L".liukai-install-state");
            if(!detail::parse_state(old_state_,&owner,&version,&cli,&brand)||owner!=request_.owner||version!=request_.expected_old_version||cli!=request_.expected_old_cli_sha256||brand!=request_.expected_old_branding_sha256||detail::sha256_file(root/L"LiuKaiInstaller.exe")!=cli||!detail::architecture(root/L"LiuKaiInstaller.exe",true,false)||detail::sha256_file(root/L"shared"/L"branding.dll")!=brand||!detail::resource_only_branding(root/L"shared"/L"branding.dll"))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            old_cli_hash_=cli;old_brand_hash_=brand;
        }
        if(token_.empty())token_=detail::unique_token();
        if(token_.empty()){if(fresh_)fs::remove(root,ec);return E_FAIL;}
        txn_=root/(L".txn-"+detail::widen_ascii(token_));
        if(!fs::create_directory(txn_,ec)||ec){if(fresh_)fs::remove(root,ec);return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);}
        if(!detail::write_text_new(txn_/L".txn-owner",request_.owner+"\n"+token_+"\n"))return abort_apply(E_FAIL);
        const auto stage_version=txn_/L"version"; if(FAILED(make_dirs(stage_version/L"x64"))||(package_.has_x86&&FAILED(make_dirs(stage_version/L"x86"))))return abort_apply(E_FAIL);
        if(!detail::write_text_new(stage_version/L".liukai-version-owner",detail::version_owner_contents(request_.owner,package_.version)))return abort_apply(E_FAIL);
        const std::vector<std::string> architectures=package_.has_x86?std::vector<std::string>{"x64","x86"}:std::vector<std::string>{"x64"};
        for(const auto& arch:architectures) for(const auto& name:detail::arch_files()) {
            if(detail::sha256_file(request_.package_root/L"manifest.json")!=package_.manifest_sha256)
                return abort_apply(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
            const auto relative=detail::version_prefix(package_.version,arch)+name; const auto row=package_.files.at(relative);
            const auto dest=stage_version/detail::widen_ascii(arch)/detail::widen_ascii(name);
            if(!detail::copy_verified(request_.package_root/detail::widen_ascii(relative),dest,row,relative,request_.faults))return abort_apply(E_FAIL);
            if(request_.faults&&request_.faults->after_copy_verified&&!request_.faults->after_copy_verified(relative,dest))return abort_apply(E_FAIL);
        }
        const auto manifest_copy=stage_version/L"package-manifest.json";
        if(detail::sha256_file(request_.package_root/L"manifest.json")!=package_.manifest_sha256)
            return abort_apply(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
        if(!detail::copy_verified(request_.package_root/L"manifest.json",manifest_copy,detail::Row{"manifest.json",detail::sha256_file(request_.package_root/L"manifest.json"),fs::file_size(request_.package_root/L"manifest.json")},"package-manifest.json",request_.faults))return abort_apply(E_FAIL);
        if(request_.faults&&request_.faults->after_copy_verified&&!request_.faults->after_copy_verified("package-manifest.json",manifest_copy))return abort_apply(E_FAIL);
        if(detail::sha256_file(request_.package_root/L"manifest.json")!=package_.manifest_sha256||
           detail::sha256_file(manifest_copy)!=package_.manifest_sha256)
            return abort_apply(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
        if(!detail::validate_installed_version(stage_version,request_.owner,package_.version))return abort_apply(HRESULT_FROM_WIN32(ERROR_CRC));
        if(!detail::copy_verified(request_.package_root/L"LiuKaiInstaller.exe",txn_/L"new-cli",package_.files.at("LiuKaiInstaller.exe"),"LiuKaiInstaller.exe",request_.faults))return abort_apply(E_FAIL);
        if(request_.faults&&request_.faults->after_copy_verified&&!request_.faults->after_copy_verified("LiuKaiInstaller.exe",txn_/L"new-cli"))return abort_apply(E_FAIL);
        if(!detail::copy_verified(request_.package_root/L"shared"/L"branding.dll",txn_/L"new-branding",package_.files.at("shared/branding.dll"),"shared/branding.dll",request_.faults))return abort_apply(E_FAIL);
        if(request_.faults&&request_.faults->after_copy_verified&&!request_.faults->after_copy_verified("shared/branding.dll",txn_/L"new-branding"))return abort_apply(E_FAIL);
        if(FAILED(make_dirs(root/L"versions"))||FAILED(make_dirs(root/L"shared")))return abort_apply(E_FAIL);
        if(fs::exists(paths_.version_root))return abort_apply(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        bool version_published=false;
        const bool version_rename_ok=detail::rename_checked(stage_version,paths_.version_root,"version-root",request_.faults,&version_published);
        // rename_checked reports publication before running its post-rename
        // verification hook. Record ownership immediately so an injected or
        // concurrent post-publication failure can remove our exact version
        // tree during rollback; if that tree was externally changed,
        // undo_applied() will refuse removal and retain recovery evidence.
        if(version_published)created_version_=true;
        if(!version_rename_ok)return abort_apply(E_FAIL);
        const auto state=root/L".liukai-install-state",cli=root/L"LiuKaiInstaller.exe",brand=root/L"shared"/L"branding.dll";
        auto replace=[&](const fs::path& staged,const fs::path& live,const fs::path& backup,const std::string& expected_old,const std::string& expected_new,const std::string& label,bool* changed)->bool {
            if(changed)*changed=false;
            if(fs::exists(live)) { if(detail::sha256_file(live)!=expected_old)return false; if(!detail::rename_checked(live,backup,"backup-"+label,request_.faults))return false; }
            bool published=false;
            const bool rename_ok=detail::rename_checked(staged,live,label,request_.faults,&published,expected_new);
            if(published&&changed)*changed=true;
            if(!rename_ok) {
                // A post-publication failure may mean another writer changed the
                // target. Leave the backup/journal in place; undo_applied() uses
                // the recorded publication fact plus current hash to decide.
                if(!published&&fs::exists(backup))detail::rename_checked(backup,live,"restore-"+label,nullptr);
                return false;
            }
            return detail::sha256_file(live)==expected_new;
        };
        const std::string expected_cli=fresh_?std::string():old_cli_hash_,expected_brand=fresh_?std::string():old_brand_hash_;
        if(!replace(txn_/L"new-cli",cli,txn_/L"old-cli",expected_cli,package_.cli_hash,"installer-cli",&cli_changed_))return abort_apply(E_FAIL);
        if(!replace(txn_/L"new-branding",brand,txn_/L"old-branding",expected_brand,package_.branding_hash,"branding",&brand_changed_))return abort_apply(E_FAIL);
        result_state_=detail::state_contents(request_.owner,package_.version,package_.cli_hash,package_.branding_hash);
        if(fs::exists(state)){
            if(fresh_||detail::read_text(state)!=old_state_||detail::read_text(root/L".liukai-owner")!=detail::owner_contents(request_.owner))return abort_apply(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
            if(!detail::rename_checked(state,txn_/L"old-state","backup-state",request_.faults))return abort_apply(E_FAIL);
            state_backup_moved_=true;
        } else if(!fresh_) return abort_apply(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
        if(!detail::write_text_new(txn_/L"new-state",result_state_)||!detail::rename_checked(txn_/L"new-state",state,"install-state",request_.faults))return abort_apply(E_FAIL);
        state_changed_=true;
        if(fresh_){
            const auto owner_stage=txn_/L"new-owner";const auto owner_live=root/L".liukai-owner";
            if(!detail::write_text_new(owner_stage,detail::owner_contents(request_.owner))||!detail::rename_checked(owner_stage,owner_live,"install-owner",request_.faults))return abort_apply(E_FAIL);
            owner_created_=true;
        }
        applied_=true; return S_OK;
        } catch(...) {
            try { return abort_apply(E_FAIL); }
            catch(...) { recovery_pending_=true; return E_ACCESSDENIED; }
        }
    }
    HRESULT rollback() { if((!applied_&&!recovery_pending_)||finished_)return E_UNEXPECTED; const HRESULT hr=no_op_?S_OK:undo_applied(); if(SUCCEEDED(hr)){recovery_pending_=false;finished_=true;} return hr; }
    HRESULT finalize() { if(!applied_||finished_)return E_UNEXPECTED; const HRESULT hr=no_op_?S_OK:clean_txn(); if(FAILED(hr))return hr; finished_=true; return S_OK; }
};

inline HRESULT FileDeployment::prepare(const DeploymentRequest& request,std::unique_ptr<PreparedDeployment>* out) {
    if(!out)return E_POINTER;
    out->reset();
    if(!detail::safe_label(request.owner))return E_INVALIDARG;
    detail::Package package; if(!detail::parse_package(request.package_root,&package))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if(request.include_machine_com_x64 && package.has_x86)return E_INVALIDARG;
    if(!request.install_root.is_absolute()||!detail::absolute_no_reparse(request.install_root,false)||detail::is_within(request.package_root,request.install_root)||detail::is_within(request.install_root,request.package_root))return E_INVALIDARG;
    const auto parent=request.install_root.parent_path(); if(!fs::is_directory(parent)||detail::reparse(parent))return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if(!fs::exists(request.install_root)) {
        if(!request.expected_old_version.empty()||!request.expected_old_cli_sha256.empty()||!request.expected_old_branding_sha256.empty())return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    } else {
        if(!fs::is_directory(request.install_root)||detail::read_text(request.install_root/L".liukai-owner")!=detail::owner_contents(request.owner))return E_ACCESSDENIED;
        std::string owner,version,cli,brand; if(!detail::parse_state(detail::read_text(request.install_root/L".liukai-install-state"),&owner,&version,&cli,&brand)||owner!=request.owner)return E_ACCESSDENIED;
        if(version!=request.expected_old_version||cli!=request.expected_old_cli_sha256||brand!=request.expected_old_branding_sha256)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        if(detail::sha256_file(request.install_root/L"LiuKaiInstaller.exe")!=cli||!detail::architecture(request.install_root/L"LiuKaiInstaller.exe",true,false)||
           detail::sha256_file(request.install_root/L"shared"/L"branding.dll")!=brand||!detail::resource_only_branding(request.install_root/L"shared"/L"branding.dll"))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        // Existing top-level contents must be recognizable; unknown files and directories are never adopted.
        const std::set<std::wstring> allowed={L".liukai-owner",L".liukai-install-state",L"LiuKaiInstaller.exe",L"shared",L"versions"};
        std::set<std::wstring> found;
        std::error_code ec; for(fs::directory_iterator it(request.install_root,ec),end;it!=end&&!ec;it.increment(ec)){
            const auto name=it->path().filename().wstring();if(!allowed.count(name)||detail::reparse(it->path()))return E_ACCESSDENIED;found.insert(name);
            if(name==L"shared"||name==L"versions"){if(!it->is_directory(ec)||ec)return E_ACCESSDENIED;}
            else if(!it->is_regular_file(ec)||ec)return E_ACCESSDENIED;
        }
        if(ec||found!=allowed)return E_ACCESSDENIED;
        const auto shared=request.install_root/L"shared";if(!fs::is_directory(shared)||detail::reparse(shared))return E_ACCESSDENIED;
        std::set<std::wstring> shared_files;for(fs::directory_iterator it(shared,ec),end;it!=end&&!ec;it.increment(ec)){if(detail::reparse(it->path())||!it->is_regular_file(ec))return E_ACCESSDENIED;shared_files.insert(it->path().filename().wstring());}
        if(ec||shared_files!=std::set<std::wstring>{L"branding.dll"})return E_ACCESSDENIED;
        const auto versions=request.install_root/L"versions";if(!fs::is_directory(versions)||detail::reparse(versions))return E_ACCESSDENIED;
        bool current_seen=false;size_t version_count=0;
        for(fs::directory_iterator it(versions,ec),end;it!=end&&!ec;it.increment(ec)){
            if(detail::reparse(it->path())||!it->is_directory(ec)||ec)return E_ACCESSDENIED;
            const auto label=it->path().filename().string();if(!detail::safe_label(label)||!detail::validate_installed_version(it->path(),request.owner,label))return E_ACCESSDENIED;
            current_seen=current_seen||label==version;++version_count;
        }
        if(ec||!version_count||!current_seen)return E_ACCESSDENIED;
    }
    *out=std::unique_ptr<PreparedDeployment>(new PreparedDeployment(request,std::move(package))); return S_OK;
}

inline HRESULT FileDeployment::inspect(const fs::path& root,const std::string& expected_owner,DeploymentSnapshot* out) {
    if(!out)return E_POINTER;
    *out=DeploymentSnapshot{};
    if(!detail::safe_label(expected_owner)||!root.is_absolute()||!detail::absolute_no_reparse(root,true)||!fs::is_directory(root)||detail::reparse(root))return E_INVALIDARG;
    if(detail::read_text(root/L".liukai-owner")!=detail::owner_contents(expected_owner))return E_ACCESSDENIED;
    std::string owner,version,cli,branding;
    if(!detail::parse_state(detail::read_text(root/L".liukai-install-state"),&owner,&version,&cli,&branding)||owner!=expected_owner)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const auto cli_path=root/L"LiuKaiInstaller.exe",brand_path=root/L"shared"/L"branding.dll",version_root=root/L"versions"/detail::widen_ascii(version);
    bool has_x86=false;
    if(detail::sha256_file(cli_path)!=cli||!detail::architecture(cli_path,true,false)||detail::sha256_file(brand_path)!=branding||!detail::resource_only_branding(brand_path)||!detail::validate_installed_version(version_root,expected_owner,version,&has_x86))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    out->version=version;out->installer_cli_sha256=cli;out->branding_sha256=branding;
    out->manifest_sha256=detail::sha256_file(version_root/L"package-manifest.json");
    if(!detail::valid_hash(out->manifest_sha256))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    out->installer_cli=cli_path;out->branding_dll=brand_path;out->x64_dll=version_root/L"x64"/L"BoshiamyTsf.dll";
    out->has_x86=has_x86;
    if(has_x86)out->x86_dll=version_root/L"x86"/L"BoshiamyTsf.dll";
    return S_OK;
}

} // namespace boshiamy::installer::files
