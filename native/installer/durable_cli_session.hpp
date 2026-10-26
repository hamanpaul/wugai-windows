#pragma once

#include "durable_transaction_observer.hpp"
#include "deployment_coordinator.hpp"
#include "file_deployment.hpp"
#include "durable_compensation_marker.hpp"
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <memory>
#include <string>

namespace boshiamy::installer::durable_cli {
namespace fs = std::filesystem;
namespace journal = boshiamy::installer::durable_journal;
namespace snapshot = boshiamy::installer::durable_snapshot;
namespace observer_ns = boshiamy::installer::durable_observer;

inline const wchar_t* ledger_namespace(bool machine_com_x64) {
    return journal::detail::ledger_namespace(machine_com_x64);
}

namespace detail {
inline bool ensure_directory_chain(const fs::path& path) {
    try {
        if (!path.is_absolute()) return false;
        fs::path current = path.root_path();
        for (const auto& part : path.relative_path()) {
            if (part.empty() || part == L".") continue;
            if (part == L"..") return false;
            current /= part;
            DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
                if (!CreateDirectoryW(current.c_str(), nullptr)) {
                    const DWORD create_error = GetLastError();
                    if (create_error != ERROR_ALREADY_EXISTS) return false;
                    attributes = GetFileAttributesW(current.c_str());
                } else {
                    attributes = GetFileAttributesW(current.c_str());
                }
            }
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
        }
        return true;
    } catch (...) { return false; }
}

inline bool make_token(std::string* token) {
    if (!token) return false;
    std::array<uint8_t, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    static constexpr char hex[] = "0123456789abcdef";
    token->clear();
    token->reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        token->push_back(hex[byte >> 4]);
        token->push_back(hex[byte & 15]);
    }
    return true;
}

inline bool same_path(const fs::path& left, const fs::path& right) {
    try {
        const auto a = left.lexically_normal().wstring();
        const auto b = right.lexically_normal().wstring();
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    } catch (...) { return false; }
}


struct MachineScopeLock {
    HANDLE handle=nullptr;
    bool owned=false;
    ~MachineScopeLock() {
        if(owned)ReleaseMutex(handle);
        if(handle)CloseHandle(handle);
    }
    HRESULT acquire() {
        handle=CreateMutexW(nullptr,FALSE,L"Global\\LiuKai-7254A21F-ProfileDisplayMetadata");
        if(!handle)return HRESULT_FROM_WIN32(GetLastError());
        const DWORD result=WaitForSingleObject(handle,0);
        if(result==WAIT_ABANDONED) {
            owned=true;
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        }
        if(result==WAIT_OBJECT_0) {
            owned=true;
            return S_OK;
        }
        return result==WAIT_TIMEOUT?HRESULT_FROM_WIN32(ERROR_BUSY):HRESULT_FROM_WIN32(GetLastError());
    }
};
// Read only the new namespace. Historical user-scope ledgers cannot authorize
// or block a machine replay, and are never rewritten by this classifier.
inline HRESULT inspect_machine_history(const fs::path& root,const std::string& owner,
                                       observer_ns::ApplyProjection projection={}) {
    const HRESULT uncertain=HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    try {
        const auto base=root.lexically_normal().parent_path()/ledger_namespace(true)/
            files::detail::widen_ascii(owner);
        std::error_code error;
        if(!fs::exists(base,error))return error?uncertain:S_OK;
        if(error||!files::detail::absolute_no_reparse(base,true)||!fs::is_directory(base))return uncertain;
        size_t count=0;
        for(const auto& entry:fs::directory_iterator(base)) {
            if(++count>1024||!entry.is_directory()||!files::detail::absolute_no_reparse(entry.path(),true))return uncertain;
            const auto journal_path=entry.path()/L"transaction.journal";
            const auto snapshot_path=entry.path()/L"transaction.snapshot";
            if(!files::detail::absolute_no_reparse(journal_path,true)||
               !files::detail::absolute_no_reparse(snapshot_path,true))return uncertain;
            std::string image;
            if(!journal::detail::read_file(journal_path,&image))return uncertain;
            journal::Identity identity;
            journal::detail::Digest chain{};
            size_t at=0;
            if(!journal::detail::parse_header(image,&at,&identity,&chain)||
               !journal::detail::machine_com_scope(identity)||identity.owner!=owner||
               !journal::detail::same_path(identity.storage_root,entry.path().wstring())||
               !journal::detail::same_path(identity.install_root,root.wstring()))return uncertain;
            journal::Store store(journal_path,identity);
            journal::Document document;
            if(store.read(identity,&document)!=S_OK)return uncertain;
            bool terminal=false,registration_terminal=false;
            for(const auto& item:fs::directory_iterator(entry.path())) {
                if(!item.is_regular_file()||!files::detail::absolute_no_reparse(item.path(),true))return uncertain;
                const auto name=item.path().filename();
                if(name==L"terminal-compensation.journal")terminal=true;
                else if(name==L"terminal-registration-compensation.journal")registration_terminal=true;
                else if(name!=L"transaction.journal"&&name!=L"transaction.snapshot")return uncertain;
            }
            if(terminal||registration_terminal) {
                if(terminal&&registration_terminal)return uncertain;
                if(document.committed)return uncertain;
                const HRESULT proof=terminal?
                    durable_compensation_marker::inspect(identity,root,projection):
                    durable_compensation_marker::inspect_registration(identity,root,projection);
                if(proof!=S_OK)return uncertain;
                continue;
            }
            using Kind=journal::Event::Kind;
            if(!document.committed||!document.all_operations_succeeded||document.events.size()<3||
               document.events.front().kind!=Kind::intent||document.events.front().intent.resource!="prestate"||
               document.events.front().intent.expected_before_fingerprint!=std::string(64,'0'))return uncertain;
            const auto& capture=document.events.front().intent;
            if(capture.action!="capture-install-or-repair"&&capture.action!="capture-uninstall"&&
               capture.action!="capture-x64-dev-host-migration")return uncertain;
            if(document.events[1].kind!=Kind::observation||document.events[1].observation.operation_status!=S_OK||
               document.events[1].observation.observed_fingerprint!=capture.desired_fingerprint)return uncertain;
            for(size_t event_index=0;event_index<document.events.size();++event_index) {
                const auto& event=document.events[event_index];
                if(event.kind==Kind::intent&&event.intent.resource=="prestate"&&event_index!=0)return uncertain;
                if(event.kind==Kind::intent&&event.intent.resource=="file-publication"&&
                    (capture.action=="capture-uninstall"||(event.intent.action!="apply"&&event.intent.action!="rollback")))return uncertain;
                if(event.kind==Kind::observation&&event.observation.operation_status!=S_OK)return uncertain;
                if(event.kind==Kind::intent&&event.intent.resource!="prestate"&&event.intent.resource!="file-publication") {
                    bool known=false;
                    for(size_t index=0;index<static_cast<size_t>(Resource::count);++index)
                        if(event.intent.resource==observer_ns::detail::resource_name(static_cast<Resource>(index)))known=true;
                    if(!known||event.intent.resource=="com-x86"||
                        (event.intent.action!="apply"&&event.intent.action!="restore"))return uncertain;
                }
            }
            snapshot::Store snapshot_store(snapshot_path,identity);
            ResourceStates pre{};
            std::string hash;
            if(snapshot_store.read(identity,&pre,&hash)!=S_OK||hash!=capture.desired_fingerprint)return uncertain;
        }
        return S_OK;
    }catch(...){return uncertain;}
}

inline bool file_fingerprint(const journal::Identity& identity, bool present,
                             const files::DeploymentSnapshot* snapshot_value,
                             const files::DeploymentPaths* desired_value,
                             std::string* output) {
    return durable_compensation_marker::file_fingerprint(identity,present,snapshot_value,desired_value,output);
}
}

// Owns stores before the observer that references them. The session itself is
// returned as TransactionObserver so the coordinator can retain its lifetime.
class Session final : public TransactionObserver, public FileDeploymentObserver {
public:
    static HRESULT create(const files::DeploymentRequest& request,
                          const files::DeploymentPaths& paths,
                          std::unique_ptr<TransactionObserver>* output,
                          observer_ns::ApplyProjection apply_projection = {},
                          TransactionOperation operation = TransactionOperation::install_or_repair) {
        if (!output) return E_POINTER;
        output->reset();
        if (operation != TransactionOperation::install_or_repair &&
            operation != TransactionOperation::migrate_x64_dev_host) return E_INVALIDARG;
        try {
            if (!files::detail::safe_label(request.owner) || !request.install_root.is_absolute() ||
                request.package_root.empty() || !request.package_root.is_absolute()) return E_INVALIDARG;
            files::detail::Package package;
            if (!files::detail::parse_package(request.package_root, &package) ||
                package.version != paths.version || package.manifest_sha256 != paths.manifest_sha256 ||
                package.has_x86 != paths.has_x86)
                return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            if ((operation == TransactionOperation::migrate_x64_dev_host || request.include_machine_com_x64) && package.has_x86) return E_INVALIDARG;
            const fs::path expected_version = request.install_root / L"versions" /
                files::detail::widen_ascii(package.version);
            if (!detail::same_path(paths.version_root, expected_version) ||
                !detail::same_path(paths.x64_dll, expected_version / L"x64" / L"BoshiamyTsf.dll") ||
                (paths.has_x86 && !detail::same_path(paths.x86_dll, expected_version / L"x86" / L"BoshiamyTsf.dll")) ||
                (!paths.has_x86 && !paths.x86_dll.empty()) ||
                !detail::same_path(paths.branding_dll, request.install_root / L"shared" / L"branding.dll") ||
                !detail::same_path(paths.installer_cli, request.install_root / L"LiuKaiInstaller.exe"))
                return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            const std::string manifest_hash = paths.manifest_sha256;
            if (files::detail::sha256_file(request.package_root / L"manifest.json") != manifest_hash)
                return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            if (!files::detail::valid_hash(manifest_hash)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

            journal::Identity identity;
            identity.schema_version = request.include_machine_com_x64 ? 3 : 2;
            identity.owner = request.owner;
            identity.install_root = request.install_root.lexically_normal().wstring();
            identity.package_version = package.version;
            identity.package_manifest_sha256 = manifest_hash;

            return create_identity_session(std::move(identity),output,std::move(apply_projection),operation);
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }


    // Registration removal binds existing lifecycle metadata without checking
    // payload health or loading the DLL, so owned broken installs remain removable.
    static HRESULT create_uninstall(const InstallRequest& request,const fs::path& root,
                                    std::unique_ptr<TransactionObserver>* output,
                                    observer_ns::ApplyProjection projection={}) {
        if(!output)return E_POINTER;
        output->reset();
        try {
            if(!request.include_machine_com_x64||request.include_x86||!root.is_absolute()||
               request.owner!=L"LiuKai-TSF-v1"||!files::detail::absolute_no_reparse(root,true))return E_INVALIDARG;
            const std::string owner="LiuKai-TSF-v1";
            if(!files::detail::absolute_no_reparse(root/L".liukai-owner",true)||
               !files::detail::absolute_no_reparse(root/L".liukai-install-state",true))return E_ACCESSDENIED;
            if(files::detail::read_text(root/L".liukai-owner")!=files::detail::owner_contents(owner))return E_ACCESSDENIED;
            std::string found_owner,version,cli,branding;
            if(!files::detail::parse_state(files::detail::read_text(root/L".liukai-install-state"),
                &found_owner,&version,&cli,&branding)||found_owner!=owner)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            const auto version_root=root/L"versions"/files::detail::widen_ascii(version);
            const auto expected=version_root/L"x64"/L"BoshiamyTsf.dll";
            const auto manifest=version_root/L"package-manifest.json";
            if(!detail::same_path(request.expected_x64_path,expected)||
               !files::detail::absolute_no_reparse(manifest,true))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            journal::Identity identity;
            identity.schema_version=3;
            identity.owner=owner;
            identity.install_root=root.lexically_normal().wstring();
            identity.package_version=version;
            identity.package_manifest_sha256=files::detail::sha256_file(manifest);
            if(!files::detail::valid_hash(identity.package_manifest_sha256))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return create_identity_session(std::move(identity),output,std::move(projection),TransactionOperation::uninstall);
        }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
    }

    HRESULT complete_uninstall(const LifecycleResult& result,const ResourceStates& after) {
        if(operation_!=TransactionOperation::uninstall||observer_.pending())
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if(!registration_started_)return result.operation!=S_OK&&result.cleanup==S_OK?
            S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if(result.operation==S_OK&&result.cleanup==S_OK)return observer_.commit(after);
        if(result.cleanup!=S_OK||after!=registration_pre_state_)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return durable_compensation_marker::record_registration(identity_,after,apply_projection_);
    }

    ~Session() override {
        if (observer_.started()) return;
        const DWORD journal_attributes = GetFileAttributesW(journal_path_.c_str());
        const DWORD journal_error = journal_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        const DWORD snapshot_attributes = GetFileAttributesW(snapshot_path_.c_str());
        const DWORD snapshot_error = snapshot_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        const bool journal_absent = journal_attributes == INVALID_FILE_ATTRIBUTES &&
            (journal_error == ERROR_FILE_NOT_FOUND || journal_error == ERROR_PATH_NOT_FOUND);
        const bool snapshot_absent = snapshot_attributes == INVALID_FILE_ATTRIBUTES &&
            (snapshot_error == ERROR_FILE_NOT_FOUND || snapshot_error == ERROR_PATH_NOT_FOUND);
        if (journal_absent && snapshot_absent) RemoveDirectoryW(storage_root_.c_str());
    }

    HRESULT begin(TransactionOperation operation, const ResourceStates& state) override {
        if (deployment_prestarted_) {
            if (!file_phase_observed_ || operation != operation_ ||
                state != pre_file_state_) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            return S_OK;
        }
        if (operation != operation_) return E_INVALIDARG;
        const HRESULT status=observer_.begin(operation,state);
        if(status==S_OK) {
            registration_pre_state_=state;
            registration_started_=true;
        }
        return status;
    }
    HRESULT before(Resource resource, TransactionMutation mutation,
                   const ResourceState& expected, const ResourceState& desired) override {
        return observer_.before(resource, mutation, expected, desired);
    }
    HRESULT after(Resource resource, TransactionMutation mutation, HRESULT operation_status,
                  HRESULT readback_status, const ResourceState* observed,
                  bool mutation_may_have_occurred) override {
        return observer_.after(resource, mutation, operation_status, readback_status,
                               observed, mutation_may_have_occurred);
    }
    HRESULT commit(const ResourceStates& state) override { return observer_.commit(state); }

    std::string file_transaction_token() const override { return identity_.token; }

    HRESULT before_file_publication(const ResourceStates& pre_state,
        const files::DeploymentSnapshot* old_files, const files::DeploymentPaths& desired_files) override {
        if (operation_ == TransactionOperation::migrate_x64_dev_host && old_files) return E_INVALIDARG;
        if (deployment_prestarted_ || file_intent_pending_ || desired_files.version != identity_.package_version ||
            desired_files.manifest_sha256 != identity_.package_manifest_sha256)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        HRESULT status = observer_.begin(operation_, pre_state);
        if (FAILED(status)) return status;
        pre_file_state_ = pre_state;
        deployment_prestarted_ = true;
        if (!detail::file_fingerprint(identity_, old_files != nullptr, old_files, nullptr,
                                      &old_file_fingerprint_) ||
            !detail::file_fingerprint(identity_, true, nullptr, &desired_files,
                                      &desired_file_fingerprint_)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        status = journal_.append_intent("file-publication", "apply", old_file_fingerprint_,
                                        desired_file_fingerprint_, &file_intent_sequence_);
        if (FAILED(status)) return status;
        file_intent_pending_ = true;
        return S_OK;
    }

    HRESULT after_file_publication(HRESULT operation_status,
        const files::DeploymentSnapshot* observed_files) override {
        if (!deployment_prestarted_ || !file_intent_pending_) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string observed_fingerprint;
        if (!detail::file_fingerprint(identity_, observed_files != nullptr, observed_files, nullptr,
                                      &observed_fingerprint)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        HRESULT recorded_status = operation_status;
        if (SUCCEEDED(recorded_status) && observed_fingerprint != desired_file_fingerprint_)
            recorded_status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        const HRESULT status = journal_.append_observation(file_intent_sequence_, recorded_status,
            observed_fingerprint, observed_files != nullptr || FAILED(operation_status));
        if (FAILED(status)) return status;
        file_intent_pending_ = false;
        file_phase_observed_ = true;
        return FAILED(operation_status) ? S_OK : recorded_status;
    }

    HRESULT before_file_rollback() override {
        if (!deployment_prestarted_ || !file_phase_observed_ || file_intent_pending_ ||
            file_rollback_pending_) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        HRESULT status = journal_.append_intent("file-publication", "rollback",
            desired_file_fingerprint_, old_file_fingerprint_, &file_rollback_sequence_);
        if (SUCCEEDED(status)) file_rollback_pending_ = true;
        return status;
    }

    HRESULT after_file_rollback(HRESULT operation_status,
        const files::DeploymentSnapshot* observed_files) override {
        if (!file_rollback_pending_) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string observed_fingerprint;
        if (!detail::file_fingerprint(identity_, observed_files != nullptr, observed_files, nullptr,
                                      &observed_fingerprint)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        HRESULT recorded_status = operation_status;
        if (SUCCEEDED(recorded_status) && observed_fingerprint != old_file_fingerprint_)
            recorded_status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        const HRESULT status = journal_.append_observation(file_rollback_sequence_, recorded_status,
            observed_fingerprint, observed_files != nullptr || FAILED(operation_status));
        if (FAILED(status)) return status;
        file_rollback_pending_ = false;
        file_rollback_observed_ = recorded_status == S_OK;
        return FAILED(operation_status) ? S_OK : recorded_status;
    }

    HRESULT complete_failed_compensation(const ResourceStates& restored,
        const files::DeploymentSnapshot* observed_files) override {
        if (!deployment_prestarted_ || !file_phase_observed_ || file_intent_pending_ ||
            file_rollback_pending_ || !file_rollback_observed_ || restored != pre_file_state_)
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        std::string observed_files_hash;
        if (!detail::file_fingerprint(identity_,observed_files!=nullptr,observed_files,nullptr,
                                      &observed_files_hash) ||
            observed_files_hash != old_file_fingerprint_)
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        // A failed forward dev-host migration has no prior committed migration
        // to compensate. Keep its existing uncertain-history disposition.
        if (operation_!=TransactionOperation::install_or_repair) return S_OK;
        return durable_compensation_marker::record(identity_,restored,old_file_fingerprint_,apply_projection_);
    }

    const journal::Identity& identity() const { return identity_; }

private:
    static HRESULT create_identity_session(journal::Identity identity,
        std::unique_ptr<TransactionObserver>* output,observer_ns::ApplyProjection projection,
        TransactionOperation operation) {
        std::unique_ptr<detail::MachineScopeLock> scope_lock;
        if(journal::detail::machine_com_scope(identity)) {
            scope_lock=std::make_unique<detail::MachineScopeLock>();
            HRESULT status=scope_lock->acquire();
            if(status!=S_OK)return status;
            status=detail::inspect_machine_history(identity.install_root,identity.owner,projection);
            if(status!=S_OK)return status;
        }
        const fs::path ledger_base=fs::path(identity.install_root).parent_path()/
            journal::detail::ledger_namespace(identity)/files::detail::widen_ascii(identity.owner);
        if(!detail::ensure_directory_chain(ledger_base))return E_ACCESSDENIED;
        fs::path storage_root;
        for(unsigned attempt=0;attempt<8;++attempt) {
            if(!detail::make_token(&identity.token))return E_FAIL;
            storage_root=ledger_base/files::detail::widen_ascii(identity.token);
            if(CreateDirectoryW(storage_root.c_str(),nullptr))break;
            if(GetLastError()!=ERROR_ALREADY_EXISTS)return E_ACCESSDENIED;
            storage_root.clear();
        }
        if(storage_root.empty())return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        const fs::path created=storage_root;
        identity.storage_root=storage_root.wstring();
        if(!journal::detail::valid_identity(identity)) {
            RemoveDirectoryW(created.c_str());
            return E_INVALIDARG;
        }
        try {
            *output=std::unique_ptr<TransactionObserver>(new Session(std::move(identity),std::move(storage_root),
                std::move(projection),operation,std::move(scope_lock)));
        }catch(...) {
            RemoveDirectoryW(created.c_str());
            throw;
        }
        return S_OK;
    }
    Session(journal::Identity identity, fs::path storage_root, observer_ns::ApplyProjection apply_projection, TransactionOperation operation,
            std::unique_ptr<detail::MachineScopeLock> scope_lock)
        : machine_scope_lock_(std::move(scope_lock)), operation_(operation), identity_(std::move(identity)), storage_root_(std::move(storage_root)),
          journal_path_(storage_root_ / L"transaction.journal"),
          snapshot_path_(storage_root_ / L"transaction.snapshot"),
          journal_(journal_path_, identity_, storage_root_),
          snapshot_(snapshot_path_, identity_, storage_root_),
          apply_projection_(apply_projection),observer_(journal_, snapshot_, identity_, std::move(apply_projection)) {}

    std::unique_ptr<detail::MachineScopeLock> machine_scope_lock_;
    const TransactionOperation operation_;
    journal::Identity identity_;
    fs::path storage_root_;
    fs::path journal_path_;
    fs::path snapshot_path_;
    journal::Store journal_;
    snapshot::Store snapshot_;
    observer_ns::ApplyProjection apply_projection_;
    observer_ns::Observer observer_;
    bool registration_started_=false;
    ResourceStates registration_pre_state_{};
    bool deployment_prestarted_ = false;
    bool file_intent_pending_ = false;
    bool file_phase_observed_ = false;
    bool file_rollback_pending_ = false;
    bool file_rollback_observed_ = false;
    uint64_t file_intent_sequence_ = 0;
    uint64_t file_rollback_sequence_ = 0;
    ResourceStates pre_file_state_{};
    std::string old_file_fingerprint_;
    std::string desired_file_fingerprint_;
};

inline HRESULT create_observer(const files::DeploymentRequest& request,
                               const files::DeploymentPaths& paths,
                               std::unique_ptr<TransactionObserver>* output,
                               observer_ns::ApplyProjection apply_projection = {},
                               TransactionOperation operation = TransactionOperation::install_or_repair) {
    return Session::create(request, paths, output, std::move(apply_projection), operation);
}
} // namespace boshiamy::installer::durable_cli
