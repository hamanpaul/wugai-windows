#include "../wix/user-context.cpp"
#include <iostream>
int main() {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return 1;
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
    std::vector<BYTE> bytes(size);
    if(!size||!GetTokenInformation(token,TokenUser,bytes.data(),size,&size)){CloseHandle(token);return 1;}
    CloseHandle(token);
    auto current=reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid;
    BYTE systemSid[SECURITY_MAX_SID_SIZE];DWORD systemSize=sizeof(systemSid);
    if(!CreateWellKnownSid(WinLocalSystemSid,nullptr,systemSid,&systemSize))return 1;
    if(!desktop_session_matches(current)||desktop_session_matches(systemSid))return 1;
    std::cout<<"desktop_session_identity_test=passed\n";
}
