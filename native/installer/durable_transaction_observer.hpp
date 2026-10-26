#pragma once

#include "dual_architecture_transaction.hpp"
#include "durable_journal.hpp"
#include "durable_transaction_snapshot.hpp"
#include <string>
#include <utility>

namespace boshiamy::installer::durable_observer {
namespace journal = boshiamy::installer::durable_journal;
namespace snapshot = boshiamy::installer::durable_snapshot;

namespace detail {
inline const char* resource_name(Resource resource) {
    switch(resource) {
    case Resource::com_x64:return "com-x64";
    case Resource::com_x86:return "com-x86";
    case Resource::machine_com_x64:return "com-machine-x64";
    case Resource::profile:return "profile";
    case Resource::keyboard_category:return "keyboard-category";
    case Resource::system_tray_category:return "system-tray-category";
    case Resource::ui_element_category:return "ui-element-category";
    default:return nullptr;
    }
}
inline const char* mutation_name(TransactionMutation mutation) {
    return mutation==TransactionMutation::apply?"apply":"restore";
}
inline bool fingerprint_state(const journal::Identity& identity,Resource resource,
                             const ResourceState& state,std::string* output,bool exact_com_tree=true) {
    if(!output||!journal::detail::valid_identity(identity))return false;
    const char* name=resource_name(resource);if(!name)return false;
    if(resource==Resource::machine_com_x64&&!journal::detail::machine_com_scope(identity))return false;
    ResourceStates states{};
    ResourceState projected=state;
    // COM private_data is the exact registry-tree rollback snapshot. The
    // apply intent/observation fingerprints describe owned COM semantics;
    // WindowsInstallerBackend independently validates that only owned values
    // changed and retains the complete observed tree for CAS/restore. Machine
    // apply projections retain explicit SID/root binding in private_data; only
    // the two historical user-COM raw trees are discarded here.
    if(!exact_com_tree&&(resource==Resource::com_x64||resource==Resource::com_x86))
        projected.private_data.clear();
    states[static_cast<size_t>(resource)]=std::move(projected);
    std::string image;
    if(!snapshot::detail::encode(identity,states,&image))return false;
    if(journal::detail::machine_com_scope(identity)) {
        // Domain-separate even absent resources, whose state bytes are equal.
        image.append("LiuKai-MachineScope-Resource-v1:");image.append(name);
    }
    journal::detail::Digest digest{};
    if(!journal::detail::sha256(image,&digest))return false;
    static constexpr char hex[]="0123456789abcdef";
    output->clear();output->reserve(64);
    for(uint8_t byte:digest){output->push_back(hex[byte>>4]);output->push_back(hex[byte&15]);}
    return true;
}
inline std::string unknown_fingerprint() {
    static const std::string value=[] {
        journal::detail::Digest digest{};
        const std::string input="LiuKai-TSF:readback-unavailable:v1";
        if(!journal::detail::sha256(input,&digest))return std::string(64,'0');
        static constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);
        for(uint8_t byte:digest){out.push_back(hex[byte>>4]);out.push_back(hex[byte&15]);}
        return out;
    }();
    return value;
}
}

using ApplyProjection = std::function<HRESULT(Resource,const ResourceState&,ResourceState*)>;

class Observer final : public TransactionObserver {
public:
    Observer(journal::Store& journal_store,snapshot::Store& snapshot_store,
             journal::Identity identity, ApplyProjection apply_projection = {})
        :journal_(journal_store),snapshot_(snapshot_store),identity_(std::move(identity)),
         apply_projection_(std::move(apply_projection)){}

    HRESULT begin(TransactionOperation operation,const ResourceStates& pre_state)override {
        if(started_)return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT status=snapshot_.create(pre_state);
        if(FAILED(status))return status;
        ResourceStates verified{};std::string pre_fingerprint;
        status=snapshot_.read(identity_,&verified,&pre_fingerprint);
        if(FAILED(status))return status;
        if(verified!=pre_state)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        status=journal_.create();
        if(FAILED(status))return status;
        journal::Document empty_journal;
        status=journal_.read(identity_,&empty_journal);
        if(FAILED(status)||!empty_journal.events.empty())
            return FAILED(status)?status:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        const std::string zero(64,'0');
        const char* action=operation==TransactionOperation::uninstall?"capture-uninstall":
            operation==TransactionOperation::migrate_x64_dev_host?"capture-x64-dev-host-migration":
            operation==TransactionOperation::repair_profile_display?"capture-profile-display-repair":"capture-install-or-repair";
        status=journal_.append_intent("prestate",action,zero,pre_fingerprint,&pending_sequence_);
        if(FAILED(status))return status;
        pending_=true;pending_resource_name_="prestate";pending_action_="capture";
        status=journal_.append_observation(pending_sequence_,S_OK,pre_fingerprint,false);
        if(FAILED(status))return status;
        pending_=false;pending_sequence_=0;pending_resource_name_.clear();pending_action_.clear();
        initial_state_=pre_state;latest_state_=pre_state;
        started_=true;
        return S_OK;
    }

    HRESULT before(Resource resource,TransactionMutation mutation,
                   const ResourceState& expected,const ResourceState& desired)override {
        if(!started_||pending_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        const char* name=detail::resource_name(resource);
        if(!name)return E_INVALIDARG;
        std::string before_fingerprint,desired_fingerprint;
        const bool exact_com_tree=mutation==TransactionMutation::restore;
        if(resource==Resource::machine_com_x64&&!exact_com_tree) {
            const HRESULT projected=fingerprint_for_mutation(resource,mutation,expected,&before_fingerprint);
            if(projected!=S_OK)return projected;
        } else if(!detail::fingerprint_state(identity_,resource,expected,&before_fingerprint,exact_com_tree))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const HRESULT projected=fingerprint_for_mutation(resource,mutation,desired,&desired_fingerprint);
        if(projected!=S_OK)return projected;
        uint64_t sequence=0;
        const HRESULT status=journal_.append_intent(name,detail::mutation_name(mutation),
            before_fingerprint,desired_fingerprint,&sequence);
        if(FAILED(status))return status;
        pending_=true;pending_sequence_=sequence;pending_resource_=resource;pending_resource_name_=name;
        pending_action_=detail::mutation_name(mutation);pending_mutation_=mutation;
        pending_expected_=expected;pending_desired_=desired;
        return S_OK;
    }

    HRESULT after(Resource resource,TransactionMutation mutation,HRESULT operation_status,
                  HRESULT readback_status,const ResourceState* observed,
                  bool mutation_may_have_occurred)override {
        if(!pending_||resource!=pending_resource_||mutation!=pending_mutation_)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string fingerprint;
        if(readback_status==S_OK&&observed) {
            const HRESULT projected=fingerprint_for_mutation(resource,mutation,*observed,&fingerprint);
            if(projected!=S_OK)return projected;
        } else {
            fingerprint=detail::unknown_fingerprint();
        }
        HRESULT recorded_status=operation_status!=S_OK?operation_status:readback_status;
        if(readback_status==S_OK&&!observed)recorded_status=E_UNEXPECTED;
        std::string desired_fingerprint;
        const HRESULT desired_status=fingerprint_for_mutation(resource,mutation,pending_desired_,&desired_fingerprint);
        if(desired_status!=S_OK)return desired_status;
        if(recorded_status==S_OK&&observed&&fingerprint!=desired_fingerprint)
            recorded_status=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        // The boolean records whether a write may have happened; an unavailable
        // read alone does not imply a write on pre-mutation paths. Callers pass
        // that fact explicitly, including successful/positive write statuses.
        const HRESULT status=journal_.append_observation(pending_sequence_,recorded_status,
            fingerprint,mutation_may_have_occurred);
        if(FAILED(status))return status; // Keep pending: caller must not mutate again.
        if(readback_status==S_OK&&observed)latest_state_[static_cast<size_t>(resource)]=*observed;
        pending_=false;pending_sequence_=0;pending_resource_name_.clear();pending_action_.clear();
        return S_OK;
    }

    HRESULT commit(const ResourceStates& final_state) override {
        if(!started_||pending_||committed_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if(final_state!=latest_state_)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        ResourceStates captured{};std::string snapshot_fingerprint;
        HRESULT status=snapshot_.read(identity_,&captured,&snapshot_fingerprint);
        if(FAILED(status))return status;
        if(captured!=initial_state_)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        std::string image;
        if(!snapshot::detail::encode(identity_,final_state,&image))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        journal::detail::Digest digest{};
        if(!journal::detail::sha256(image,&digest))return E_FAIL;
        static constexpr char hex[]="0123456789abcdef";std::string final_fingerprint;
        final_fingerprint.reserve(64);
        for(uint8_t byte:digest){final_fingerprint.push_back(hex[byte>>4]);final_fingerprint.push_back(hex[byte&15]);}
        status=journal_.append_commit(final_fingerprint);
        if(FAILED(status))return status;
        journal::Document verified;
        status=journal_.read(identity_,&verified);
        if(FAILED(status))return status;
        if(!verified.committed||!verified.all_operations_succeeded||verified.events.empty()||
           verified.events.back().kind!=journal::Event::Kind::commit||
           verified.events.back().commit_fingerprint!=final_fingerprint)
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        committed_=true;return S_OK;
    }

    bool pending()const{return pending_;}
    bool started()const{return started_;}
    bool committed()const{return committed_;}
private:
    HRESULT fingerprint_for_mutation(Resource resource, TransactionMutation mutation,
        const ResourceState& state, std::string* output) const {
        ResourceState projected=state;
        if(mutation==TransactionMutation::apply && apply_projection_) {
            const HRESULT status=apply_projection_(resource,state,&projected);
            if(status!=S_OK)return status;
        }
        return detail::fingerprint_state(identity_,resource,projected,output,
            mutation==TransactionMutation::restore) ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    journal::Store& journal_;
    snapshot::Store& snapshot_;
    journal::Identity identity_;
    ApplyProjection apply_projection_;
    bool started_=false;
    bool pending_=false;
    uint64_t pending_sequence_=0;
    Resource pending_resource_=Resource::count;
    std::string pending_resource_name_;
    std::string pending_action_;
    TransactionMutation pending_mutation_=TransactionMutation::apply;
    ResourceState pending_expected_,pending_desired_;
    ResourceStates initial_state_{},latest_state_{};
    bool committed_=false;
};
} // namespace boshiamy::installer::durable_observer
