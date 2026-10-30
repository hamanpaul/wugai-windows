#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include "../tsf/service.cpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

class ContextRead final : public ITfEditSession {
    std::atomic<ULONG> refs{1}; ITfContext* context_;
public:
    std::wstring value;
    explicit ContextRead(ITfContext* context) : context_(context) { context_->AddRef(); }
    ~ContextRead() { context_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown)&&!IsEqualIID(iid,IID_ITfEditSession))return E_NOINTERFACE;
        *out=static_cast<ITfEditSession*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        ITfRange *start=nullptr,*end=nullptr;
        HRESULT hr=context_->GetStart(cookie,&start);
        if(SUCCEEDED(hr))hr=context_->GetEnd(cookie,&end);
        if(SUCCEEDED(hr))hr=start->ShiftEndToRange(cookie,end,TF_ANCHOR_END);
        if(SUCCEEDED(hr)) {wchar_t buffer[64]{};ULONG count=0;hr=start->GetText(cookie,0,buffer,64,&count);if(SUCCEEDED(hr))value.assign(buffer,buffer+count);}
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
    if(argc!=2 && argc!=3)return 2;
    const auto directory=std::filesystem::absolute(argv[1]).lexically_normal();
    wchar_t executable[32768]{};const DWORD length=GetModuleFileNameW(nullptr,executable,32768);
    CHECK(length>0 && length<32768);
    const auto executableDirectory=std::filesystem::path(executable).parent_path().lexically_normal();
    CHECK(_wcsicmp(executableDirectory.c_str(),directory.c_str())==0);
    CHECK(directory.filename().wstring().find(L"tsf-uiless-fixture-")==0);
    const auto dictionary=directory/L"dictionary.tsv";
    if(argc==3) {
        // The supplied product dictionary is read-only; never overwrite a
        // pre-existing test file or modify the original release payload.
        CHECK(CopyFileW(argv[2],dictionary.c_str(),TRUE));
    } else {
        HANDLE file=CreateFileW(dictionary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        CHECK(file!=INVALID_HANDLE_VALUE);
        const char data[]="section\tcode\ttext\tfreq\torder\n"
            "TRADITIONAL\ta\t\xe5\xb0\x8d\t100\t0\n"
            "TRADITIONAL\toek\t\xe5\x98\xb8\t100\t1\n";
        DWORD written=0;BOOL wrote=WriteFile(file,data,sizeof(data)-1,&written,nullptr);CloseHandle(file);
        CHECK(wrote && written==sizeof(data)-1);
    }
    struct Cleanup {std::filesystem::path path;~Cleanup(){DeleteFileW(path.c_str());}} cleanup{dictionary};
    CHECK(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)==S_OK);
    ITfThreadMgr* manager=nullptr;
    CHECK(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,
        reinterpret_cast<void**>(&manager))==S_OK);
    ITfThreadMgrEx* extended=nullptr;CHECK(manager->QueryInterface(IID_ITfThreadMgrEx,reinterpret_cast<void**>(&extended))==S_OK);
    TfClientId client=0;
    CHECK(extended->ActivateEx(&client,TF_TMAE_NOACTIVATETIP|TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);
    extended->Release();
    ITfDocumentMgr* document=nullptr;ITfContext* context=nullptr;TfEditCookie cookie=0;
    CHECK(manager->CreateDocumentMgr(&document)==S_OK);
    CHECK(document->CreateContext(client,0,nullptr,&context,&cookie)==S_OK);
    CHECK(document->Push(context)==S_OK);
    auto* service=new Service(launch_tool_process,{});
    CHECK(service->ActivateEx(manager,client,TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);
    CHECK(service->menu_command(boshiamy::MenuCommand::Chinese)==S_OK);
    CHECK(service->menu_command(boshiamy::MenuCommand::Traditional)==S_OK);
    auto press=[&](WPARAM key) {
        BOOL eaten=FALSE;
        HRESULT hr=service->key_down_for_state(context,key,false,false,false,&eaten);
        return hr==S_OK && eaten;
    };
    CHECK(press('A') && press(VK_SPACE));
    std::wstring text;CHECK(read_context(context,client,&text)==S_OK && text==L"\u5c0d");
    CHECK(press('O') && press('E') && press('K') && press(VK_SPACE));
    CHECK(read_context(context,client,&text)==S_OK && text==L"\u5c0d\u5638");
    CHECK(service->Deactivate()==S_OK);service->Release();
    CHECK(document->Pop(TF_POPF_ALL)==S_OK);context->Release();document->Release();
    CHECK(manager->Deactivate()==S_OK);manager->Release();CoUninitialize();
    CHECK(DllCanUnloadNow()==S_OK);
    std::cout << "UI-less TSF fixture committed two Chinese characters using " << (argc==3?"supplied":"synthetic")
              << " dictionary; no TIP was registered\n";
    return 0;
}
