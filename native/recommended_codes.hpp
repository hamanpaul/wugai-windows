#ifndef BOSHIAMY_RECOMMENDED_CODES_HPP
#define BOSHIAMY_RECOMMENDED_CODES_HPP
#include "modes.hpp"
#include <istream>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
namespace boshiamy {
class RecommendedCodes {
    using Key = std::pair<Mode, std::string>;
    struct Data { std::map<Key, std::string> codes; };
    std::shared_ptr<const Data> data_;
    explicit RecommendedCodes(std::shared_ptr<const Data> data):data_(std::move(data)){}
    static bool one_scalar(const std::string& s) {
        if (s.empty()) return false;
        unsigned char a=static_cast<unsigned char>(s[0]);
        std::size_t n=a<=0x7f?1:a>=0xc2&&a<=0xdf?2:a>=0xe0&&a<=0xef?3:a>=0xf0&&a<=0xf4?4:0;
        if(!n||s.size()!=n)return false;
        for(std::size_t j=1;j<n;++j){unsigned char b=static_cast<unsigned char>(s[j]);if(b<0x80||b>0xbf)return false;if(j==1&&((a==0xe0&&b<0xa0)||(a==0xed&&b>0x9f)||(a==0xf0&&b<0x90)||(a==0xf4&&b>0x8f)))return false;}
        return true;
    }
    static bool has_nul(const std::string& s){return s.find('\0')!=std::string::npos;}
    static bool valid_section(const std::string& s, Mode& mode){
        if(s=="TRADITIONAL"){mode=Mode::Traditional;return true;}
        if(s=="SIMPLIFIED"){mode=Mode::Simplified;return true;}
        if(s=="TW_SIMPLIFIED"){mode=Mode::TradToSimplified;return true;}
        if(s=="JAPANESE"){mode=Mode::Japanese;return true;}return false;
    }
    static bool valid_code(std::string& code){
        if(code.empty()||code.size()>5||has_nul(code))return false;
        for(char& c:code){if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');}
        auto root=[](char c){return(c>='a'&&c<='z')||c==','||c=='.'||c=='\''||c=='['||c==']';};
        auto aux=[](char c){return (c>='0'&&c<='9')||c==';'||c=='/'||c=='-';};
        if(!root(code[0]))return false;
        for(std::size_t i=1;i<code.size();++i)if(!root(code[i])&&!aux(code[i]))return false;
        return true;
    }
public:
    RecommendedCodes():data_(empty_data()){}
    static std::shared_ptr<const Data> empty_data(){static const auto value=std::make_shared<const Data>();return value;}
    static RecommendedCodes parse(std::istream& input){
        auto data=std::make_shared<Data>();std::string line;bool first=true;
        while(std::getline(input,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(first){first=false;if(line.size()>=3&&static_cast<unsigned char>(line[0])==0xef&&static_cast<unsigned char>(line[1])==0xbb&&static_cast<unsigned char>(line[2])==0xbf)line.erase(0,3);}
            std::size_t start=line.find_first_not_of(" \t");if(start==std::string::npos||line[start]=='#')continue;
            const auto a=line.find('\t'),b=a==std::string::npos?a:line.find('\t',a+1);
            if(a==std::string::npos||b==std::string::npos||line.find('\t',b+1)!=std::string::npos)continue;
            const std::string section=line.substr(0,a),text=line.substr(a+1,b-a-1);std::string code=line.substr(b+1);
            if(section=="section"&&text=="text"&&code=="code")continue;
            Mode mode; if(!valid_section(section,mode)||has_nul(section)||has_nul(text)||has_nul(code)||!one_scalar(text)||!valid_code(code))continue;
            data->codes[{mode,text}]=std::move(code);
        }
        if(input.bad()||(input.fail()&&!input.eof()))throw std::runtime_error("Failed to read recommended codes stream");
        if(data->codes.empty()) return RecommendedCodes();
        return RecommendedCodes(std::move(data));
    }
    std::string get(const std::string& text,Mode mode)const{if(mode==Mode::Fast)mode=Mode::Traditional;auto i=data_->codes.find({mode,text});return i==data_->codes.end()?std::string{}:i->second;}
    std::size_t size()const{return data_->codes.size();}
    bool empty()const{return data_->codes.empty();}
};
}
#endif
