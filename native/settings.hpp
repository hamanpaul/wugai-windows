#ifndef BOSHIAMY_SETTINGS_HPP
#define BOSHIAMY_SETTINGS_HPP
#include "engine.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <map>
#include <atomic>
#include <vector>
#include <sstream>
namespace boshiamy {
struct Settings {
 bool default_english=true; Mode default_language=Mode::Traditional; bool inherit_system=true; bool shift_toggle=true; bool show_extensions=true; bool shift_uppercase=false; bool shift_lower_symbols=false;
 ReverseMode reverse_mode=ReverseMode::Recommended; bool speed_hint_only=false; bool related_enter_space=false;
 bool operator==(const Settings&o)const{return default_english==o.default_english&&default_language==o.default_language&&inherit_system==o.inherit_system&&shift_toggle==o.shift_toggle&&show_extensions==o.show_extensions&&shift_uppercase==o.shift_uppercase&&shift_lower_symbols==o.shift_lower_symbols&&reverse_mode==o.reverse_mode&&speed_hint_only==o.speed_hint_only&&related_enter_space==o.related_enter_space;}
 bool operator!=(const Settings&o)const{return !(*this==o);}
 static Settings load(const std::filesystem::path& path) {
  Settings d; std::ifstream f(path,std::ios::binary); if(!f){if(!std::filesystem::exists(path))return d;throw std::runtime_error("Cannot read settings file");}
  std::string s((std::istreambuf_iterator<char>(f)),{});if(f.bad())throw std::runtime_error("Cannot read settings file");std::istringstream in(s);std::map<std::string,std::string> m;std::string line;
  while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto n=line.find('=');if(n==std::string::npos||n==0||n+1==line.size()||!m.emplace(line.substr(0,n),line.substr(n+1)).second)throw std::runtime_error("Invalid settings file");}
  const char* keys[]={"version","default_english","default_language","inherit_system","shift_toggle","show_extensions","shift_uppercase","shift_lower_symbols","reverse_mode","speed_hint_only","related_enter_space"};if(m.size()!=11)throw std::runtime_error("Invalid settings keys");for(auto k:keys)if(!m.count(k))throw std::runtime_error("Missing settings key");if(m["version"]!="1")throw std::runtime_error("Unsupported version");
  auto b=[&](const char*k){if(m[k]!="0"&&m[k]!="1")throw std::runtime_error("Invalid boolean");return m[k]=="1";};d.default_english=b("default_english");auto l=m["default_language"];if(l=="Traditional")d.default_language=Mode::Traditional;else if(l=="Simplified")d.default_language=Mode::Simplified;else if(l=="TradToSimplified")d.default_language=Mode::TradToSimplified;else if(l=="Japanese")d.default_language=Mode::Japanese;else throw std::runtime_error("Invalid language");d.inherit_system=b("inherit_system");d.shift_toggle=b("shift_toggle");d.show_extensions=b("show_extensions");d.shift_uppercase=b("shift_uppercase");d.shift_lower_symbols=b("shift_lower_symbols");auto r=m["reverse_mode"];if(r=="Recommended")d.reverse_mode=ReverseMode::Recommended;else if(r=="Shortest")d.reverse_mode=ReverseMode::Shortest;else if(r=="All")d.reverse_mode=ReverseMode::All;else if(r=="Associated")d.reverse_mode=ReverseMode::Associated;else throw std::runtime_error("Invalid reverse mode");d.speed_hint_only=b("speed_hint_only");d.related_enter_space=b("related_enter_space");return d;
 }
 void save(const std::filesystem::path& path) const {
  const char* mode="";switch(default_language){case Mode::Traditional:mode="Traditional";break;case Mode::Simplified:mode="Simplified";break;case Mode::TradToSimplified:mode="TradToSimplified";break;case Mode::Japanese:mode="Japanese";break;default:throw std::runtime_error("Invalid language");}const char* rev="";switch(reverse_mode){case ReverseMode::Recommended:rev="Recommended";break;case ReverseMode::Shortest:rev="Shortest";break;case ReverseMode::All:rev="All";break;case ReverseMode::Associated:rev="Associated";break;default:throw std::runtime_error("Invalid reverse mode");}
  std::ostringstream out;out<<"version=1\ndefault_english="<<default_english<<"\ndefault_language="<<mode<<"\ninherit_system="<<inherit_system<<"\nshift_toggle="<<shift_toggle<<"\nshow_extensions="<<show_extensions<<"\nshift_uppercase="<<shift_uppercase<<"\nshift_lower_symbols="<<shift_lower_symbols<<"\nreverse_mode="<<rev<<"\nspeed_hint_only="<<speed_hint_only<<"\nrelated_enter_space="<<related_enter_space<<"\n";const std::string bytes=out.str();
  if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());static std::atomic<unsigned long long> sequence{0};std::filesystem::path temp;HANDLE file=INVALID_HANDLE_VALUE;for(unsigned attempt=0;attempt<128&&file==INVALID_HANDLE_VALUE;++attempt){std::wostringstream suffix;suffix<<path.filename().wstring()<<L".tmp."<<GetCurrentProcessId()<<L"."<<GetTickCount64()<<L"."<<sequence.fetch_add(1);temp=path.parent_path()/suffix.str();file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(file==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot create temporary settings file");}if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot reserve temporary settings file");
  bool owned=true;try{size_t offset=0;while(offset<bytes.size()){DWORD chunk=(DWORD)std::min<size_t>(bytes.size()-offset,0x7fffffff);DWORD written=0;if(!WriteFile(file,bytes.data()+offset,chunk,&written,nullptr)||written!=chunk)throw std::runtime_error("Cannot write temporary settings file");offset+=written;}if(!FlushFileBuffers(file))throw std::runtime_error("Cannot flush temporary settings file");if(!CloseHandle(file)){file=INVALID_HANDLE_VALUE;throw std::runtime_error("Cannot close temporary settings file");}file=INVALID_HANDLE_VALUE;if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace settings file");owned=false;}catch(...){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(owned)DeleteFileW(temp.c_str());throw;}
 }
};
inline std::filesystem::path settings_file_path(){DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);if(!n)throw std::runtime_error("LOCALAPPDATA unavailable");std::wstring b(n,L' ');DWORD got=GetEnvironmentVariableW(L"LOCALAPPDATA",b.data(),n);if(!got||got>=n)throw std::runtime_error("LOCALAPPDATA unavailable");b.resize(got);return std::filesystem::path(b)/L"LiuKai"/L"settings.conf";}
}
#endif
