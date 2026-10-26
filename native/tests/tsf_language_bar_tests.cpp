#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <atomic>
#include <stdexcept>
#include "../tsf/service.cpp"
#define REQUIRE(x) do{if(!(x)){std::cerr<<#x<<" failed at line "<<__LINE__<<"\n";return 1;}}while(0)
class BarRead final: public ITfEditSession {
 std::atomic<ULONG> refs{1}; ITfContext* ctx;
public:
 std::wstring text;
 explicit BarRead(ITfContext* c):ctx(c){ctx->AddRef();}
 ~BarRead(){ctx->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_ITfEditSession))*out=this;else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override {ITfRange *a=nullptr,*b=nullptr;HRESULT h=ctx->GetStart(ec,&a);if(FAILED(h))return h;h=ctx->GetEnd(ec,&b);if(SUCCEEDED(h))h=a->ShiftEndToRange(ec,b,TF_ANCHOR_END);if(SUCCEEDED(h)){wchar_t buf[256]{};ULONG n=0;h=a->GetText(ec,0,buf,256,&n);if(SUCCEEDED(h))text.assign(buf,buf+n);}if(b)b->Release();a->Release();return h;}
};
static std::wstring text(ITfContext* ctx,TfClientId id){auto* r=new BarRead(ctx);HRESULT session=E_FAIL;HRESULT h=ctx->RequestEditSession(id,r,TF_ES_SYNC|TF_ES_READ,&session);if(FAILED(h)||FAILED(session)){r->Release();throw std::runtime_error("read context");}auto out=r->text;r->Release();return out;}
// Independent SDK contract oracle: INPUTMODE_CONVERSION and the
// documented NATIVE/FULLSHAPE bits. Keep this separate from product constants.
static const GUID kSystemConversionMode={0xccf05dd8,0x4a87,0x11d7,{0xa6,0xe2,0x00,0x06,0x5b,0x84,0x43,0x5c}};
static HRESULT system_compartment(ITfThreadMgr* mgr,TfClientId client,REFGUID guid,LONG* value,bool write){
 ITfCompartmentMgr* manager=nullptr;HRESULT hr=mgr->QueryInterface(IID_ITfCompartmentMgr,reinterpret_cast<void**>(&manager));if(FAILED(hr))return hr;
 ITfCompartment* compartment=nullptr;hr=manager->GetCompartment(guid,&compartment);manager->Release();if(FAILED(hr))return hr;
 VARIANT data;VariantInit(&data);if(write){data.vt=VT_I4;data.lVal=*value;hr=compartment->SetValue(client,&data);}else{hr=compartment->GetValue(&data);if(hr==S_OK){if(data.vt==VT_I4)*value=data.lVal;else hr=DISP_E_TYPEMISMATCH;}}VariantClear(&data);compartment->Release();return hr;
}
static HRESULT write_conversion(ITfThreadMgr* mgr,TfClientId client,LONG value){return system_compartment(mgr,client,kSystemConversionMode,&value,true);}
static HRESULT read_conversion(ITfThreadMgr* mgr,TfClientId client,LONG* value){return system_compartment(mgr,client,kSystemConversionMode,value,false);}
static HRESULT read_open_close(ITfThreadMgr* mgr,TfClientId client,LONG* value){return system_compartment(mgr,client,GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,value,false);}
int main(){
 REQUIRE(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)));
 auto path=dll_directory()/L"dictionary.tsv";{std::ofstream f(path,std::ios::binary);f<<"section\tcode\ttext\tfreq\torder\nTRADITIONAL\ta\t\xe7\x94\xb2\t1\t1\nSIMPLIFIED\ta\t\xe4\xb9\x99\t1\t1\n";REQUIRE(f.good());}
 ITfThreadMgr* mgr=nullptr;REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,(void**)&mgr)));TfClientId id=0;ITfThreadMgrEx* isolated=nullptr;REQUIRE(mgr->QueryInterface(IID_ITfThreadMgrEx,reinterpret_cast<void**>(&isolated))==S_OK);REQUIRE(isolated->ActivateEx(&id,TF_TMAE_NOACTIVATETIP)==S_OK);isolated->Release();ITfDocumentMgr* doc=nullptr;REQUIRE(SUCCEEDED(mgr->CreateDocumentMgr(&doc)));ITfContext* ctx=nullptr;TfEditCookie ec=0;REQUIRE(SUCCEEDED(doc->CreateContext(id,0,nullptr,&ctx,&ec)));REQUIRE(SUCCEEDED(doc->Push(ctx)));
 REQUIRE(write_conversion(mgr,id,0x101)==S_OK);
 auto* service=new Service();REQUIRE(SUCCEEDED(service->Activate(mgr,id)));auto* bar=service->language_bar_item();REQUIRE(bar);bar->AddRef();
 TF_LANGBARITEMINFO info{};REQUIRE(SUCCEEDED(bar->GetInfo(&info)));REQUIRE(IsEqualCLSID(info.clsidService,CLSID_BoshiamyTsf));
 HRESULT registration=service->language_bar_result();std::cout<<"language bar AddItem status: 0x"<<std::hex<<(unsigned long)registration<<std::dec<<"\n";
 if(SUCCEEDED(registration)){ITfLangBarItemMgr* manager=nullptr;REQUIRE(SUCCEEDED(mgr->QueryInterface(IID_ITfLangBarItemMgr,(void**)&manager)));ITfLangBarItem* registered=nullptr;REQUIRE(SUCCEEDED(manager->GetItem(info.guidItem,&registered)));IUnknown *a=nullptr,*b=nullptr;registered->QueryInterface(IID_IUnknown,(void**)&a);bar->QueryInterface(IID_IUnknown,(void**)&b);REQUIRE(a==b);a->Release();b->Release();registered->Release();manager->Release();}
 HICON icon=nullptr;REQUIRE(SUCCEEDED(bar->GetIcon(&icon))&&icon);DestroyIcon(icon);
 using boshiamy::MenuCommand;
 // The system conversion compartment is the independent source of truth for
 // native Chinese mode and full shape. The unrelated 0x100 bit must survive.
 REQUIRE(!bar->display_state().english);
 LONG system_flags=0,open_close=1;
 REQUIRE(read_conversion(mgr,id,&system_flags)==S_OK&&system_flags==0x101);
 REQUIRE(read_open_close(mgr,id,&open_close)==S_OK&&open_close==1);
 REQUIRE(bar->OnClick(TF_LBI_CLK_LEFT,POINT{},nullptr)==S_OK);REQUIRE(bar->display_state().english);
 REQUIRE(read_conversion(mgr,id,&system_flags)==S_OK&&system_flags==0x100);
 BSTR mode_text=nullptr;REQUIRE(bar->GetText(&mode_text)==S_OK);REQUIRE(std::wstring(mode_text)==L"英");SysFreeString(mode_text);
 REQUIRE(bar->OnClick(TF_LBI_CLK_LEFT,POINT{},nullptr)==S_OK);REQUIRE(!bar->display_state().english);
 REQUIRE(read_conversion(mgr,id,&system_flags)==S_OK&&system_flags==0x101);
 REQUIRE(read_open_close(mgr,id,&open_close)==S_OK&&open_close==1);
 REQUIRE(bar->GetText(&mode_text)==S_OK);REQUIRE(std::wstring(mode_text)==L"中");SysFreeString(mode_text);
 // External conversion changes must update both visible modes and OpenClose.
 REQUIRE(write_conversion(mgr,id,0x109)==S_OK);
 REQUIRE(!bar->display_state().english&&bar->display_state().fullwidth);
 REQUIRE(read_open_close(mgr,id,&open_close)==S_OK&&open_close==1);
 REQUIRE(write_conversion(mgr,id,0x100)==S_OK);
 REQUIRE(bar->display_state().english&&!bar->display_state().fullwidth);
 REQUIRE(read_open_close(mgr,id,&open_close)==S_OK&&open_close==0);
 REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Chinese)==S_OK);
 REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Simplified)==S_OK);REQUIRE(bar->display_state().mode==boshiamy::Mode::Simplified);
 BOOL eaten=FALSE;service->key_down_for_state(ctx,'A',false,false,false,&eaten);REQUIRE(eaten);
 REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Traditional)==E_PENDING);REQUIRE(bar->display_state().mode==boshiamy::Mode::Simplified);
 service->key_down_for_state(ctx,VK_SPACE,false,false,false,&eaten);REQUIRE(eaten&&text(ctx,id)==L"\u4e59");
 REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::English)==S_OK);REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Fullwidth)==S_OK);REQUIRE(bar->display_state().english&&bar->display_state().fullwidth);
 service->key_down_for_state(ctx,'X',false,false,false,&eaten);REQUIRE(eaten&&text(ctx,id)==L"\u4e59\uff58");
 REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Chinese)==S_OK);REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Halfwidth)==S_OK);REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::Traditional)==S_OK);
 service->key_down_for_state(ctx,'A',false,false,false,&eaten);service->key_down_for_state(ctx,VK_SPACE,false,false,false,&eaten);REQUIRE(eaten&&text(ctx,id)==L"\u4e59\uff58\u7532");
 service->key_down_for_state(ctx,VK_SPACE,false,true,false,&eaten);REQUIRE(eaten&&bar->display_state().english);
 service->key_down_for_state(ctx,VK_SPACE,false,true,false,&eaten);REQUIRE(eaten&&!bar->display_state().english);
 REQUIRE(write_conversion(mgr,id,0x101)==S_OK);
 service->key_down_for_state(ctx,'A',false,false,false,&eaten);REQUIRE(eaten);
 // Changing conversion mode during composition follows the existing deferred
 // OpenClose policy: keep the current language until the composition ends.
 REQUIRE(write_conversion(mgr,id,0x100)==S_OK);
 REQUIRE(!bar->display_state().english);
 service->key_down_for_state(ctx,VK_SPACE,false,false,false,&eaten);REQUIRE(eaten);
 service->key_down_for_state(ctx,'Z',false,false,false,&eaten);REQUIRE(!eaten&&bar->display_state().english);
 service->Deactivate();service->Release();REQUIRE(DllCanUnloadNow()==S_FALSE && "retained language bar object must keep the DLL loaded");REQUIRE(bar->OnMenuSelect((UINT)MenuCommand::English)==E_UNEXPECTED);bar->Release();REQUIRE(DllCanUnloadNow()==S_OK);
 doc->Pop(TF_POPF_ALL);ctx->Release();doc->Release();mgr->Deactivate();mgr->Release();std::filesystem::remove(path);CoUninitialize();std::cout<<"TSF language bar menu/context bridge passed (system AddItem separately reported)\n";return 0;
}
