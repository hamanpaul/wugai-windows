#include <windows.h>
#include <iostream>
struct Args {const wchar_t* dll;};
DWORD WINAPI worker(void* raw){
 auto* args=static_cast<Args*>(raw);HMODULE module=LoadLibraryW(args->dll);if(!module)return 2;
 auto call=reinterpret_cast<int(*)()>(GetProcAddress(module,"TestScope"));if(!call)return 3;
 if(call()!=1)return 4;if(!FreeLibrary(module))return 5;return 0;
}
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 2;Args args{argv[1]};HANDLE thread=CreateThread(nullptr,0,worker,&args,0,nullptr);if(!thread)return 3;
 DWORD wait=WaitForSingleObject(thread,5000);if(wait!=WAIT_OBJECT_0){std::cerr<<"thread_timeout\n";return 4;}
 DWORD exit=1;GetExitCodeThread(thread,&exit);CloseHandle(thread);std::cout<<"thread_exit="<<exit<<"\n";return static_cast<int>(exit);
}
