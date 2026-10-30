#include "../tsf/candidate_ui_element.hpp"
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)
using namespace boshiamy::candidate_ui;

// A host that takes responsibility for drawing candidate UI, as in UI-less
// mode. It reads actual COM interfaces and drives selection/finalization.
struct UiLessHost final : ITfUIElementMgr {
    ULONG refs=1;
    ITfUIElement* element=nullptr;
    unsigned begins=0,updates=0,ends=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown)&&!IsEqualIID(iid,IID_ITfUIElementMgr))return E_NOINTERFACE;
        *out=static_cast<ITfUIElementMgr*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {return --refs;}
    HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement* value,BOOL* show,DWORD* id) override {
        if(!value||!show||!id||element)return E_INVALIDARG;
        element=value;element->AddRef();++begins;*show=FALSE;*id=42;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override {
        if(id!=42||!element)return E_INVALIDARG;++updates;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override {
        if(id!=42||!element)return E_INVALIDARG;
        ++ends;element->Release();element=nullptr;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUIElement(DWORD id,ITfUIElement** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(id!=42||!element)return E_INVALIDARG;
        element->AddRef();*out=element;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements**) override {return E_NOTIMPL;}
};

int main() {
    auto callbacks=std::make_shared<Callbacks>();
    Snapshot snapshot;
    snapshot.strings={L"one",L"two",L"three",L"four",L"five"};
    snapshot.page_starts={0,2,4};
    snapshot.selection=2;
    auto* element=CandidateUiElement::Create(snapshot,callbacks,{});
    CHECK(element!=nullptr);
    UINT count=0,selection=0;
    CHECK(element->GetPageIndex(nullptr,0,&count)==S_OK && count==3);
    CHECK(element->GetPageIndex(nullptr,3,&count)==S_OK && count==3);
    UINT pages[3]={99,99,99};
    CHECK(element->GetPageIndex(pages,3,&count)==S_OK && count==3);
    CHECK(pages[0]==0 && pages[1]==2 && pages[2]==4);
    UINT short_buffer[2]={99,99};
    CHECK(element->GetPageIndex(short_buffer,2,&count)==E_INVALIDARG && count==3);
    CHECK(short_buffer[0]==99 && short_buffer[1]==99);
    CHECK(element->GetSelection(&selection)==S_OK && selection==2);
    snapshot.strings.clear();snapshot.page_starts.clear();snapshot.selection=0;
    CHECK(element->replace(snapshot)==S_OK);
    CHECK(element->GetSelection(&selection)==S_FALSE);
    CHECK(element->GetPageIndex(nullptr,0,&count)==S_OK && count==0);
    element->Release();
    UiLessHost host;
    unsigned selected=99,finalized=99;
    callbacks->set_selection=[&](UINT index) {selected=index;return S_OK;};
    callbacks->finalize=[&](UINT index) {finalized=index;return S_OK;};
    snapshot.strings={L"first",L"second"};snapshot.page_starts={0};snapshot.selection=0;
    auto controller=CandidateUiController::Create(&host,{});
    CHECK(controller && controller->begin(snapshot,callbacks)==S_OK);
    CHECK(host.begins==1 && controller->suppressed() && controller->host_notifications());
    CandidateListUiElementBehavior* behavior=nullptr;
    CHECK(host.element && host.element->QueryInterface(iid_candidate_list_ui_element_behavior,
        reinterpret_cast<void**>(&behavior))==S_OK && behavior);
    UINT indices[1]={99};
    CHECK(behavior->GetPageIndex(indices,1,&count)==S_OK && count==1 && indices[0]==0);
    CHECK(behavior->SetSelection(1)==S_OK && selected==1);
    CHECK(behavior->Finalize()==S_OK && finalized==1);
    snapshot.selection=1;
    CHECK(controller->update(snapshot)==S_OK && host.updates==1);
    CHECK(behavior->GetSelection(&selection)==S_OK && selection==1);
    CHECK(controller->end()==S_OK && host.ends==1 && !host.element);
    behavior->Release();controller.reset();
    CHECK(host.refs==1);
    std::cout << "candidate UI page-index, empty-selection and UI-less host contracts passed\n";
    return 0;
}
