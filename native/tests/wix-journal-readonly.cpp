#include "../installer/durable_journal.hpp"
#include <iostream>
namespace j=boshiamy::installer::durable_journal;
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    std::filesystem::path path(argv[1]);std::string image;
    if(!j::detail::read_file(path,&image)){std::cerr<<"read failed\n";return 1;}
    size_t pos=0;j::Identity id;j::detail::Digest chain{};
    if(!j::detail::parse_header(image,&pos,&id,&chain)){std::cerr<<"header invalid\n";return 1;}
    j::Store store(path,id);j::Document doc;HRESULT hr=store.read(id,&doc);
    std::cout<<"result="<<static_cast<unsigned long>(hr)<<" committed="<<doc.committed<<" success="<<doc.all_operations_succeeded<<" events="<<doc.events.size()<<"\n";
    if(hr!=S_OK)return 1;
    for(const auto& e:doc.events){
        if(e.kind==j::Event::Kind::intent)std::cout<<"intent "<<e.intent.sequence<<" "<<e.intent.resource<<" "<<e.intent.action<<"\n";
        else if(e.kind==j::Event::Kind::observation)std::cout<<"observation "<<e.observation.sequence<<" status="<<static_cast<unsigned long>(e.observation.operation_status)<<"\n";
        else std::cout<<"commit\n";
    }
    return 0;
}
