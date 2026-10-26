#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "../tsf/service.cpp"
#include "../tsf/compartment_ids.hpp"
#define REQUIRE(x) do{if(!(x)){std::cerr<<#x<<" failed at line "<<__LINE__<<"\n";return 1;}}while(0)
static HRESULT compartment(ITfThreadMgr* mgr,TfClientId client,REFGUID guid,LONG& flags,bool write){
 ITfCompartmentMgr* cm=nullptr;HRESULT hr=mgr->QueryInterface(IID_ITfCompartmentMgr,reinterpret_cast<void**>(&cm));if(FAILED(hr))return hr;
 ITfCompartment* value=nullptr;hr=cm->GetCompartment(guid,&value);cm->Release();if(FAILED(hr))return hr;
 VARIANT v;VariantInit(&v);if(write){v.vt=VT_I4;v.lVal=flags;hr=value->SetValue(client,&v);}else{hr=value->GetValue(&v);if(hr==S_OK){if(v.vt==VT_I4)flags=v.lVal;else hr=DISP_E_TYPEMISMATCH;}}VariantClear(&v);value->Release();return hr;
}
int main(){
 REQUIRE(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)==S_OK);
 auto dictionary=dll_directory()/L"dictionary.tsv",settings=dll_directory()/L"conversion-settings.conf";
 {std::ofstream f(dictionary,std::ios::binary);f<<"section\tcode\ttext\tfreq\torder\nTRADITIONAL\ta\t甲\t1\t1\n";REQUIRE(f.good());}
 boshiamy::Settings configuration;configuration.inherit_system=false;configuration.default_english=true;configuration.save(settings);
 ITfThreadMgr* mgr=nullptr;REQUIRE(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,reinterpret_cast<void**>(&mgr))==S_OK);TfClientId client=0;
 // This test owns the Service under test; do not activate another registered TIP.
 ITfThreadMgrEx* isolated=nullptr;REQUIRE(mgr->QueryInterface(IID_ITfThreadMgrEx,reinterpret_cast<void**>(&isolated))==S_OK);
 HRESULT activation=isolated->ActivateEx(&client,TF_TMAE_NOACTIVATETIP);isolated->Release();REQUIRE(activation==S_OK);
 ITfDocumentMgr* document=nullptr;ITfContext* context=nullptr;TfEditCookie cookie=0;REQUIRE(mgr->CreateDocumentMgr(&document)==S_OK);REQUIRE(document->CreateContext(client,0,nullptr,&context,&cookie)==S_OK);REQUIRE(document->Push(context)==S_OK);
 auto set_conversion=[&](LONG flags){return compartment(mgr,client,boshiamy::kConversionCompartment,flags,true);};
 auto flags=[&](){LONG value=-1;if(compartment(mgr,client,boshiamy::kConversionCompartment,value,false)!=S_OK)throw std::runtime_error("conversion read");return value;};
 REQUIRE(set_conversion(0x401)==S_OK);std::cout<<"conversion.seed="<<std::hex<<flags()<<std::dec<<"\n";auto* service=new Service(launch_tool_process,settings);REQUIRE(service->Activate(mgr,client)==S_OK);auto* bar=service->language_bar_item();REQUIRE(bar&&bar->display_state().english&&!bar->display_state().fullwidth&&flags()==0x400);
 std::cout<<"conversion.activated="<<std::hex<<flags()<<" status="<<service->conversion_result()<<std::dec<<"\n";
 REQUIRE(service->menu_command(boshiamy::MenuCommand::Fullwidth)==S_OK);
 std::cout<<"conversion.fullwidth="<<std::hex<<flags()<<" status="<<service->conversion_result()<<std::dec<<"\n";
 REQUIRE(flags()==0x408&&"English fullwidth changes FULLSHAPE, clears NATIVE, and preserves unrelated 0x400");
 REQUIRE(service->menu_command(boshiamy::MenuCommand::Halfwidth)==S_OK&&flags()==0x400);
 auto press=[&](WPARAM key,bool shift=false){BOOL eaten=FALSE;return service->key_down_for_state(context,key,shift,false,false,&eaten)==S_OK&&eaten;};
 REQUIRE(press(VK_SPACE,true)&&flags()==0x408);REQUIRE(press(VK_SPACE,true)&&flags()==0x400);
 REQUIRE(set_conversion(0x809)==S_OK);REQUIRE(!bar->display_state().english&&bar->display_state().fullwidth&&"external NATIVE and FULLSHAPE bits update both language and shape");
 // The existing fullwidth digit case exercises English literal input.
 REQUIRE(set_conversion(0x808)==S_OK);REQUIRE(bar->display_state().english&&bar->display_state().fullwidth);
 REQUIRE(press('1')&&flags()==0x808);
 REQUIRE(set_conversion(0x801)==S_OK);REQUIRE(!bar->display_state().fullwidth&&!bar->display_state().english);
 REQUIRE(set_conversion(0x800)==S_OK);REQUIRE(bar->display_state().english&&!bar->display_state().fullwidth);
 BOOL eaten=TRUE;REQUIRE(service->test_key_down_for_state(context,'1',0,false,false,false,&eaten)==S_OK&&!eaten);REQUIRE(flags()==0x800);
 REQUIRE(service->menu_command(boshiamy::MenuCommand::Chinese)==S_OK);REQUIRE(press('A'));
 REQUIRE(set_conversion(0x809)==S_OK);REQUIRE(bar->display_state().fullwidth);REQUIRE(press(VK_SPACE));
 REQUIRE(press('A')&&press(VK_SPACE,true)&&!bar->display_state().fullwidth&&flags()==0x801);REQUIRE(press(VK_SPACE));
 bar->AddRef();REQUIRE(service->Deactivate()==S_OK);service->Release();REQUIRE(set_conversion(0x809)==S_OK);REQUIRE(!bar->display_state().fullwidth);bar->Release();
 configuration.inherit_system=true;configuration.save(settings);LONG closed=0;REQUIRE(compartment(mgr,client,GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,closed,true)==S_OK);REQUIRE(set_conversion(0x808)==S_OK);
 auto* inherited=new Service(launch_tool_process,settings);REQUIRE(inherited->Activate(mgr,client)==S_OK);REQUIRE(inherited->language_bar_item()->display_state().english&&inherited->language_bar_item()->display_state().fullwidth&&flags()==0x808);
 REQUIRE(inherited->Deactivate()==S_OK);inherited->Release();document->Pop(TF_POPF_ALL);context->Release();document->Release();mgr->Deactivate();mgr->Release();std::filesystem::remove(dictionary);std::filesystem::remove(settings);CoUninitialize();REQUIRE(DllCanUnloadNow()==S_OK);
 std::cout<<"TSF FULLSHAPE publish, external notifications, flag preservation and inherited shape passed\n";
}
