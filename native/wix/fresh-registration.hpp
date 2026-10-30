#pragma once
#include "../installer/dual_architecture_transaction.hpp"
#include "../installer/durable_transaction_observer.hpp"
#include <cstring>

namespace boshiamy::installer::msi_registration {

// For a first installation only. Refuse even apparently owned pre-existing
// state: the MSI did not create it and cannot promise to restore it on rollback.
// The observer must be durable; this helper does not implement MSI rollback.
class FreshObserver final : public TransactionObserver {
    TransactionObserver& inner_;
public:
    explicit FreshObserver(TransactionObserver& inner) : inner_(inner) {}
    HRESULT begin(TransactionOperation operation, const ResourceStates& state) override {
        if (operation != TransactionOperation::install_or_repair) return E_INVALIDARG;
        for (const auto& item : state)
            if (item != ResourceState{}) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        return inner_.begin(operation, state);
    }
    HRESULT before(Resource r, TransactionMutation m, const ResourceState& a, const ResourceState& b) override {
        return inner_.before(r,m,a,b);
    }
    HRESULT after(Resource r, TransactionMutation m, HRESULT op, HRESULT read,
                  const ResourceState* observed, bool mutated) override {
        return inner_.after(r,m,op,read,observed,mutated);
    }
    HRESULT commit(const ResourceStates& state) override { return inner_.commit(state); }
};

class FinalSnapshotObserver final : public TransactionObserver {
    TransactionObserver& inner_;durable_snapshot::Store& final_;
public:
    FinalSnapshotObserver(TransactionObserver& inner,durable_snapshot::Store& final):inner_(inner),final_(final){}
    HRESULT begin(TransactionOperation op,const ResourceStates& state)override{return inner_.begin(op,state);}
    HRESULT before(Resource r,TransactionMutation m,const ResourceState& a,const ResourceState& b)override{return inner_.before(r,m,a,b);}
    HRESULT after(Resource r,TransactionMutation m,HRESULT op,HRESULT read,const ResourceState* state,bool mutated)override{return inner_.after(r,m,op,read,state,mutated);}
    HRESULT commit(const ResourceStates& state)override{const HRESULT hr=final_.create(state);return hr==S_OK?inner_.commit(state):hr;}
};

inline bool same_owned_profile_allow_user_state(const ResourceState& recorded,const ResourceState& current) {
    if(recorded==current)return true;
    if(!recorded.present||!current.present||recorded.private_data.size()<6||recorded.private_data.size()!=current.private_data.size())return false;
    const size_t offset=recorded.private_data.size()-6;uint32_t left=0,right=0;
    std::memcpy(&left,recorded.private_data.data()+offset,4);std::memcpy(&right,current.private_data.data()+offset,4);
    if(left!=0x3155534c||right!=left||recorded.private_data[offset+4]>1||recorded.private_data[offset+5]>1||
       current.private_data[offset+4]>1||current.private_data[offset+5]>1||
       !std::equal(recorded.private_data.begin(),recorded.private_data.end()-2,current.private_data.begin()))return false;
    ResourceState adjusted=current;adjusted.private_data=recorded.private_data;return adjusted==recorded;
}

inline LifecycleResult install_fresh(Backend& backend, const InstallRequest& request,
                                     TransactionObserver& observer) {
    if (request.owner != L"LiuKai-TSF-v1" || request.include_x86 || !request.include_machine_com_x64 ||
        !request.expected_x64_path.empty() || !request.expected_x86_path.empty()) return {E_INVALIDARG,S_OK};
    FreshObserver guard(observer);
    const auto result=install_or_repair(backend,request,&guard);
    if (result.operation!=S_OK || result.cleanup!=S_OK) return result;
    ResourceStates after{};
    HRESULT status=detail::read_all(backend,after);
    if (status!=S_OK) return {status,status};
    status=guard.commit(after);
    return status==S_OK ? result : LifecycleResult{status,status};
}

// Caller must hold the shared lifecycle lock and provide an already-created,
// protected ledger directory. The durable stores reject unexpected paths and
// do not overwrite an existing journal or snapshot. Recovery and MSI rollback
// must be implemented before this can be used by an installable package.
inline LifecycleResult install_fresh_durable(Backend& backend, const InstallRequest& request,
                                             const durable_journal::Identity& identity) {
    namespace fs=std::filesystem;
    namespace journal=durable_journal;
    namespace snapshot=durable_snapshot;
    if (identity.schema_version!=3 || identity.owner!="LiuKai-TSF-v1" ||
        !journal::detail::valid_identity(identity)) return {E_INVALIDARG,S_OK};
    const fs::path root(identity.install_root);
    const fs::path expected=root / L"versions" / fs::path(identity.package_version) / L"x64" / L"BoshiamyTsf.dll";
    if (!detail::same_path(expected.wstring(),request.x64_binary_path) ||
        !detail::same_path((root/L"shared"/L"branding.dll").wstring(),request.icon_path)) return {E_INVALIDARG,S_OK};
    const fs::path ledger(identity.storage_root);
    journal::Store log(ledger/L"transaction.journal",identity,ledger);
    snapshot::Store saved(ledger/L"transaction.snapshot",identity,ledger);
    durable_observer::Observer observer(log,saved,identity,
        [&backend](Resource r,const ResourceState& s,ResourceState* out) {
            return backend.project_apply_state(r,s,out);
        });
    snapshot::Store final(ledger/L"transaction.final.snapshot",identity,ledger);
    FinalSnapshotObserver tracked(observer,final);
    return install_fresh(backend,request,tracked);
}

// Verify a completed fresh registration against its snapshot, journal and live
// state before any MSI rollback is considered. This function makes no writes.
inline HRESULT inspect_committed_fresh(Backend& backend,const durable_journal::Identity& identity,ResourceStates* verified=nullptr) {
    namespace journal=durable_journal;
    namespace snapshot=durable_snapshot;
    namespace observation=durable_observer;
    if(identity.schema_version!=3 || identity.owner!="LiuKai-TSF-v1" ||
       !journal::detail::valid_identity(identity)) return E_INVALIDARG;
    const std::filesystem::path ledger(identity.storage_root);
    journal::Store log(ledger/L"transaction.journal",identity,ledger);
    snapshot::Store saved(ledger/L"transaction.snapshot",identity,ledger);
    snapshot::Store final(ledger/L"transaction.final.snapshot",identity,ledger);
    journal::Document document; ResourceStates before{},current{},recorded{};std::string before_hash;
    if(log.read(identity,&document)!=S_OK || !document.committed || !document.all_operations_succeeded ||
       saved.read(identity,&before,&before_hash)!=S_OK || final.read(identity,&recorded)!=S_OK) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    for(const auto& state:before)if(state!=ResourceState{})return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    using Kind=journal::Event::Kind;
    const Resource order[]={Resource::com_x64,Resource::machine_com_x64,Resource::profile,
        Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category};
    if(document.events.size()!=15 || document.events[0].kind!=Kind::intent ||
       document.events[0].intent.resource!="prestate" || document.events[0].intent.action!="capture-install-or-repair" ||
       document.events[0].intent.desired_fingerprint!=before_hash ||
       document.events[1].kind!=Kind::observation || document.events[1].observation.operation_status!=S_OK ||
       document.events[1].observation.observed_fingerprint!=before_hash ||
       document.events.back().kind!=Kind::commit) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    HRESULT status=detail::read_all(backend,current);if(status!=S_OK)return status;
    if(current[static_cast<size_t>(Resource::com_x86)]!=ResourceState{})return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    for(size_t i=0;i<current.size();++i) {
        if(i==static_cast<size_t>(Resource::profile)) {
            if(!same_owned_profile_allow_user_state(recorded[i],current[i]))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        } else if(recorded[i]!=current[i])return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    for(size_t i=0;i<6;++i) {
        const auto resource=order[i];const size_t index=static_cast<size_t>(resource);
        const auto& intent=document.events[2+i*2];const auto& result=document.events[3+i*2];
        if(intent.kind!=Kind::intent || intent.intent.resource!=observation::detail::resource_name(resource) ||
           intent.intent.action!="apply" || result.kind!=Kind::observation ||
           result.observation.sequence!=intent.intent.sequence || result.observation.operation_status!=S_OK ||
           result.observation.observed_fingerprint!=intent.intent.desired_fingerprint) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ResourceState projected=current[index], prior=before[index];
        if(backend.project_apply_state(resource,current[index],&projected)!=S_OK ||
           (resource==Resource::machine_com_x64 && backend.project_apply_state(resource,before[index],&prior)!=S_OK))
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        std::string actual_hash,prior_hash;
        if(!observation::detail::fingerprint_state(identity,resource,projected,&actual_hash,false) ||
           !observation::detail::fingerprint_state(identity,resource,prior,&prior_hash,false) ||
           actual_hash!=intent.intent.desired_fingerprint || prior_hash!=intent.intent.expected_before_fingerprint)
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    std::string image;
    if(!snapshot::detail::encode(identity,recorded,&image))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    journal::detail::Digest digest{};if(!journal::detail::sha256(image,&digest))return E_FAIL;
    static constexpr char hex[]="0123456789abcdef";std::string actual;actual.reserve(64);
    for(auto byte:digest){actual.push_back(hex[byte>>4]);actual.push_back(hex[byte&15]);}
    if(actual!=document.events.back().commit_fingerprint)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if(verified)*verified=current;
    return S_OK;
}

class ExactUninstallObserver final : public TransactionObserver {
    TransactionObserver& inner_; const ResourceStates& expected_;
public:
    ExactUninstallObserver(TransactionObserver& inner,const ResourceStates& expected):inner_(inner),expected_(expected){}
    HRESULT begin(TransactionOperation op,const ResourceStates& state) override {
        if(op!=TransactionOperation::uninstall || state!=expected_)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return inner_.begin(op,state);
    }
    HRESULT before(Resource r,TransactionMutation m,const ResourceState& a,const ResourceState& b) override {return inner_.before(r,m,a,b);}
    HRESULT after(Resource r,TransactionMutation m,HRESULT op,HRESULT read,const ResourceState* s,bool changed) override {return inner_.after(r,m,op,read,s,changed);}
    HRESULT commit(const ResourceStates& s) override {return inner_.commit(s);}
};

// Caller holds the lifecycle lock and supplies a different protected, empty
// ledger directory for compensation. Incomplete or changed state fails closed.
inline LifecycleResult rollback_committed_fresh(Backend& backend,const InstallRequest& request,
        const durable_journal::Identity& installed,const durable_journal::Identity& rollback) {
    namespace journal=durable_journal;namespace snapshot=durable_snapshot;
    if(!journal::detail::valid_identity(rollback) || rollback.schema_version!=3 ||
       installed.schema_version!=rollback.schema_version || installed.owner!=rollback.owner ||
       installed.package_version!=rollback.package_version || installed.package_manifest_sha256!=rollback.package_manifest_sha256 ||
       !journal::detail::same_path(installed.install_root,rollback.install_root) || installed.token==rollback.token ||
       request.owner!=L"LiuKai-TSF-v1" || request.include_x86 || !request.include_machine_com_x64)
        return {E_INVALIDARG,S_OK};
    const auto expected=std::filesystem::path(installed.install_root)/L"versions"/
        std::filesystem::path(installed.package_version)/L"x64"/L"BoshiamyTsf.dll";
    if(!detail::same_path(request.expected_x64_path,expected.wstring()))return {E_INVALIDARG,S_OK};
    ResourceStates verified{};HRESULT status=inspect_committed_fresh(backend,installed,&verified);
    if(status!=S_OK)return {status,S_OK};
    const std::filesystem::path ledger(rollback.storage_root);
    journal::Store log(ledger/L"transaction.journal",rollback,ledger);
    snapshot::Store saved(ledger/L"transaction.snapshot",rollback,ledger);
    durable_observer::Observer durable(log,saved,rollback,[&backend](Resource r,const ResourceState& s,ResourceState* out){return backend.project_apply_state(r,s,out);});
    ExactUninstallObserver observer(durable,verified);
    const auto result=uninstall(backend,request,&observer);
    if(result.operation!=S_OK || result.cleanup!=S_OK)return result;
    ResourceStates after{};status=detail::read_all(backend,after);
    if(status!=S_OK)return {status,status};
    for(const auto& state:after)if(state!=ResourceState{})return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)};
    status=observer.commit(after);
    return status==S_OK?result:LifecycleResult{status,status};
}

// A view for validating the original install against the exact pre-removal
// snapshot, without pretending that the live system is still installed.
class SnapshotView final : public Backend {
    Backend& platform_;const ResourceStates& state_;
public:
    SnapshotView(Backend& platform,const ResourceStates& state):platform_(platform),state_(state){}
    HRESULT read(Resource r,ResourceState* out)override{if(!out)return E_POINTER;*out=state_[static_cast<size_t>(r)];return S_OK;}
    HRESULT apply(Resource,const ResourceState&)override{return E_NOTIMPL;}
    HRESULT restore(Resource,const ResourceState&)override{return E_NOTIMPL;}
    HRESULT project_apply_state(Resource r,const ResourceState& s,ResourceState* out)const override{return platform_.project_apply_state(r,s,out);}
};

// Restore exact captured state using the backend's CAS-checked restore path.
class ExactRestoreBackend final : public Backend {
    Backend& platform_;
public:
    explicit ExactRestoreBackend(Backend& platform):platform_(platform){}
    HRESULT read(Resource r,ResourceState* out)override{return platform_.read(r,out);}
    HRESULT apply(Resource r,const ResourceState& target)override{return platform_.restore(r,target);}
    HRESULT restore(Resource r,const ResourceState& target)override{return platform_.restore(r,target);}
    HRESULT verify_install_health()override{return platform_.verify_install_health();}
    bool failure_may_have_mutated(Resource r,HRESULT status)const override{return platform_.failure_may_have_mutated(r,status);}
};

inline HRESULT inspect_committed_removal(Backend& backend,const durable_journal::Identity& installed,
        const durable_journal::Identity& removed,ResourceStates* original) {
    namespace j=durable_journal;namespace s=durable_snapshot;namespace o=durable_observer;
    if(!original||!j::detail::valid_identity(installed)||!j::detail::valid_identity(removed)||
       installed.schema_version!=3||removed.schema_version!=3||installed.owner!=removed.owner||
       installed.package_version!=removed.package_version||installed.package_manifest_sha256!=removed.package_manifest_sha256||
       !j::detail::same_path(installed.install_root,removed.install_root)||installed.token==removed.token)return E_INVALIDARG;
    const std::filesystem::path ledger(removed.storage_root);
    j::Store journal(ledger/L"transaction.journal",removed,ledger);s::Store snapshot(ledger/L"transaction.snapshot",removed,ledger);
    j::Document doc;std::string initialHash;ResourceStates initial{},current{};
    if(journal.read(removed,&doc)!=S_OK||!doc.committed||!doc.all_operations_succeeded||
       snapshot.read(removed,&initial,&initialHash)!=S_OK)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    SnapshotView historical(backend,initial);
    HRESULT status=inspect_committed_fresh(historical,installed);if(status!=S_OK)return status;
    using Kind=j::Event::Kind;
    if(doc.events.size()!=15||doc.events[0].kind!=Kind::intent||doc.events[0].intent.resource!="prestate"||
       doc.events[0].intent.action!="capture-uninstall"||doc.events[0].intent.desired_fingerprint!=initialHash||
       doc.events[1].kind!=Kind::observation||doc.events[1].observation.operation_status!=S_OK||
       doc.events[1].observation.observed_fingerprint!=initialHash||doc.events.back().kind!=Kind::commit)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const Resource order[]={Resource::com_x64,Resource::machine_com_x64,Resource::ui_element_category,
        Resource::keyboard_category,Resource::system_tray_category,Resource::profile};
    for(size_t i=0;i<6;++i) {
        const auto r=order[i];const auto& intent=doc.events[2+i*2];const auto& result=doc.events[3+i*2];
        if(intent.kind!=Kind::intent||intent.intent.resource!=o::detail::resource_name(r)||intent.intent.action!="apply"||
           result.kind!=Kind::observation||result.observation.sequence!=intent.intent.sequence||
           result.observation.operation_status!=S_OK||result.observation.observed_fingerprint!=intent.intent.desired_fingerprint)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ResourceState prior=initial[static_cast<size_t>(r)],empty{},projected{};std::string priorHash,emptyHash;
        if(r==Resource::machine_com_x64){if(backend.project_apply_state(r,prior,&projected)!=S_OK)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);prior=projected;}
        if(backend.project_apply_state(r,ResourceState{},&empty)!=S_OK||
           !o::detail::fingerprint_state(removed,r,prior,&priorHash,false)||
           !o::detail::fingerprint_state(removed,r,empty,&emptyHash,false)||
           intent.intent.expected_before_fingerprint!=priorHash||intent.intent.desired_fingerprint!=emptyHash)
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    status=detail::read_all(backend,current);if(status!=S_OK)return status;
    for(const auto& state:current)if(state!=ResourceState{})return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    std::string image;if(!s::detail::encode(removed,current,&image))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    j::detail::Digest digest{};if(!j::detail::sha256(image,&digest))return E_FAIL;
    static constexpr char hex[]="0123456789abcdef";std::string hash;for(auto byte:digest){hash.push_back(hex[byte>>4]);hash.push_back(hex[byte&15]);}
    if(hash!=doc.events.back().commit_fingerprint)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    *original=std::move(initial);return S_OK;
}

// The caller holds the lifecycle lock and supplies a protected new ledger.
// This restores only the exact verified pre-removal snapshot.
inline LifecycleResult restore_committed_removal(Backend& backend,const durable_journal::Identity& installed,
        const durable_journal::Identity& removed,const durable_journal::Identity& restoration) {
    namespace j=durable_journal;namespace s=durable_snapshot;
    if(!j::detail::valid_identity(restoration)||restoration.schema_version!=3||
       restoration.owner!=installed.owner||restoration.package_version!=installed.package_version||
       restoration.package_manifest_sha256!=installed.package_manifest_sha256||
       !j::detail::same_path(restoration.install_root,installed.install_root)||
       restoration.token==installed.token||restoration.token==removed.token)return {E_INVALIDARG,S_OK};
    ResourceStates original{};HRESULT status=inspect_committed_removal(backend,installed,removed,&original);
    if(status!=S_OK)return {status,S_OK};
    const std::filesystem::path ledger(restoration.storage_root);
    j::Store journal(ledger/L"transaction.journal",restoration,ledger);s::Store snapshot(ledger/L"transaction.snapshot",restoration,ledger);
    durable_observer::Observer durable(journal,snapshot,restoration);
    FreshObserver observer(durable);ExactRestoreBackend restore(backend);
    ResourceStates current{};status=detail::read_all(restore,current);if(status!=S_OK)return {status,S_OK};
    const std::vector<Resource> order{Resource::com_x64,Resource::machine_com_x64,Resource::profile,
        Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category};
    const auto result=detail::apply_transaction(restore,current,original,order,&observer,TransactionOperation::install_or_repair);
    if(result.operation!=S_OK||result.cleanup!=S_OK)return result;
    ResourceStates after{};status=detail::read_all(restore,after);
    if(status!=S_OK||after!=original)return {status!=S_OK?status:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)};
    status=observer.commit(after);return status==S_OK?result:LifecycleResult{status,status};
}
}
