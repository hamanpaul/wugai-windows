#ifndef LIUKAI_INSTALLER_WINDOWS_BACKEND_HPP
#define LIUKAI_INSTALLER_WINDOWS_BACKEND_HPP

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include "dual_architecture_transaction.hpp"
#include "deployment_access_preflight.hpp"
#include <filesystem>

namespace boshiamy::installer {

class SharedResourceStore {
public:
    virtual ~SharedResourceStore() = default;
    virtual HRESULT read(Resource resource, ResourceState* state) = 0;
    virtual HRESULT apply(Resource resource, const ResourceState& desired) = 0;
    virtual HRESULT restore(Resource resource, const ResourceState& snapshot) = 0;
    virtual HRESULT verify_applied(Resource, const ResourceState& desired, const ResourceState& observed) const {
        return desired == observed ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    virtual HRESULT project_apply_state(Resource, const ResourceState& state, ResourceState* out) const {
        if (!out) return E_POINTER;
        *out = state;
        return S_OK;
    }
    virtual HRESULT verify_install_health() { return S_OK; }
    virtual HRESULT prepare_apply_targets(TransactionOperation, const InstallRequest&,
        const ResourceStates&, ResourceStates* desired) { return desired ? S_OK : E_POINTER; }
    virtual bool failure_may_have_mutated(Resource, HRESULT) const { return true; }
};

class WindowsInstallerBackend final : public Backend {
    struct Key {
        HKEY value = nullptr;
        Key() = default;
        explicit Key(HKEY handle) : value(handle) {}
        Key(const Key&) = delete;
        Key& operator=(const Key&) = delete;
        Key(Key&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
        Key& operator=(Key&& other) noexcept {
            if (this != &other) { if (value) RegCloseKey(value); value = std::exchange(other.value, nullptr); }
            return *this;
        }
        ~Key() { if (value) RegCloseKey(value); }
        HKEY* put() { if (value) { RegCloseKey(value); value = nullptr; } return &value; }
    };
    struct Value { std::wstring name; DWORD type = 0; std::vector<BYTE> bytes; };
    struct Node { std::vector<Value> values; std::vector<std::pair<std::wstring, Node>> children; };

    Key classes64_;
    Key classes32_;
    Key machine_classes64_;
    std::wstring machine_sid_;
    std::wstring machine_install_root_;
    bool machine_writable_ = false;
    bool machine_root_is_clsid_ = false;
    std::wstring clsid_name_;
    std::wstring owner_;
    SharedResourceStore& shared_;
    std::function<LONG(HKEY, const std::wstring&)> delete_owned_tree_;
    std::array<std::vector<BYTE>, 3> last_snapshot_{};
    std::array<bool, 3> last_present_{};
    std::array<std::vector<BYTE>, 3> last_applied_snapshot_{};
    std::array<bool, 3> last_applied_valid_{};
    HRESULT ready_ = E_UNEXPECTED;

    static HRESULT win32(LONG status) { return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status); }
    static HRESULT read_value(HKEY key, const wchar_t* name, DWORD* type, std::vector<BYTE>* bytes) {
        DWORD size = 0, actual_type = 0;
        LONG status = RegQueryValueExW(key, name, nullptr, &actual_type, nullptr, &size);
        if (status != ERROR_SUCCESS) return win32(status);
        if (size > 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        std::vector<BYTE> buffer(size);
        status = RegQueryValueExW(key, name, nullptr, &actual_type, buffer.empty() ? nullptr : buffer.data(), &size);
        if (status != ERROR_SUCCESS) return win32(status);
        buffer.resize(size);
        if (type) *type = actual_type;
        if (bytes) *bytes = std::move(buffer);
        return S_OK;
    }
    static HRESULT string_value(HKEY key, const wchar_t* name, std::wstring* out) {
        DWORD type = 0; std::vector<BYTE> bytes;
        HRESULT hr = read_value(key, name, &type, &bytes);
        if (FAILED(hr)) return hr;
        if (type != REG_SZ || bytes.size() < sizeof(wchar_t) || bytes.size() % sizeof(wchar_t) != 0)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const wchar_t* data = reinterpret_cast<const wchar_t*>(bytes.data());
        size_t count = bytes.size() / sizeof(wchar_t);
        if (data[count - 1] != L'\0' || std::wmemchr(data, L'\0', count - 1) != nullptr)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        out->assign(data, count - 1);
        return S_OK;
    }
    static HRESULT write_string(HKEY key, const wchar_t* name, const std::wstring& value) {
        if (value.find(L'\0') != std::wstring::npos || value.size() > 32760) return E_INVALIDARG;
        LONG status = RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        return win32(status);
    }
    static void append_u32(std::vector<BYTE>& out, uint32_t value) {
        const BYTE* bytes = reinterpret_cast<const BYTE*>(&value); out.insert(out.end(), bytes, bytes + sizeof(value));
    }
    static void append_string(std::vector<BYTE>& out, const std::wstring& value) {
        append_u32(out, static_cast<uint32_t>(value.size()));
        const BYTE* bytes = reinterpret_cast<const BYTE*>(value.data());
        out.insert(out.end(), bytes, bytes + value.size() * sizeof(wchar_t));
    }
    static void serialize_node(const Node& node, std::vector<BYTE>& out, unsigned depth = 0) {
        append_u32(out, static_cast<uint32_t>(node.values.size()));
        append_u32(out, static_cast<uint32_t>(node.children.size()));
        for (const Value& value : node.values) {
            append_string(out, value.name); append_u32(out, value.type); append_u32(out, static_cast<uint32_t>(value.bytes.size()));
            out.insert(out.end(), value.bytes.begin(), value.bytes.end());
        }
        for (const auto& child : node.children) { append_string(out, child.first); serialize_node(child.second, out, depth + 1); }
        (void)depth;
    }
    static bool node_equal(const Node& left, const Node& right) {
        if (left.values.size() != right.values.size() || left.children.size() != right.children.size()) return false;
        auto value_order=[](const Value& a,const Value& b){const int ci=_wcsicmp(a.name.c_str(),b.name.c_str());return ci==0?a.name<b.name:ci<0;};
        auto child_order=[](const auto& a,const auto& b){const int ci=_wcsicmp(a.first.c_str(),b.first.c_str());return ci==0?a.first<b.first:ci<0;};
        std::vector<Value> lv=left.values,rv=right.values;
        std::vector<std::pair<std::wstring,Node>> lc=left.children,rc=right.children;
        std::sort(lv.begin(),lv.end(),value_order);std::sort(rv.begin(),rv.end(),value_order);
        std::sort(lc.begin(),lc.end(),child_order);std::sort(rc.begin(),rc.end(),child_order);
        for(size_t i=0;i<lv.size();++i)
            if(lv[i].name!=rv[i].name||lv[i].type!=rv[i].type||lv[i].bytes!=rv[i].bytes)return false;
        for(size_t i=0;i<lc.size();++i)
            if(lc[i].first!=rc[i].first||!node_equal(lc[i].second,rc[i].second))return false;
        return true;
    }
    static void set_node_string(Node* node,const std::wstring& name,const std::wstring& value) {
        auto found=std::find_if(node->values.begin(),node->values.end(),[&](const Value& v){return _wcsicmp(v.name.c_str(),name.c_str())==0;});
        Value row;if(found!=node->values.end())row=*found;else row.name=name;
        row.type=REG_SZ;row.bytes.resize((value.size()+1)*sizeof(wchar_t));
        std::memcpy(row.bytes.data(),value.c_str(),row.bytes.size());
        if(found!=node->values.end())*found=std::move(row);else node->values.push_back(std::move(row));
    }
    struct Reader {
        const std::vector<BYTE>& bytes; size_t offset = 0;
        bool u32(uint32_t* value) {
            if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t)) return false;
            std::memcpy(value, bytes.data() + offset, sizeof(uint32_t)); offset += sizeof(uint32_t); return true;
        }
        bool string(std::wstring* value) {
            uint32_t chars = 0;
            if (!u32(&chars) || chars > 32760 || size_t(chars) > (bytes.size() - offset) / sizeof(wchar_t)) return false;
            value->assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset), chars);
            offset += size_t(chars) * sizeof(wchar_t); return true;
        }
        bool node(Node* value, unsigned depth = 0) {
            uint32_t value_count = 0, child_count = 0;
            if (depth > 32 || !u32(&value_count) || !u32(&child_count) || value_count > 4096 || child_count > 4096) return false;
            for (uint32_t i = 0; i < value_count; ++i) {
                Value row; uint32_t type = 0, size = 0;
                if (!string(&row.name) || !u32(&type) || !u32(&size) || size > 1024 * 1024 || size > bytes.size() - offset) return false;
                row.type = type;
                row.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + size); offset += size;
                value->values.push_back(std::move(row));
            }
            for (uint32_t i = 0; i < child_count; ++i) {
                std::wstring name; Node child;
                if (!string(&name) || name.empty() || !node(&child, depth + 1)) return false;
                value->children.emplace_back(std::move(name), std::move(child));
            }
            return true;
        }
    };
    static HRESULT snapshot_node(const std::vector<BYTE>& snapshot,Node* node) {
        if(!node)return E_POINTER;
        Reader reader{snapshot};uint32_t version=0;
        if(!reader.u32(&version)||version!=1||!reader.node(node)||reader.offset!=reader.bytes.size())
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return S_OK;
    }
    HRESULT expected_written_tree(Resource resource,const ResourceState& current,const std::vector<BYTE>& before,
                                  const ResourceState& desired,std::vector<BYTE>* expected) const {
        if(!expected)return E_POINTER;
        Node root;
        if(current.present) { HRESULT hr=snapshot_node(before,&root);if(FAILED(hr))return hr; }
        else {
            set_node_string(&root,L"LiuKaiOwner",owner_);
            set_node_string(&root,L"",L"嘸．改");
        }
        if(resource==Resource::machine_com_x64) {
            set_node_string(&root,L"LiuKaiInstallSid",machine_sid_);
            set_node_string(&root,L"LiuKaiInstallRoot",machine_install_root_);
        }
        auto server=std::find_if(root.children.begin(),root.children.end(),[](const auto& item){return _wcsicmp(item.first.c_str(),L"InprocServer32")==0;});
        if(server==root.children.end()) { root.children.emplace_back(L"InprocServer32",Node{});server=std::prev(root.children.end()); }
        set_node_string(&server->second,L"",desired.path);
        set_node_string(&server->second,L"ThreadingModel",L"Apartment");
        expected->clear();append_u32(*expected,1);serialize_node(root,*expected);return S_OK;
    }
    static HRESULT capture_tree(HKEY key, Node* node, unsigned depth = 0) {
        if (depth > 32) return HRESULT_FROM_WIN32(ERROR_STACK_OVERFLOW);
        DWORD subkeys = 0, max_subkey = 0, values = 0, max_value_name = 0, max_value_data = 0;
        LONG status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, &max_subkey, nullptr,
            &values, &max_value_name, &max_value_data, nullptr, nullptr);
        if (status != ERROR_SUCCESS) return win32(status);
        if (subkeys > 4096 || values > 4096 || max_subkey > 32760 || max_value_name > 32760 || max_value_data > 1024 * 1024)
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        std::vector<wchar_t> name(static_cast<size_t>(max_value_name) + 2);
        std::vector<BYTE> data(static_cast<size_t>(max_value_data) + 1);
        for (DWORD i = 0; i < values; ++i) {
            DWORD name_len = static_cast<DWORD>(name.size()), data_len = static_cast<DWORD>(data.size()), type = 0;
            status = RegEnumValueW(key, i, name.data(), &name_len, nullptr, &type, data.data(), &data_len);
            if (status != ERROR_SUCCESS) return win32(status);
            Value value; value.name.assign(name.data(), name_len); value.type = type; value.bytes.assign(data.begin(), data.begin() + data_len);
            node->values.push_back(std::move(value));
        }
        name.resize(static_cast<size_t>(max_subkey) + 2);
        for (DWORD i = 0; i < subkeys; ++i) {
            DWORD name_len = static_cast<DWORD>(name.size());
            status = RegEnumKeyExW(key, i, name.data(), &name_len, nullptr, nullptr, nullptr, nullptr);
            if (status != ERROR_SUCCESS) return win32(status);
            std::wstring child_name(name.data(), name_len); Key child;
            status = RegOpenKeyExW(key, child_name.c_str(), 0, KEY_READ, child.put());
            if (status != ERROR_SUCCESS) return win32(status);
            Node child_node; HRESULT hr = capture_tree(child.value, &child_node, depth + 1);
            if (FAILED(hr)) return hr;
            node->children.emplace_back(std::move(child_name), std::move(child_node));
        }
        return S_OK;
    }
    static HRESULT set_value(HKEY key, const Value& value) {
        return win32(RegSetValueExW(key, value.name.empty() ? nullptr : value.name.c_str(), 0, value.type,
            value.bytes.empty() ? nullptr : value.bytes.data(), static_cast<DWORD>(value.bytes.size())));
    }
    static HRESULT restore_node(HKEY parent, const std::wstring& name, const Node& node) {
        Key key; DWORD disposition = 0;
        LONG status = RegCreateKeyExW(parent, name.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, key.put(), &disposition);
        if (status != ERROR_SUCCESS) return win32(status);
        // Restore is exact: remove current contents before recreating the snapshot.
        status = RegDeleteTreeW(key.value, nullptr);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return win32(status);
        for (const Value& value : node.values) { HRESULT hr = set_value(key.value, value); if (FAILED(hr)) return hr; }
        for (const auto& child : node.children) { HRESULT hr = restore_node(key.value, child.first, child.second); if (FAILED(hr)) return hr; }
        return S_OK;
    }
    HKEY classes(Resource resource) const {
        return resource==Resource::machine_com_x64?machine_classes64_.value:
            resource==Resource::com_x64?classes64_.value:classes32_.value;
    }
    int com_index(Resource resource) const { return resource==Resource::machine_com_x64?2:resource==Resource::com_x64?0:1; }
    static bool is_com(Resource resource) {
        return resource==Resource::com_x64||resource==Resource::com_x86||resource==Resource::machine_com_x64;
    }
    static bool canonical_root(const std::wstring& value) {
        try {
            const std::filesystem::path path(value);
            if(value.empty()||value.find(L'\0')!=std::wstring::npos||!path.is_absolute()||path!=path.lexically_normal())return false;
            for(const auto& part:path)if(part==L"."||part==L"..")return false;
            return !path.filename().empty();
        } catch(...) { return false; }
    }
    static bool path_below_root(const std::wstring& path_value,const std::wstring& root_value) {
        if(!canonical_root(root_value)||!canonical_root(path_value))return false;
        const std::filesystem::path path(path_value),root(root_value);
        auto file=path.begin();
        for(auto parent=root.begin();parent!=root.end();++parent,++file)
            if(file==path.end()||_wcsicmp(file->c_str(),parent->c_str())!=0)return false;
        return file!=path.end();
    }
    HRESULT machine_binding(const ResourceState& state,std::wstring* root_out=nullptr,bool exact_tree=false) const {
        if(!state.present)return S_OK;
        Node node;HRESULT hr=snapshot_node(state.private_data,&node);if(FAILED(hr))return hr;
        auto value=[&](const wchar_t* name,std::wstring* output) {
            auto row=std::find_if(node.values.begin(),node.values.end(),[&](const Value& v){return _wcsicmp(v.name.c_str(),name)==0;});
            if(row==node.values.end()||row->type!=REG_SZ||row->bytes.size()<sizeof(wchar_t)||row->bytes.size()%sizeof(wchar_t))return false;
            const auto* data=reinterpret_cast<const wchar_t*>(row->bytes.data());
            const size_t count=row->bytes.size()/sizeof(wchar_t);
            if(data[count-1]!=L'\0'||std::wmemchr(data,L'\0',count-1))return false;
            output->assign(data,count-1);return true;
        };
        std::wstring sid,root;
        if(!value(L"LiuKaiInstallSid",&sid)||!value(L"LiuKaiInstallRoot",&root)||
           sid!=machine_sid_||!deployment_access::canonical_sid(sid)||!path_below_root(state.path,root)||
           (!machine_install_root_.empty()&&!detail::same_path(root,machine_install_root_))||
           state.owner!=owner_||state.threading_model!=L"Apartment")
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if(exact_tree) {
            auto server=std::find_if(node.children.begin(),node.children.end(),[](const auto& child){return _wcsicmp(child.first.c_str(),L"InprocServer32")==0;});
            if(server==node.children.end())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            auto matches=[](const Node& parent,const wchar_t* name,const std::wstring& expected) {
                auto row=std::find_if(parent.values.begin(),parent.values.end(),[&](const Value& v){return _wcsicmp(v.name.c_str(),name)==0;});
                if(row==parent.values.end()||row->type!=REG_SZ||row->bytes.size()!=(expected.size()+1)*sizeof(wchar_t))return false;
                return std::memcmp(row->bytes.data(),expected.c_str(),row->bytes.size())==0;
            };
            if(!matches(node,L"LiuKaiOwner",state.owner)||!matches(server->second,L"",state.path)||
               !matches(server->second,L"ThreadingModel",state.threading_model))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        if(root_out)*root_out=root;
        return S_OK;
    }
    HRESULT validate_machine_request(const InstallRequest& request) const {
        if(!request.include_machine_com_x64)return S_OK;
        if(!machine_classes64_.value||!machine_writable_||!deployment_access::canonical_sid(machine_sid_)||
           !canonical_root(machine_install_root_)||request.include_x86||
           !path_below_root(request.x64_binary_path,machine_install_root_)||
           (!request.expected_x64_path.empty()&&!path_below_root(request.expected_x64_path,machine_install_root_)))return E_INVALIDARG;
        return S_OK;
    }
    HRESULT clsid_path(Resource resource, std::wstring* path) const {
        if (clsid_name_.empty()) return E_INVALIDARG;
        *path = resource == Resource::machine_com_x64 && machine_root_is_clsid_ ? clsid_name_ : L"CLSID\\" + clsid_name_;
        return S_OK;
    }
    HRESULT capture_raw_com(Resource resource, bool* present, std::vector<BYTE>* snapshot) const {
        if (!present || !snapshot) return E_POINTER;
        if(!classes(resource)) { *present=false;snapshot->clear();return S_OK; }
        std::wstring path; HRESULT hr = clsid_path(resource, &path); if (FAILED(hr)) return hr;
        Key key; LONG status = RegOpenKeyExW(classes(resource), path.c_str(), 0, KEY_READ, key.put());
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
            *present = false; snapshot->clear(); return S_OK;
        }
        if (status != ERROR_SUCCESS) return win32(status);
        Node node; hr = capture_tree(key.value, &node); if (FAILED(hr)) return hr;
        snapshot->clear(); append_u32(*snapshot, 1); serialize_node(node, *snapshot);
        *present = true;
        return S_OK;
    }
    HRESULT capture_com(Resource resource, ResourceState* state, std::vector<BYTE>* snapshot) const {
        if(!classes(resource)) { *state=ResourceState{};snapshot->clear();return S_OK; }
        std::wstring path; HRESULT hr = clsid_path(resource, &path); if (FAILED(hr)) return hr;
        Key key; LONG status = RegOpenKeyExW(classes(resource), path.c_str(), 0, KEY_READ, key.put());
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
            *state = ResourceState{}; snapshot->clear(); return S_OK;
        }
        if (status != ERROR_SUCCESS) return win32(status);
        Node node; hr = capture_tree(key.value, &node); if (FAILED(hr)) return hr;
        snapshot->clear(); append_u32(*snapshot, 1); serialize_node(node, *snapshot);
        *state=ResourceState{};
        state->present = true;
        hr = string_value(key.value, L"LiuKaiOwner", &state->owner); if (FAILED(hr)) return hr;
        state->created_by_lifecycle = resource!=Resource::machine_com_x64&&state->owner == owner_;
        Key server; status = RegOpenKeyExW(key.value, L"InprocServer32", 0, KEY_READ, server.put());
        if (status != ERROR_SUCCESS) return win32(status);
        hr = string_value(server.value, nullptr, &state->path); if (FAILED(hr)) return hr;
        hr = string_value(server.value, L"ThreadingModel", &state->threading_model); if (FAILED(hr)) return hr;
        state->private_data = *snapshot;
        if(resource==Resource::machine_com_x64) {
            state->created_by_lifecycle=false;
            hr=machine_binding(*state);if(FAILED(hr))return hr;
            state->created_by_lifecycle=true;
        }
        return S_OK;
    }
    HRESULT clear_or_restore(Resource resource, const ResourceState& snapshot) {
        std::wstring path; HRESULT hr = clsid_path(resource, &path); if (FAILED(hr)) return hr;
        if (!snapshot.present) {
            LONG status = RegDeleteTreeW(classes(resource), path.c_str());
            return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND ? S_OK : win32(status);
        }
        Reader reader{snapshot.private_data}; uint32_t version = 0; Node node;
        if (!reader.u32(&version) || version != 1 || !reader.node(&node) || reader.offset != reader.bytes.size())
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        LONG status = RegDeleteTreeW(classes(resource), path.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) return win32(status);
        return restore_node(classes(resource), path, node);
    }
    HRESULT apply_com(Resource resource, const ResourceState& desired) {
        if(resource==Resource::machine_com_x64&&(!machine_writable_||!machine_classes64_.value))return E_ACCESSDENIED;
        if(resource==Resource::machine_com_x64&&desired.present&&
           (!path_below_root(desired.path,machine_install_root_)||desired.owner!=owner_))return E_INVALIDARG;
        const int index = com_index(resource);
        last_applied_valid_[index]=false;
        ResourceState current; std::vector<BYTE> current_snapshot;
        HRESULT hr = capture_com(resource, &current, &current_snapshot); if (FAILED(hr)) return hr;
        if (current.present != last_present_[index] || current_snapshot != last_snapshot_[index])
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        auto failed_after_write = [&](HRESULT failure) {
            bool present = false; std::vector<BYTE> observed;
            if (SUCCEEDED(capture_raw_com(resource, &present, &observed))) {
                last_present_[index] = present; last_snapshot_[index] = std::move(observed);
            }
            return failure;
        };
        if (!desired.present) {
            if (!current.present) return S_FALSE;
            if (current.owner != owner_) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
            std::wstring clsid_path_value; hr = clsid_path(resource, &clsid_path_value); if (FAILED(hr)) return hr;
            LONG status = delete_owned_tree_ ? delete_owned_tree_(classes(resource), clsid_path_value) :
                RegDeleteTreeW(classes(resource), clsid_path_value.c_str());
            if (status != ERROR_SUCCESS) return failed_after_write(win32(status));
            bool remains = false; std::vector<BYTE> after_delete;
            hr = capture_raw_com(resource, &remains, &after_delete);
            if (FAILED(hr)) return failed_after_write(hr);
            if (remains) return failed_after_write(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            last_present_[index] = false; last_snapshot_[index].clear();
            last_applied_snapshot_[index].clear();last_applied_valid_[index]=true;return S_OK;
        }
        if (desired.owner != owner_ || desired.threading_model != L"Apartment" || desired.path.empty()) return E_INVALIDARG;
        if (current.present && (current.owner != owner_ || current.threading_model != L"Apartment"))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        std::wstring path; hr = clsid_path(resource, &path); if (FAILED(hr)) return hr;
        Key class_key; DWORD disposition = 0;
        LONG status = RegCreateKeyExW(classes(resource), path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
            nullptr, class_key.put(), &disposition);
        if (status != ERROR_SUCCESS) return failed_after_write(win32(status));
        if (!current.present) {
            hr = write_string(class_key.value, L"LiuKaiOwner", owner_); if (FAILED(hr)) return failed_after_write(hr);
            hr = write_string(class_key.value, nullptr, L"嘸．改"); if (FAILED(hr)) return failed_after_write(hr);
        }
        if(resource==Resource::machine_com_x64) {
            hr=write_string(class_key.value,L"LiuKaiInstallSid",machine_sid_);if(FAILED(hr))return failed_after_write(hr);
            hr=write_string(class_key.value,L"LiuKaiInstallRoot",machine_install_root_);if(FAILED(hr))return failed_after_write(hr);
        }
        Key server; status = RegCreateKeyExW(class_key.value, L"InprocServer32", 0, nullptr, 0,
            KEY_READ | KEY_WRITE, nullptr, server.put(), nullptr);
        if (status != ERROR_SUCCESS) return failed_after_write(win32(status));
        hr = write_string(server.value, nullptr, desired.path); if (FAILED(hr)) return failed_after_write(hr);
        hr = write_string(server.value, L"ThreadingModel", L"Apartment"); if (FAILED(hr)) return failed_after_write(hr);
        ResourceState verify; std::vector<BYTE> verified_snapshot;
        hr = capture_com(resource, &verify, &verified_snapshot); if (FAILED(hr)) return failed_after_write(hr);
        if (!verify.present || verify.owner != owner_ || !detail::same_path(verify.path, desired.path) || verify.threading_model != L"Apartment")
            return failed_after_write(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        std::vector<BYTE> expected_snapshot;
        hr=expected_written_tree(resource,current,current_snapshot,desired,&expected_snapshot);
        if(FAILED(hr))return failed_after_write(hr);
        Node expected_node,verified_node;
        hr=snapshot_node(expected_snapshot,&expected_node);if(FAILED(hr))return failed_after_write(hr);
        hr=snapshot_node(verified_snapshot,&verified_node);if(FAILED(hr))return failed_after_write(hr);
        if(!node_equal(expected_node,verified_node))return failed_after_write(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
        last_present_[index] = true; last_snapshot_[index] = verified_snapshot;
        last_applied_snapshot_[index]=verified_snapshot;last_applied_valid_[index]=true;
        return S_OK;
    }
    static HRESULT validate_pe_dll(const std::wstring& path, WORD expected_machine, WORD expected_magic) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER size{}; HRESULT result = S_OK;
        IMAGE_DOS_HEADER dos{}; DWORD read = 0;
        if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(dos)) ||
            !ReadFile(file, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
            result = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
        } else {
            const LONGLONG required = static_cast<LONGLONG>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD);
            if (required > size.QuadPart || dos.e_lfanew > 16 * 1024 * 1024 ||
                SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
                result = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
            } else {
                DWORD signature = 0; IMAGE_FILE_HEADER header{}; WORD magic = 0;
                if (!ReadFile(file, &signature, sizeof(signature), &read, nullptr) || read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
                    !ReadFile(file, &header, sizeof(header), &read, nullptr) || read != sizeof(header) ||
                    !ReadFile(file, &magic, sizeof(magic), &read, nullptr) || read != sizeof(magic) ||
                    header.Machine != expected_machine || magic != expected_magic || !(header.Characteristics & IMAGE_FILE_DLL)) {
                    result = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
                }
            }
        }
        CloseHandle(file);
        return result;
    }

public:
    WindowsInstallerBackend(HKEY classes64, HKEY classes32, REFCLSID clsid, std::wstring owner, SharedResourceStore& shared,
                            std::function<LONG(HKEY, const std::wstring&)> delete_owned_tree = {},
                            HKEY machine_classes64=nullptr,std::wstring machine_sid={},
                            std::wstring machine_install_root={},bool machine_writable=false,bool machine_root_is_clsid=false)
        : classes64_(classes64), classes32_(classes32), machine_classes64_(machine_classes64),
          machine_sid_(std::move(machine_sid)),machine_install_root_(std::move(machine_install_root)),
          machine_writable_(machine_writable),machine_root_is_clsid_(machine_root_is_clsid),owner_(std::move(owner)), shared_(shared),
          delete_owned_tree_(std::move(delete_owned_tree)) {
        wchar_t id[40]{};
        if (StringFromGUID2(clsid, id, 40) > 0) {
            clsid_name_ = id; if (!clsid_name_.empty() && clsid_name_.back() == L'\0') clsid_name_.pop_back();
            ready_ = classes64_.value && classes32_.value && !owner_.empty() ? S_OK : E_INVALIDARG;
        }
    }
    static REGSAM view_flag(Resource resource) {
        return resource == Resource::com_x86 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
    }
    static HRESULT open_classes_root(HKEY hive, const std::wstring& path, Resource resource, REGSAM access, HKEY* out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        const REGSAM view = view_flag(resource);
        return win32(RegOpenKeyExW(hive, path.c_str(), 0, access | view, out));
    }
    static HRESULT open_current_user(REFCLSID clsid, std::wstring owner, SharedResourceStore& shared,
                                     std::unique_ptr<WindowsInstallerBackend>* out,
                                     const std::wstring& classes_path = L"Software\\Classes",
                                     std::function<LONG(HKEY, const std::wstring&)> delete_owned_tree = {},
                                     REGSAM requested_access = KEY_READ | KEY_WRITE) {
        if (!out) return E_POINTER;
        out->reset();
        HKEY classes64 = nullptr, classes32 = nullptr;
        HRESULT hr = open_classes_root(HKEY_CURRENT_USER, classes_path, Resource::com_x64,
                                       requested_access, &classes64);
        if (FAILED(hr)) return hr;
        hr = open_classes_root(HKEY_CURRENT_USER, classes_path, Resource::com_x86,
                               requested_access, &classes32);
        if (FAILED(hr)) { RegCloseKey(classes64); return hr; }
        std::unique_ptr<WindowsInstallerBackend> backend(new (std::nothrow)
            WindowsInstallerBackend(classes64, classes32, clsid, std::move(owner), shared, std::move(delete_owned_tree)));
        if (!backend) { RegCloseKey(classes64); RegCloseKey(classes32); return E_OUTOFMEMORY; }
        if (FAILED(backend->ready())) return backend->ready();
        *out = std::move(backend);
        return S_OK;
    }
    // Production composition is explicit: user COM keeps its old hive, while
    // the machine view is always observed and opened for writes only by policy.
    static HRESULT open_scope(REFCLSID clsid,std::wstring owner,SharedResourceStore& shared,
        std::unique_ptr<WindowsInstallerBackend>* out,bool machine_policy,
        const std::wstring& install_root={},REGSAM requested_access=KEY_READ|KEY_WRITE) {
        if(!out)return E_POINTER;
        out->reset();
        const CLSID own={0x5d7c4291,0x2b34,0x4ce8,{0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
        if(!IsEqualCLSID(clsid,own)||owner!=L"LiuKai-TSF-v1")return E_INVALIDARG;
        deployment_access::WindowsApi identity;std::wstring sid;
        DWORD status=identity.process_user_sid(&sid);if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
        const bool writable=(requested_access&(KEY_SET_VALUE|KEY_CREATE_SUB_KEY|DELETE))!=0;
        if(machine_policy&&writable) {
            if(!canonical_root(install_root))return E_INVALIDARG;
            HANDLE token=nullptr;TOKEN_ELEVATION elevation{};DWORD bytes=0;
            if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return HRESULT_FROM_WIN32(GetLastError());
            const BOOL queried=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&bytes);
            const DWORD error=queried?ERROR_SUCCESS:GetLastError();CloseHandle(token);
            if(!queried)return HRESULT_FROM_WIN32(error);
            if(!elevation.TokenIsElevated)return E_ACCESSDENIED;
        }
        Key user64,user32,machine64;
        HRESULT hr=open_classes_root(HKEY_CURRENT_USER,L"Software\\Classes",Resource::com_x64,requested_access,user64.put());
        if(FAILED(hr))return hr;
        hr=open_classes_root(HKEY_CURRENT_USER,L"Software\\Classes",Resource::com_x86,requested_access,user32.put());
        if(FAILED(hr))return hr;
        // The Classes parent can deny KEY_WRITE even for elevated admins.
        // Open the actual CLSID parent and address only this product's GUID.
        hr=open_classes_root(HKEY_LOCAL_MACHINE,L"Software\\Classes\\CLSID",Resource::machine_com_x64,
            machine_policy?requested_access:KEY_READ,machine64.put());
        if(FAILED(hr))return hr;
        std::unique_ptr<WindowsInstallerBackend> backend(new(std::nothrow) WindowsInstallerBackend(
            user64.value,user32.value,clsid,std::move(owner),shared,{},machine64.value,std::move(sid),install_root,machine_policy&&writable,true));
        if(!backend)return E_OUTOFMEMORY;
        user64.value=nullptr;user32.value=nullptr;machine64.value=nullptr;
        if(FAILED(backend->ready()))return backend->ready();
        *out=std::move(backend);return S_OK;
    }
    HRESULT ready() const { return ready_; }
    HRESULT project_apply_state(Resource resource, const ResourceState& state, ResourceState* out) const override {
        if (resource == Resource::profile || resource == Resource::keyboard_category || resource == Resource::system_tray_category || resource == Resource::ui_element_category)
            return shared_.project_apply_state(resource, state, out);
        if(is_com(resource)&&!state.present) {
            if(!out)return E_POINTER;
            *out=ResourceState{};return S_OK;
        }
        if(resource==Resource::machine_com_x64) {
            if(!out)return E_POINTER;
            *out=state;out->private_data.clear();
            std::wstring root=machine_install_root_;
            if(!state.private_data.empty()) { HRESULT hr=machine_binding(state,&root);if(FAILED(hr))return hr; }
            if(!canonical_root(root)||!deployment_access::canonical_sid(machine_sid_)||!path_below_root(state.path,root))return E_INVALIDARG;
            std::transform(root.begin(),root.end(),root.begin(),[](wchar_t unit){return static_cast<wchar_t>(std::towlower(unit));});
            append_u32(out->private_data,1);append_string(out->private_data,machine_sid_);append_string(out->private_data,root);
            return S_OK;
        }
        return Backend::project_apply_state(resource, state, out);
    }
    HRESULT verify_install_health() override { return shared_.verify_install_health(); }
    HRESULT prepare_apply_targets(TransactionOperation operation, const InstallRequest& request,
        const ResourceStates& before, ResourceStates* desired) override {
        HRESULT hr=validate_machine_request(request);if(FAILED(hr))return hr;
        hr=shared_.prepare_apply_targets(operation,request,before,desired);if(hr!=S_OK)return hr;
        if(request.include_machine_com_x64&&operation==TransactionOperation::uninstall) {
            // Removed resources read back as canonical absence. Keep complete
            // old trees in before[] for exact rollback and durable snapshots.
            for(size_t i=0;i<desired->size();++i)
                if(before[i].present&&!(*desired)[i].present)(*desired)[i]=ResourceState{};
        }
        return S_OK;
    }
    HRESULT validate_install(const InstallRequest& request) override {
        if (FAILED(ready_)) return ready_;
        HRESULT machine_status=validate_machine_request(request);if(FAILED(machine_status))return machine_status;
        const DWORD icon_attributes = GetFileAttributesW(request.icon_path.c_str());
        if (icon_attributes == INVALID_FILE_ATTRIBUTES || (icon_attributes & FILE_ATTRIBUTE_DIRECTORY))
            return HRESULT_FROM_WIN32(icon_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY);
        HRESULT hr = validate_pe_dll(request.x64_binary_path, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
        if (FAILED(hr)) return hr;
        return request.include_x86
            ? validate_pe_dll(request.x86_binary_path, IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            : S_OK;
    }
    HRESULT read(Resource resource, ResourceState* state) override {
        if (FAILED(ready_)) return ready_;
        if (!state) return E_POINTER;
        if (resource == Resource::profile || resource == Resource::keyboard_category || resource == Resource::system_tray_category || resource == Resource::ui_element_category)
            return shared_.read(resource, state);
        if (!is_com(resource)) return E_INVALIDARG;
        std::vector<BYTE> snapshot; HRESULT hr = capture_com(resource, state, &snapshot); if (FAILED(hr)) return hr;
        const int i = com_index(resource); last_present_[i] = state->present; last_snapshot_[i] = std::move(snapshot);
        return S_OK;
    }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        if (FAILED(ready_)) return ready_;
        if (resource == Resource::profile || resource == Resource::keyboard_category || resource == Resource::system_tray_category || resource == Resource::ui_element_category)
            return shared_.apply(resource, desired);
        if (!is_com(resource)) return E_INVALIDARG;
        return apply_com(resource, desired);
    }
    HRESULT verify_applied(Resource resource,const ResourceState& desired,const ResourceState& observed)const override {
        if(resource==Resource::profile||resource==Resource::keyboard_category||resource==Resource::system_tray_category||resource==Resource::ui_element_category)
            return shared_.verify_applied(resource,desired,observed);
        if(!is_com(resource))return E_INVALIDARG;
        const int i=com_index(resource);
        if(!desired.present)return !observed.present&&observed.private_data.empty()?S_OK:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        if(!last_applied_valid_[i]||!observed.present||observed.created_by_lifecycle!=desired.created_by_lifecycle||
           observed.owner!=desired.owner||!detail::same_path(observed.path,desired.path)||
           observed.threading_model!=desired.threading_model||observed.private_data!=last_applied_snapshot_[i])
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return S_OK;
    }
    HRESULT restore(Resource resource, const ResourceState& snapshot) override {
        if (FAILED(ready_)) return ready_;
        if (resource == Resource::profile || resource == Resource::keyboard_category || resource == Resource::system_tray_category || resource == Resource::ui_element_category)
            return shared_.restore(resource, snapshot);
        if (!is_com(resource)) return E_INVALIDARG;
        if(resource==Resource::machine_com_x64) {
            if(!machine_writable_||!machine_classes64_.value)return E_ACCESSDENIED;
            HRESULT binding=machine_binding(snapshot,nullptr,true);if(FAILED(binding))return binding;
        }
        const int i = com_index(resource);
        last_applied_valid_[i]=false;
        bool current_present = false; std::vector<BYTE> current_bytes;
        HRESULT hr = capture_raw_com(resource, &current_present, &current_bytes);
        if (FAILED(hr)) return hr;
        if (current_present != last_present_[i] || current_bytes != last_snapshot_[i])
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        hr = clear_or_restore(resource, snapshot); if (FAILED(hr)) return hr;
        bool restored_present = false; std::vector<BYTE> bytes;
        hr = capture_raw_com(resource, &restored_present, &bytes); if (FAILED(hr)) return hr;
        if (restored_present != snapshot.present || bytes != snapshot.private_data)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        last_present_[i] = restored_present; last_snapshot_[i] = std::move(bytes);
        return S_OK;
    }
    bool failure_may_have_mutated(Resource resource, HRESULT status) const override {
        if (resource == Resource::profile || resource == Resource::keyboard_category || resource == Resource::system_tray_category || resource == Resource::ui_element_category)
            return shared_.failure_may_have_mutated(resource, status);
        if (status == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) ||
            status == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || status == E_INVALIDARG)
            return false;
        return true;
    }
};

} // namespace boshiamy::installer
#endif
