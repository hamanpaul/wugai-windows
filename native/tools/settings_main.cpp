#include "settings_window.hpp"
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
 int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 if(!argv){MessageBoxW(nullptr,L"無法解析命令列參數。",L"嘸蝦米輸入法 設定",MB_ICONERROR);return 1;}
 struct Arguments {LPWSTR* value;~Arguments(){LocalFree(value);}} arguments{argv};
 std::filesystem::path path;
 try{path=argc>1?std::filesystem::path(argv[1]):boshiamy::settings_file_path();}
 catch(...){MessageBoxW(nullptr,L"找不到本機設定資料夾。",L"嘸蝦米輸入法 設定",MB_ICONERROR);return 1;}
 boshiamy::SettingsWindow window(instance,path,[]{PostQuitMessage(0);});
 if(!window.create()){MessageBoxW(nullptr,L"無法開啟設定視窗。",L"嘸蝦米輸入法 設定",MB_ICONERROR);return 1;}
 window.show(show);MSG message{};int result;
 while((result=GetMessageW(&message,nullptr,0,0))>0){
  if(!window.hwnd()||!IsDialogMessageW(window.hwnd(),&message)){TranslateMessage(&message);DispatchMessageW(&message);}
 }
 return result<0?1:static_cast<int>(message.wParam);
}
