#ifndef BOSHIAMY_CANDIDATE_UI_ELEMENT_HPP
#define BOSHIAMY_CANDIDATE_UI_ELEMENT_HPP
#define BOSHIAMY_READING_UI_ELEMENT_AVAILABLE 1

#include <windows.h>
#include <msctf.h>
#include <oleauto.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace boshiamy::candidate_ui {

// Microsoft SDK ITfCandidateListUIElement IID and method order. MinGW's
// msctf.h omits this interface, so keep the compatible declaration local.
inline constexpr IID iid_candidate_list_ui_element{
    0xea1ea138,0x19df,0x11d7,{0xa6,0xd2,0x00,0x06,0x5b,0x84,0x43,0x5c}};
struct CandidateListUiElement : ITfUIElement {
    virtual HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSelection(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetString(UINT,BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPageIndex(UINT*,UINT,UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*,UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT*) = 0;
};

// Microsoft SDK msctf.h declares ITfCandidateListUIElementBehavior as an
// extension of ITfCandidateListUIElement. MinGW omits the declaration.
inline constexpr IID iid_candidate_list_ui_element_behavior{
    0x85fad185,0x58ce,0x497a,{0x94,0x60,0x35,0x53,0x66,0xb6,0x4b,0x9a}};
struct CandidateListUiElementBehavior : CandidateListUiElement {
    virtual HRESULT STDMETHODCALLTYPE SetSelection(UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE Finalize() = 0;
    virtual HRESULT STDMETHODCALLTYPE Abort() = 0;
};

// MinGW's bundled msctf.h also omits ITfReadingInformationUIElement. Keep
// the SDK ABI here so reading hints are not exposed as selectable candidates.
inline constexpr IID iid_reading_information_ui_element{
    0xea1ea139,0x19df,0x11d7,{0xa6,0xd2,0x00,0x06,0x5b,0x84,0x43,0x5c}};
struct ReadingInformationUiElement : ITfUIElement {
    virtual HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContext(ITfContext**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetString(BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaxReadingStringLength(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetErrorIndex(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsVerticalOrderPreferred(BOOL*) = 0;
};

// document is borrowed by the caller. Element::replace adds a reference while
// active and releases it at end_snapshot, even if a host retains this COM object.
struct Snapshot {
    std::wstring description;
    GUID guid{};
    ITfDocumentMgr* document=nullptr;
    std::vector<std::wstring> strings;
    std::vector<UINT> page_starts;
    UINT selection=0;
    UINT current_page=0;
    DWORD updated_flags=0;
};
struct Callbacks {
    std::function<HRESULT(const std::vector<UINT>&)> set_page_indices;
    std::function<HRESULT(UINT)> set_selection;
    std::function<HRESULT(UINT)> finalize;
    std::function<HRESULT()> abort;
    std::function<HRESULT(BOOL)> show;
    std::function<HRESULT(BOOL*)> is_shown;
};

class CandidateUiElement final : public CandidateListUiElementBehavior {
public:
    static CandidateUiElement* Create(const Snapshot& snapshot,
        std::weak_ptr<Callbacks> callbacks,std::shared_ptr<void> module_pin) noexcept {
        try { return new CandidateUiElement(snapshot,std::move(callbacks),std::move(module_pin)); }
        catch (...) { return nullptr; }
    }
    HRESULT replace(const Snapshot& next,bool preserve_flags=false) noexcept {
        try {
            Snapshot copy=next;
            if(copy.document)copy.document->AddRef();
            ITfDocumentMgr* old=nullptr;
            bool rejected=false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(ended_)rejected=true;
                else{
                    old=data_.document;
                    if(preserve_flags)copy.updated_flags=data_.updated_flags;
                    data_=std::move(copy);
                    ++generation_;
                }
            }
            if(rejected){if(copy.document)copy.document->Release();return E_UNEXPECTED;}
            if(old)old->Release();
            return S_OK;
        }catch(...){return E_OUTOFMEMORY;}
    }
    void detach_callbacks() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.reset();
    }
    void end_snapshot() noexcept {
        ITfDocumentMgr* old=nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(ended_)return;
            ended_=true;
            old=data_.document;
            data_.document=nullptr;
            data_.strings.clear();
            data_.page_starts.clear();
            data_.selection=0;
            data_.current_page=0;
            data_.updated_flags=0;
            callbacks_.reset();
            requested_=false;
            ++generation_;
        }
        if(old)old->Release();
    }
    void apply_begin_show(BOOL show) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!ended_&&!explicit_show_)requested_=show!=FALSE;
    }
    bool native_requested() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return !ended_&&requested_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        if(IsEqualIID(iid,IID_IUnknown)||IsEqualIID(iid,IID_ITfUIElement))
            *out=static_cast<ITfUIElement*>(this);
        else if(IsEqualIID(iid,iid_candidate_list_ui_element))
            *out=static_cast<CandidateListUiElement*>(this);
        else if(IsEqualIID(iid,iid_candidate_list_ui_element_behavior))
            *out=static_cast<CandidateListUiElementBehavior*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG remaining=--references_;if(!remaining)delete this;return remaining;}
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        try {
            std::wstring description;
            {std::lock_guard<std::mutex> lock(mutex_);description=data_.description;}
            *out=SysAllocStringLen(description.data(),static_cast<UINT>(description.size()));
            return *out?S_OK:E_OUTOFMEMORY;
        }catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* out) override {
        if(!out)return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);*out=data_.guid;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL requested) override {
        std::shared_ptr<Callbacks> callbacks;
        bool previous=false,previous_explicit=false;
        unsigned long long serial=0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(ended_)return requested?S_FALSE:S_OK;
            callbacks=callbacks_.lock();
            if(!callbacks||!callbacks->show)return E_UNEXPECTED;
            previous=requested_;
            previous_explicit=explicit_show_;
            requested_=requested!=FALSE;
            explicit_show_=true;
            serial=++generation_;
        }
        HRESULT hr=E_FAIL;
        try{hr=callbacks->show(requested);}catch(...){hr=E_FAIL;}
        if(FAILED(hr)){
            std::lock_guard<std::mutex> lock(mutex_);
            if(!ended_&&generation_==serial){
                requested_=previous;
                explicit_show_=previous_explicit;
                ++generation_;
            }
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* out) override {
        if(!out)return E_POINTER;
        *out=FALSE;
        std::shared_ptr<Callbacks> callbacks;
        {std::lock_guard<std::mutex> lock(mutex_);if(ended_)return S_FALSE;callbacks=callbacks_.lock();}
        if(!callbacks||!callbacks->is_shown)return S_FALSE;
        try{return callbacks->is_shown(out);}catch(...){*out=FALSE;return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* out) override {
        if(!out)return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);*out=data_.updated_flags;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!data_.document)return S_FALSE;
        data_.document->AddRef();*out=data_.document;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCount(UINT* out) override {
        if(!out)return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        *out=static_cast<UINT>(data_.strings.size());return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* out) override {
        if(!out)return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        if(data_.strings.empty()){*out=0;return S_FALSE;}
        *out=data_.selection;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override {
        std::shared_ptr<Callbacks> callbacks;
        unsigned long long generation=0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(ended_)return S_FALSE;
            if(index>=data_.strings.size())return E_INVALIDARG;
            callbacks=callbacks_.lock();
            generation=generation_;
        }
        if(!callbacks||!callbacks->set_selection)return E_UNEXPECTED;
        HRESULT hr=E_FAIL;
        try{hr=callbacks->set_selection(index);}catch(...){hr=E_FAIL;}
        if(hr!=S_OK)return hr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(ended_)return S_FALSE;
            const auto current_callbacks=callbacks_.lock();
            if(!current_callbacks||current_callbacks.get()!=callbacks.get())return E_UNEXPECTED;
            // A reentrant engine publish remains authoritative.
            if(generation_==generation){
                data_.selection=index;
                data_.current_page=0;
                for(std::size_t page=1;page<data_.page_starts.size();++page){
                    if(data_.page_starts[page]>index)break;
                    data_.current_page=static_cast<UINT>(page);
                }
                ++generation_;
            }
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Finalize() override {
        std::shared_ptr<Callbacks> callbacks;
        UINT selection=0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(ended_)return S_FALSE;
            callbacks=callbacks_.lock();
            selection=data_.selection;
        }
        if(!callbacks||!callbacks->finalize)return E_UNEXPECTED;
        try{return callbacks->finalize(selection);}catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE Abort() override {
        std::shared_ptr<Callbacks> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(ended_)return S_FALSE;
            callbacks=callbacks_.lock();
        }
        if(!callbacks||!callbacks->abort)return E_UNEXPECTED;
        try{return callbacks->abort();}catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE GetString(UINT index,BSTR* out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        try {
            std::wstring value;
            {std::lock_guard<std::mutex> lock(mutex_);
             if(index>=data_.strings.size())return E_INVALIDARG;
             value=data_.strings[index];}
            *out=SysAllocStringLen(value.data(),static_cast<UINT>(value.size()));
            return *out?S_OK:E_OUTOFMEMORY;
        }catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* indices,UINT capacity,UINT* count) override {
        if(!count)return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        const UINT needed=static_cast<UINT>(data_.page_starts.size());
        *count=needed;
        if(!indices)return S_OK;
        if(capacity < needed)return E_INVALIDARG;
        for(UINT i=0;i<needed;++i)indices[i]=data_.page_starts[i];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* indices,UINT count) override {
        if(!indices)return E_POINTER;
        try {
            std::vector<UINT> pages;
            std::shared_ptr<Callbacks> callbacks;
            unsigned long long generation=0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const std::size_t candidates=data_.strings.size();
                if(!ended_&&candidates==0)return E_FAIL;
                if(ended_||count==0||count>candidates)return E_INVALIDARG;
                pages.assign(indices,indices+count);
                if(pages[0]!=0)return E_INVALIDARG;
                for(std::size_t i=0;i<pages.size();++i)
                    if(pages[i]>=candidates||(i&&pages[i]<=pages[i-1]))return E_INVALIDARG;
                callbacks=callbacks_.lock();
                generation=generation_;
            }
            if(!callbacks||!callbacks->set_page_indices)return E_UNEXPECTED;
            HRESULT hr=callbacks->set_page_indices(pages);
            if(hr!=S_OK)return hr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(ended_)return S_FALSE;
                const auto current_callbacks=callbacks_.lock();
                if(!current_callbacks||current_callbacks.get()!=callbacks.get())return E_UNEXPECTED;
                // A reentrant engine publish is authoritative over this setter.
                if(generation_==generation){
                    data_.page_starts=std::move(pages);
                    // The engine's accepted host page map resets its cursor.
                    data_.selection=0;
                    data_.current_page=0;
                    ++generation_;
                }
            }
            return hr;
        }catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* out) override {
        if(!out)return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);*out=data_.current_page;return S_OK;
    }
private:
    CandidateUiElement(const Snapshot& snapshot,std::weak_ptr<Callbacks> callbacks,std::shared_ptr<void> module_pin)
        :data_(snapshot),callbacks_(std::move(callbacks)),module_pin_(std::move(module_pin)){
        if(data_.document)data_.document->AddRef();
    }
    ~CandidateUiElement(){if(data_.document)data_.document->Release();}
    std::atomic<ULONG> references_{1};
    mutable std::mutex mutex_;
    Snapshot data_;
    std::weak_ptr<Callbacks> callbacks_;
    std::shared_ptr<void> module_pin_;
    bool ended_=false,requested_=true,explicit_show_=false;
    unsigned long long generation_=0;
};

class CandidateUiController final : public std::enable_shared_from_this<CandidateUiController> {
public:
    static std::shared_ptr<CandidateUiController> Create(ITfUIElementMgr* manager,
        std::shared_ptr<void> module_pin) {
        if(!manager)return {};
        return std::shared_ptr<CandidateUiController>(new CandidateUiController(manager,std::move(module_pin)));
    }
    ~CandidateUiController(){
        CandidateUiElement* element=nullptr;DWORD ending_id=invalid_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            element=element_;element_=nullptr;
            if(state_==State::Active||state_==State::EndFailed)ending_id=id_;
            state_=State::Idle;id_=invalid_id;
        }
        if(element)element->detach_callbacks();
        if(ending_id!=invalid_id&&manager_)manager_->EndUIElement(ending_id);
        if(element){element->end_snapshot();element->Release();}
        if(manager_)manager_->Release();
    }
    HRESULT begin(const Snapshot& snapshot,std::weak_ptr<Callbacks> callbacks) noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;
        CandidateUiElement* fresh=CandidateUiElement::Create(snapshot,std::move(callbacks),module_pin_);
        if(!fresh)return E_OUTOFMEMORY;
        bool busy=false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_!=State::Idle)busy=true;
            else{
                element_=fresh;state_=State::Beginning;pending_end_=false;
                host_notifications_=false;
                id_=invalid_id;
            }
        }
        if(busy){fresh->Release();return E_PENDING;}
        BOOL show=TRUE;DWORD begun_id=invalid_id;
        HRESULT hr=manager_->BeginUIElement(static_cast<ITfUIElement*>(fresh),&show,&begun_id);
        bool deferred=false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(hr!=S_OK){
                state_=State::Idle;element_=nullptr;id_=invalid_id;pending_end_=false;
                host_notifications_=false;
            }else{
                id_=begun_id;state_=State::Active;deferred=pending_end_;
                pending_end_=false;
                host_notifications_=show==FALSE;
                fresh->apply_begin_show(show);
            }
        }
        if(hr!=S_OK){fresh->end_snapshot();fresh->Release();return hr;}
        if(deferred)return end();
        return hr;
    }
    HRESULT update(const Snapshot& snapshot) noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;
        CandidateUiElement* element=nullptr;DWORD current=invalid_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_!=State::Active||!element_)return E_PENDING;
            element=element_;element->AddRef();current=id_;
        }
        HRESULT hr=element->replace(snapshot);
        if(SUCCEEDED(hr)){
            bool still_active=false;
            {std::lock_guard<std::mutex> lock(mutex_);
             still_active=state_==State::Active&&id_==current&&element_==element;}
            hr=still_active?manager_->UpdateUIElement(current):S_FALSE;
        }
        element->Release();return hr;
    }
    HRESULT silent_replace(const Snapshot& snapshot,bool preserve_flags=false) noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;
        CandidateUiElement* element=nullptr;DWORD current=invalid_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_!=State::Active||!element_)return E_PENDING;
            element=element_;element->AddRef();current=id_;
        }
        HRESULT hr=element->replace(snapshot,preserve_flags);
        bool still_active=false;
        {std::lock_guard<std::mutex> lock(mutex_);
         still_active=state_==State::Active&&id_==current&&element_==element;}
        element->Release();
        return SUCCEEDED(hr)&&!still_active?S_FALSE:hr;
    }
    HRESULT end() noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;
        CandidateUiElement* element=nullptr;DWORD ending_id=invalid_id;
        bool deferred=false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_==State::Idle)return S_FALSE;
            if(state_==State::Beginning){
                pending_end_=true;
                deferred=true;
            }else if(state_==State::Ending)return S_OK;
            else{
                state_=State::Ending;
                ending_id=id_;
            }
            element=element_;
            if(element)element->AddRef();
        }
        // Disable actions before notifying the host, while keeping the final
        // immutable data readable during its synchronous End callback.
        if(element)element->detach_callbacks();
        if(deferred){
            if(element){element->end_snapshot();element->Release();}
            return S_OK;
        }
        const HRESULT hr=manager_->EndUIElement(ending_id);
        // Document Release may reenter end(); State::Ending prevents a second
        // notification. Failed notifications still clear data before returning.
        if(element)element->end_snapshot();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(hr==S_OK){
                element_=nullptr;id_=invalid_id;state_=State::Idle;
            }else state_=State::EndFailed;
        }
        if(hr==S_OK&&element)element->Release(); // controller's owning ref
        if(element)element->Release(); // short pin across callbacks
        return hr;
    }
    bool suppressed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return element_&&state_==State::Active&&!element_->native_requested();
    }
    bool host_notifications() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_==State::Active&&element_&&host_notifications_;
    }
    DWORD id() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);return id_;
    }
private:
    CandidateUiController(ITfUIElementMgr* manager,std::shared_ptr<void> module_pin)
        :manager_(manager),module_pin_(std::move(module_pin)){if(manager_)manager_->AddRef();}
    static constexpr DWORD invalid_id=0xffffffffu;
    enum class State {Idle,Beginning,Active,Ending,EndFailed};
    mutable std::mutex mutex_;
    ITfUIElementMgr* manager_=nullptr;
    std::shared_ptr<void> module_pin_;
    CandidateUiElement* element_=nullptr;
    DWORD id_=invalid_id;
    State state_=State::Idle;
    bool pending_end_=false;
    bool host_notifications_=false;
};

struct ReadingSnapshot {
    ITfContext* context=nullptr;
    std::wstring text;
    DWORD updated_flags=0x1f;
};

class ReadingUiElement final : public ReadingInformationUiElement {
public:
    static ReadingUiElement* Create(const ReadingSnapshot& snapshot,
        std::weak_ptr<Callbacks> callbacks,std::shared_ptr<void> module_pin) noexcept {
        try { return new ReadingUiElement(snapshot,std::move(callbacks),std::move(module_pin)); }
        catch (...) { return nullptr; }
    }
    void apply_begin_show(BOOL show) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!ended_&&!explicit_show_)requested_=show!=FALSE;
    }
    bool native_requested() const noexcept { std::lock_guard<std::mutex> lock(mutex_);return !ended_&&requested_; }
    HRESULT replace(const ReadingSnapshot& next) noexcept {
        try {
            std::wstring text=next.text;
            ITfContext* context=next.context;if(context)context->AddRef();
            ITfContext* old=nullptr;bool rejected=false;
            {std::lock_guard<std::mutex> lock(mutex_);if(ended_)rejected=true;else{old=context_;context_=context;text_=std::move(text);flags_=next.updated_flags;++generation_;}}
            if(rejected){if(context)context->Release();return E_UNEXPECTED;}
            if(old)old->Release();return S_OK;
        }catch(...){return E_OUTOFMEMORY;}
    }
    void end_snapshot() noexcept {
        ITfContext* old=nullptr;
        {std::lock_guard<std::mutex> lock(mutex_);if(ended_)return;ended_=true;old=context_;context_=nullptr;text_.clear();callbacks_.reset();requested_=false;++generation_;}
        if(old)old->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(IsEqualIID(iid,IID_IUnknown)||IsEqualIID(iid,IID_ITfUIElement))*out=static_cast<ITfUIElement*>(this);
        else if(IsEqualIID(iid,iid_reading_information_ui_element))*out=static_cast<ReadingInformationUiElement*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG remaining=--references_;if(!remaining)delete this;return remaining;}
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* out) override {return alloc(L"嘸．改讀音提示",out);}
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* out) override {if(!out)return E_POINTER;*out=guid_;return S_OK;}
    HRESULT STDMETHODCALLTYPE Show(BOOL requested) override {
        std::shared_ptr<Callbacks> callbacks;bool previous=false,previous_explicit=false;unsigned long long serial=0;
        {std::lock_guard<std::mutex> lock(mutex_);if(ended_)return requested?S_FALSE:S_OK;callbacks=callbacks_.lock();if(!callbacks||!callbacks->show)return E_UNEXPECTED;previous=requested_;previous_explicit=explicit_show_;requested_=requested!=FALSE;explicit_show_=true;serial=++generation_;}
        HRESULT hr=E_FAIL;try{hr=callbacks->show(requested);}catch(...){hr=E_FAIL;}
        if(FAILED(hr)){std::lock_guard<std::mutex> lock(mutex_);if(!ended_&&generation_==serial){requested_=previous;explicit_show_=previous_explicit;++generation_;}}
        return hr;
    }
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* out) override {
        if(!out)return E_POINTER;*out=FALSE;std::shared_ptr<Callbacks> callbacks;
        {std::lock_guard<std::mutex> lock(mutex_);if(ended_)return S_FALSE;callbacks=callbacks_.lock();}
        if(!callbacks||!callbacks->is_shown)return S_FALSE;try{return callbacks->is_shown(out);}catch(...){*out=FALSE;return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* out) override {if(!out)return E_POINTER;std::lock_guard<std::mutex> lock(mutex_);*out=ended_?0:flags_;return ended_?S_FALSE:S_OK;}
    HRESULT STDMETHODCALLTYPE GetContext(ITfContext** out) override {if(!out)return E_POINTER;*out=nullptr;std::lock_guard<std::mutex> lock(mutex_);if(ended_||!context_)return S_FALSE;context_->AddRef();*out=context_;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetString(BSTR* out) override {if(!out)return E_POINTER;*out=nullptr;try{std::wstring value;{std::lock_guard<std::mutex> lock(mutex_);if(ended_)return S_FALSE;value=text_;}return alloc(value,out);}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE GetMaxReadingStringLength(UINT* out) override {if(!out)return E_POINTER;std::lock_guard<std::mutex> lock(mutex_);*out=ended_?0:static_cast<UINT>(text_.size());return ended_?S_FALSE:S_OK;}
    HRESULT STDMETHODCALLTYPE GetErrorIndex(UINT* out) override {if(!out)return E_POINTER;std::lock_guard<std::mutex> lock(mutex_);*out=static_cast<UINT>(-1);return ended_?S_FALSE:S_OK;}
    HRESULT STDMETHODCALLTYPE IsVerticalOrderPreferred(BOOL* out) override {if(!out)return E_POINTER;std::lock_guard<std::mutex> lock(mutex_);*out=FALSE;return ended_?S_FALSE:S_OK;}
private:
    ReadingUiElement(const ReadingSnapshot& snapshot,std::weak_ptr<Callbacks> callbacks,std::shared_ptr<void> pin)
        :context_(snapshot.context),text_(snapshot.text),callbacks_(std::move(callbacks)),module_pin_(std::move(pin)),flags_(snapshot.updated_flags){if(context_)context_->AddRef();}
    ~ReadingUiElement(){if(context_)context_->Release();}
    static HRESULT alloc(const wchar_t* value,BSTR* out){if(!out)return E_POINTER;*out=SysAllocString(value);return *out?S_OK:E_OUTOFMEMORY;}
    static HRESULT alloc(const std::wstring& value,BSTR* out){if(!out)return E_POINTER;*out=SysAllocStringLen(value.data(),static_cast<UINT>(value.size()));return *out?S_OK:E_OUTOFMEMORY;}
    static constexpr GUID guid_{0x8a38b065,0xf809,0x4bf4,{0x9a,0xf3,0x17,0x54,0xd9,0x4c,0x43,0x10}};
    std::atomic<ULONG> references_{1};mutable std::mutex mutex_;ITfContext* context_=nullptr;std::wstring text_;std::weak_ptr<Callbacks> callbacks_;std::shared_ptr<void> module_pin_;DWORD flags_=0x1f;bool ended_=false,requested_=true,explicit_show_=false;unsigned long long generation_=0;
};

class ReadingUiController final : public std::enable_shared_from_this<ReadingUiController> {
public:
    static std::shared_ptr<ReadingUiController> Create(ITfUIElementMgr* manager,std::shared_ptr<void> pin){if(!manager)return {};return std::shared_ptr<ReadingUiController>(new ReadingUiController(manager,std::move(pin)));}
    ~ReadingUiController(){ReadingUiElement* element=nullptr;DWORD id=invalid_id;{std::lock_guard<std::mutex> lock(mutex_);element=element_;element_=nullptr;if(state_==State::Active||state_==State::EndFailed)id=id_;state_=State::Idle;id_=invalid_id;}if(element)element->end_snapshot();if(id!=invalid_id&&manager_)manager_->EndUIElement(id);if(element)element->Release();if(manager_)manager_->Release();}
    HRESULT begin(const ReadingSnapshot& snapshot,std::weak_ptr<Callbacks> callbacks) noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;auto* fresh=ReadingUiElement::Create(snapshot,std::move(callbacks),pin_);if(!fresh)return E_OUTOFMEMORY;
        bool busy=false;
        {std::lock_guard<std::mutex> lock(mutex_);if(state_!=State::Idle)busy=true;else{element_=fresh;state_=State::Beginning;pending_end_=false;id_=invalid_id;}}
        if(busy){fresh->Release();return E_PENDING;}
        BOOL show=TRUE;DWORD id=invalid_id;HRESULT hr=manager_->BeginUIElement(static_cast<ITfUIElement*>(fresh),&show,&id);
        bool deferred=false;
        {std::lock_guard<std::mutex> lock(mutex_);if(hr!=S_OK){state_=State::Idle;element_=nullptr;id_=invalid_id;pending_end_=false;}else{id_=id;state_=State::Active;deferred=pending_end_;pending_end_=false;fresh->apply_begin_show(show);}}
        if(hr!=S_OK){fresh->end_snapshot();fresh->Release();return hr;}
        if(deferred)return end();
        return update(snapshot);
    }
    HRESULT update(const ReadingSnapshot& snapshot) noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;ReadingUiElement* element=nullptr;DWORD id=invalid_id;
        {std::lock_guard<std::mutex> lock(mutex_);if(state_!=State::Active||!element_)return E_PENDING;element=element_;element->AddRef();id=id_;}
        HRESULT hr=element->replace(snapshot);if(SUCCEEDED(hr)){bool still_active=false;{std::lock_guard<std::mutex> lock(mutex_);still_active=state_==State::Active&&id_==id&&element_==element;}hr=still_active?manager_->UpdateUIElement(id):S_FALSE;}element->Release();return hr;
    }
    HRESULT end() noexcept {
        auto self=weak_from_this().lock();if(!self)return E_UNEXPECTED;ReadingUiElement* element=nullptr;DWORD id=invalid_id;bool deferred=false;
        {std::lock_guard<std::mutex> lock(mutex_);if(state_==State::Idle)return S_FALSE;if(state_==State::Beginning){pending_end_=true;deferred=true;}else if(state_==State::Ending)return S_OK;else{state_=State::Ending;id=id_;}element=element_;if(element)element->AddRef();}
        if(element)element->end_snapshot();if(deferred){if(element)element->Release();return S_OK;}HRESULT hr=manager_->EndUIElement(id);
        {std::lock_guard<std::mutex> lock(mutex_);if(hr==S_OK){element_=nullptr;id_=invalid_id;state_=State::Idle;}else state_=State::EndFailed;}
        if(hr==S_OK&&element)element->Release();if(element)element->Release();return hr;
    }
    bool suppressed() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return state_==State::Active&&element_&&!element_->native_requested();}
    DWORD id()const noexcept{std::lock_guard<std::mutex> lock(mutex_);return state_==State::Active||state_==State::Ending||state_==State::EndFailed?id_:invalid_id;}
private:
    ReadingUiController(ITfUIElementMgr* manager,std::shared_ptr<void> pin):manager_(manager),pin_(std::move(pin)){manager_->AddRef();}
    static constexpr DWORD invalid_id=0xffffffffu;enum class State{Idle,Beginning,Active,Ending,EndFailed};mutable std::mutex mutex_;ITfUIElementMgr* manager_=nullptr;std::shared_ptr<void> pin_;ReadingUiElement* element_=nullptr;DWORD id_=invalid_id;State state_=State::Idle;bool pending_end_=false;
};
}
#endif
