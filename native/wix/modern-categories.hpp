#pragma once
#include "../installer/package_categories.hpp"
#include "../installer/durable_journal.hpp"
namespace boshiamy::installer::msi_modern_categories {
namespace c=package_categories;
// The caller must provide a durable observer and hold the lifecycle lock.
// Any failed/uncertain mutation is left for explicit journal-based recovery.
class Observer {
public:
    virtual ~Observer()=default;
    virtual HRESULT begin(const c::Categories& before,const c::Categories& target)=0;
    virtual HRESULT before(const GUID& id,bool add,const c::Categories& expected,const c::Categories& target)=0;
    virtual HRESULT after(const GUID& id,bool add,HRESULT operation,HRESULT readback,const c::Categories* observed)=0;
    virtual HRESULT commit(const c::Categories& state)=0;
};

inline bool category_fingerprint(const durable_journal::Identity& identity,const c::Categories& state,std::string* out) {
    if(!out||!durable_journal::detail::valid_identity(identity))return false;
    unsigned mask=0;const GUID ids[]={GUID_TFCAT_TIP_KEYBOARD,c::tray,c::ui,c::immersive,c::input_mode};
    for(const auto& item:state){bool found=false;for(unsigned i=0;i<5;++i)if(IsEqualGUID(item,ids[i])){
        if(mask&(1u<<i))return false;mask|=1u<<i;found=true;break;}
        if(!found)return false;}
    std::string image=durable_journal::detail::header_payload(identity);
    image.append("LiuKai-modern-category-set-v1:");image.push_back(static_cast<char>(mask));
    durable_journal::detail::Digest digest{};if(!durable_journal::detail::sha256(image,&digest))return false;
    static constexpr char hex[]="0123456789abcdef";out->clear();out->reserve(64);
    for(auto byte:digest){out->push_back(hex[byte>>4]);out->push_back(hex[byte&15]);}return true;
}

class DurableObserver final : public Observer {
    durable_journal::Store& store_;durable_journal::Identity identity_;
    c::Categories latest_,final_,pendingTarget_;GUID pendingId_{};bool pendingAdd_=false,started_=false,pending_=false;
    uint64_t sequence_=0;
public:
    DurableObserver(durable_journal::Store& store,durable_journal::Identity identity):store_(store),identity_(std::move(identity)){}
    HRESULT begin(const c::Categories& before,const c::Categories& target)override {
        if(started_||pending_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string initial;if(!category_fingerprint(identity_,before,&initial))return E_INVALIDARG;
        HRESULT hr=store_.create();if(hr!=S_OK)return hr;
        uint64_t seq=0;hr=store_.append_intent("category-set","capture",std::string(64,'0'),initial,&seq);if(hr!=S_OK)return hr;
        hr=store_.append_observation(seq,S_OK,initial,false);if(hr!=S_OK)return hr;
        latest_=before;final_=target;started_=true;return S_OK;
    }
    HRESULT before(const GUID& id,bool add,const c::Categories& expected,const c::Categories& target)override {
        if(!started_||pending_||!c::same(latest_,expected)||!c::extra(id))return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string beforeHash,targetHash;if(!category_fingerprint(identity_,expected,&beforeHash)||!category_fingerprint(identity_,target,&targetHash))return E_INVALIDARG;
        const char* name=IsEqualGUID(id,c::immersive)?"immersive":"input-mode";
        HRESULT hr=store_.append_intent(name,add?"add":"remove",beforeHash,targetHash,&sequence_);if(hr!=S_OK)return hr;
        pending_=true;pendingId_=id;pendingAdd_=add;pendingTarget_=target;return S_OK;
    }
    HRESULT after(const GUID& id,bool add,HRESULT op,HRESULT read,const c::Categories* observed)override {
        if(!pending_||!IsEqualGUID(id,pendingId_)||add!=pendingAdd_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string actual(64,'0');HRESULT recorded=op!=S_OK?op:read;
        if(read==S_OK&&observed){if(!category_fingerprint(identity_,*observed,&actual))return E_INVALIDARG;
            if(recorded==S_OK&&!c::same(*observed,pendingTarget_))recorded=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        }else if(recorded==S_OK)recorded=E_UNEXPECTED;
        const HRESULT hr=store_.append_observation(sequence_,recorded,actual,true);if(hr!=S_OK)return hr;
        if(read==S_OK&&observed)latest_=*observed;pending_=false;return S_OK;
    }
    HRESULT commit(const c::Categories& state)override {
        if(!started_||pending_||!c::same(state,latest_)||!c::same(state,final_))return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string hash;if(!category_fingerprint(identity_,state,&hash))return E_INVALIDARG;
        HRESULT hr=store_.append_commit(hash);if(hr!=S_OK)return hr;
        durable_journal::Document doc;hr=store_.read(identity_,&doc);
        return hr==S_OK&&doc.committed&&doc.all_operations_succeeded&&doc.events.back().commit_fingerprint==hash?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
};

// Validate the complete event sequence and live set, not merely the commit
// bit. This is read-only and does not authorize recovery of partial journals.
inline HRESULT inspect_committed(c::Store& categories,durable_journal::Store& journal,
        const durable_journal::Identity& identity,bool added) {
    namespace j=durable_journal;using Kind=j::Event::Kind;
    j::Document doc;if(journal.read(identity,&doc)!=S_OK||!doc.committed||!doc.all_operations_succeeded||doc.events.size()!=7)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    c::Categories state{GUID_TFCAT_TIP_KEYBOARD,c::tray,c::ui};
    if(!added){state.push_back(c::immersive);state.push_back(c::input_mode);}
    std::string current;if(!category_fingerprint(identity,state,&current))return E_INVALIDARG;
    const auto& capture=doc.events[0];const auto& captured=doc.events[1];
    if(capture.kind!=Kind::intent||capture.intent.resource!="category-set"||capture.intent.action!="capture"||
       capture.intent.expected_before_fingerprint!=std::string(64,'0')||capture.intent.desired_fingerprint!=current||
       captured.kind!=Kind::observation||captured.observation.sequence!=capture.intent.sequence||
       captured.observation.operation_status!=S_OK||captured.observation.observed_fingerprint!=current||captured.observation.mutation_may_have_occurred)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const GUID ids[]={c::immersive,c::input_mode};const char* names[]={"immersive","input-mode"};
    for(size_t i=0;i<2;++i){
        if(added)state.push_back(ids[i]);else state.erase(std::remove_if(state.begin(),state.end(),[&](const GUID& v){return IsEqualGUID(v,ids[i]);}),state.end());
        std::string next;if(!category_fingerprint(identity,state,&next))return E_INVALIDARG;
        const auto& intent=doc.events[2+i*2];const auto& observed=doc.events[3+i*2];
        if(intent.kind!=Kind::intent||intent.intent.resource!=names[i]||intent.intent.action!=(added?"add":"remove")||
           intent.intent.expected_before_fingerprint!=current||intent.intent.desired_fingerprint!=next||
           observed.kind!=Kind::observation||observed.observation.sequence!=intent.intent.sequence||
           observed.observation.operation_status!=S_OK||observed.observation.observed_fingerprint!=next)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        current=next;
    }
    if(doc.events[6].kind!=Kind::commit||doc.events[6].commit_fingerprint!=current)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    c::Categories live;HRESULT hr=c::capture(categories,&live);if(hr!=S_OK)return hr;
    return c::same(live,state)?S_OK:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
}
inline HRESULT transition(c::Store& store,const c::Categories& expected,bool add,Observer& journal) {
    c::Categories current;
    HRESULT hr=c::capture(store,&current);if(hr!=S_OK)return hr;
    if(!c::same(current,expected))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if(!c::contains(current,GUID_TFCAT_TIP_KEYBOARD)||!c::contains(current,c::tray)||!c::contains(current,c::ui))
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if(add&&(c::contains(current,c::immersive)||c::contains(current,c::input_mode)))return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if(!add&&(!c::contains(current,c::immersive)||!c::contains(current,c::input_mode)))return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    c::Categories final=current;
    if(add){final.push_back(c::immersive);final.push_back(c::input_mode);}
    else final.erase(std::remove_if(final.begin(),final.end(),c::extra),final.end());
    hr=journal.begin(current,final);if(hr!=S_OK)return hr;
    const GUID order[]={c::immersive,c::input_mode};
    for(const auto& id:order) {
        c::Categories next=current;
        if(add)next.push_back(id);else next.erase(std::remove_if(next.begin(),next.end(),[&](const GUID& x){return IsEqualGUID(x,id);}),next.end());
        hr=journal.before(id,add,current,next);if(hr!=S_OK)return hr;
        c::Categories fresh;const HRESULT readBefore=c::capture(store,&fresh);
        if(readBefore!=S_OK||!c::same(fresh,current)) {
            const HRESULT conflict=readBefore!=S_OK?readBefore:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            const HRESULT recorded=journal.after(id,add,conflict,readBefore,readBefore==S_OK?&fresh:nullptr);
            return recorded!=S_OK?recorded:conflict;
        }
        const HRESULT operation=add?store.add(id):store.remove(id);
        const HRESULT readAfter=c::capture(store,&fresh);
        const HRESULT recorded=journal.after(id,add,operation,readAfter,readAfter==S_OK?&fresh:nullptr);
        if(recorded!=S_OK)return recorded;
        if(operation!=S_OK)return operation;
        if(readAfter!=S_OK)return readAfter;
        if(!c::same(fresh,next))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        current=std::move(fresh);
    }
    return journal.commit(current);
}

// Recover only confirmed successful steps. A pending intent is accepted only
// when live state still equals the last completed state; a failed operation
// must have independently observed that same unchanged state.
inline HRESULT recover_partial(c::Store& categories,durable_journal::Store& forward,
        const durable_journal::Identity& identity,bool wasAdd,Observer& recovery) {
    namespace j=durable_journal;using Kind=j::Event::Kind;
    j::Document doc;if(forward.read(identity,&doc)!=S_OK||doc.committed||doc.events.size()<2||doc.events.size()>6)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    c::Categories current{GUID_TFCAT_TIP_KEYBOARD,c::tray,c::ui};
    if(!wasAdd){current.push_back(c::immersive);current.push_back(c::input_mode);}
    const c::Categories original=current;std::string hash;
    if(!category_fingerprint(identity,current,&hash))return E_INVALIDARG;
    if(doc.events[0].kind!=Kind::intent||doc.events[0].intent.resource!="category-set"||
       doc.events[0].intent.action!="capture"||doc.events[0].intent.expected_before_fingerprint!=std::string(64,'0')||
       doc.events[0].intent.desired_fingerprint!=hash||doc.events[1].kind!=Kind::observation||
       doc.events[1].observation.sequence!=doc.events[0].intent.sequence||doc.events[1].observation.operation_status!=S_OK||
       doc.events[1].observation.observed_fingerprint!=hash)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const GUID ids[]={c::immersive,c::input_mode};const char* names[]={"immersive","input-mode"};
    size_t index=2;
    for(size_t step=0;index<doc.events.size()&&step<2;++step) {
        const auto& intent=doc.events[index++];c::Categories next=current;
        if(wasAdd)next.push_back(ids[step]);else next.erase(std::remove_if(next.begin(),next.end(),[&](const GUID& x){return IsEqualGUID(x,ids[step]);}),next.end());
        std::string nextHash;if(!category_fingerprint(identity,next,&nextHash))return E_INVALIDARG;
        if(intent.kind!=Kind::intent||intent.intent.resource!=names[step]||intent.intent.action!=(wasAdd?"add":"remove")||
           intent.intent.expected_before_fingerprint!=hash||intent.intent.desired_fingerprint!=nextHash)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if(index==doc.events.size())break; // Pending: only the previous state can be proven safe.
        const auto& observed=doc.events[index++];
        if(observed.kind!=Kind::observation||observed.observation.sequence!=intent.intent.sequence)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if(observed.observation.operation_status==S_OK&&observed.observation.observed_fingerprint==nextHash){current=std::move(next);hash=nextHash;continue;}
        if(observed.observation.operation_status!=S_OK&&observed.observation.observed_fingerprint==hash&&index==doc.events.size())break;
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    if(index!=doc.events.size())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    c::Categories live;HRESULT hr=c::capture(categories,&live);if(hr!=S_OK)return hr;
    if(!c::same(live,current))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    hr=recovery.begin(current,original);if(hr!=S_OK)return hr;
    for(int step=1;step>=0;--step){
        const bool shouldHave=c::contains(original,ids[step]),has=c::contains(current,ids[step]);
        if(has==shouldHave)continue;
        c::Categories next=current;if(shouldHave)next.push_back(ids[step]);else next.erase(std::remove_if(next.begin(),next.end(),[&](const GUID& x){return IsEqualGUID(x,ids[step]);}),next.end());
        hr=recovery.before(ids[step],shouldHave,current,next);if(hr!=S_OK)return hr;
        hr=c::capture(categories,&live);
        if(hr!=S_OK||!c::same(live,current)) {
            const HRESULT conflict=hr!=S_OK?hr:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            const HRESULT logged=recovery.after(ids[step],shouldHave,conflict,hr,hr==S_OK?&live:nullptr);
            return logged!=S_OK?logged:conflict;
        }
        const HRESULT op=shouldHave?categories.add(ids[step]):categories.remove(ids[step]);
        const HRESULT read=c::capture(categories,&live);
        const HRESULT logged=recovery.after(ids[step],shouldHave,op,read,read==S_OK?&live:nullptr);
        if(logged!=S_OK)return logged;if(op!=S_OK)return op;if(read!=S_OK)return read;
        if(!c::same(live,next))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        current=std::move(live);
    }
    return recovery.commit(current);
}
}
