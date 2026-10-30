#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include "../tsf/activation_diagnostics.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

class ContextRead final : public ITfEditSession {
    std::atomic<ULONG> refs{1}; ITfContext* context_;
public:
    std::wstring value;
    explicit ContextRead(ITfContext* context):context_(context){context_->AddRef();}
    ~ContextRead(){context_->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown)&&!IsEqualIID(iid,IID_ITfEditSession))return E_NOINTERFACE;
        *out=static_cast<ITfEditSession*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        ITfRange *start=nullptr,*end=nullptr;HRESULT hr=context_->GetStart(cookie,&start);
        if(SUCCEEDED(hr))hr=context_->GetEnd(cookie,&end);
        if(SUCCEEDED(hr))hr=start->ShiftEndToRange(cookie,end,TF_ANCHOR_END);
        if(SUCCEEDED(hr)){wchar_t buffer[2048]{};ULONG count=0;hr=start->GetText(cookie,0,buffer,2048,&count);if(SUCCEEDED(hr))value.assign(buffer,buffer+count);}
        if(end)end->Release();if(start)start->Release();return hr;
    }
};
static HRESULT read_context(ITfContext* context,TfClientId client,std::wstring* value) {
    auto* read=new ContextRead(context);HRESULT session=E_FAIL;
    const HRESULT hr=context->RequestEditSession(client,read,TF_ES_SYNC|TF_ES_READ,&session);
    if(SUCCEEDED(hr)&&SUCCEEDED(session))*value=read->value;
    read->Release();return FAILED(hr)?hr:session;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    const auto sandbox=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto dll=std::filesystem::absolute(argv[2]).lexically_normal();
    wchar_t executable[32768]{};const DWORD length=GetModuleFileNameW(nullptr,executable,32768);
    CHECK(length>0&&length<32768);
    CHECK(_wcsicmp(std::filesystem::path(executable).parent_path().lexically_normal().c_str(),sandbox.c_str())==0);
    CHECK(sandbox.filename().wstring().find(L"tsf-binary-fixture-")==0);
    CHECK(_wcsicmp(dll.filename().c_str(),L"BoshiamyTsf.dll")==0);
    CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA",sandbox.c_str()));
    const auto settingsDir=sandbox/L"LiuKai";
    const auto settings=settingsDir/L"settings.conf";
    {
        CHECK(std::filesystem::create_directory(settingsDir));
        HANDLE file=CreateFileW(settings.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        CHECK(file!=INVALID_HANDLE_VALUE);
        const char config[]="version=1\ndefault_english=0\ndefault_language=Traditional\ninherit_system=0\n"
            "shift_toggle=1\nshow_extensions=1\nshift_uppercase=0\nshift_lower_symbols=0\n"
            "reverse_mode=Recommended\nspeed_hint_only=0\nrelated_enter_space=0\n";
        DWORD written=0;BOOL wrote=WriteFile(file,config,sizeof(config)-1,&written,nullptr);CloseHandle(file);
        CHECK(wrote&&written==sizeof(config)-1);
    }
    struct Cleanup {std::filesystem::path file,dir;~Cleanup(){DeleteFileW(file.c_str());RemoveDirectoryW(dir.c_str());}} cleanup{settings,settingsDir};
    CHECK(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)==S_OK);
    HMODULE module=LoadLibraryW(dll.c_str());CHECK(module);
    using GetClass=HRESULT (WINAPI*)(REFCLSID,REFIID,void**);
    using CanUnload=HRESULT (WINAPI*)();
    auto getClass=reinterpret_cast<GetClass>(GetProcAddress(module,"DllGetClassObject"));
    auto canUnload=reinterpret_cast<CanUnload>(GetProcAddress(module,"DllCanUnloadNow"));
    CHECK(getClass&&canUnload);
    const CLSID service={0x5d7c4291,0x2b34,0x4ce8,{0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
    IClassFactory* factory=nullptr;CHECK(getClass(service,IID_IClassFactory,reinterpret_cast<void**>(&factory))==S_OK);
    ITfTextInputProcessorEx* tip=nullptr;
    CHECK(factory->CreateInstance(nullptr,boshiamy::tsf_activation::kTextInputProcessorExIid,reinterpret_cast<void**>(&tip))==S_OK);
    factory->Release();
    ITfKeyEventSink* keys=nullptr;CHECK(tip->QueryInterface(IID_ITfKeyEventSink,reinterpret_cast<void**>(&keys))==S_OK);
    ITfThreadMgr* manager=nullptr;
    CHECK(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,reinterpret_cast<void**>(&manager))==S_OK);
    ITfThreadMgrEx* extended=nullptr;CHECK(manager->QueryInterface(IID_ITfThreadMgrEx,reinterpret_cast<void**>(&extended))==S_OK);
    TfClientId client=0;CHECK(extended->ActivateEx(&client,TF_TMAE_NOACTIVATETIP|TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);extended->Release();
    ITfDocumentMgr* document=nullptr;ITfContext* context=nullptr;TfEditCookie cookie=0;
    CHECK(manager->CreateDocumentMgr(&document)==S_OK);
    CHECK(document->CreateContext(client,0,nullptr,&context,&cookie)==S_OK);
    CHECK(document->Push(context)==S_OK);
    CHECK(tip->ActivateEx(manager,client,TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);
    auto press=[&](WPARAM key){BOOL eaten=FALSE;HRESULT hr=keys->OnKeyDown(context,key,0,&eaten);return hr==S_OK&&eaten;};
    CHECK(press('A')&&press(VK_SPACE));
    std::wstring text;CHECK(read_context(context,client,&text)==S_OK&&text==L"\u5c0d");
    CHECK(press('O')&&press('E')&&press('K')&&press(VK_SPACE));
    CHECK(read_context(context,client,&text)==S_OK&&text==L"\u5c0d\u5638");
    std::wstring english;
    for(int i=0;i<600;++i){const auto c=static_cast<wchar_t>(L'a'+i%26);english.push_back(c);CHECK(press(static_cast<WPARAM>(L'A'+i%26)));}
    CHECK(press(VK_BACK));english.pop_back();
    CHECK(press(VK_RETURN));
    CHECK(read_context(context,client,&text)==S_OK&&text==L"\u5c0d\u5638"+english);
    CHECK(tip->Deactivate()==S_OK);keys->Release();tip->Release();
    CHECK(document->Pop(TF_POPF_ALL)==S_OK);context->Release();document->Release();
    CHECK(manager->Deactivate()==S_OK);manager->Release();CoUninitialize();
    CHECK(canUnload()==S_OK);CHECK(FreeLibrary(module));
    std::cout << "packaged TSF DLL committed two Chinese characters and 599 English characters in UI-less fixture; no TIP was registered\n";
    return 0;
}
