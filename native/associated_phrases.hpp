#ifndef BOSHIAMY_ASSOCIATED_PHRASES_HPP
#define BOSHIAMY_ASSOCIATED_PHRASES_HPP
#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace boshiamy {
// Data-only index. Full phrases and file order are preserved; this class does
// not decide when suggestions appear or whether selection inserts a suffix.
class AssociatedPhrases {
    struct Data {
        std::map<char32_t,std::vector<std::string>> groups;
        std::size_t count=0;
    };
    std::shared_ptr<const Data> data_;
    explicit AssociatedPhrases(std::shared_ptr<const Data> data):data_(std::move(data)){}
    static std::shared_ptr<const Data> empty_data() {
        static const auto value=std::make_shared<const Data>(); return value;
    }
    static void append_utf8(std::string& text,char32_t c) {
        if(c<=0x7f) text.push_back(static_cast<char>(c));
        else if(c<=0x7ff) { text.push_back(static_cast<char>(0xc0|(c>>6))); text.push_back(static_cast<char>(0x80|(c&63))); }
        else if(c<=0xffff) { text.push_back(static_cast<char>(0xe0|(c>>12))); text.push_back(static_cast<char>(0x80|((c>>6)&63))); text.push_back(static_cast<char>(0x80|(c&63))); }
        else { text.push_back(static_cast<char>(0xf0|(c>>18))); text.push_back(static_cast<char>(0x80|((c>>12)&63))); text.push_back(static_cast<char>(0x80|((c>>6)&63))); text.push_back(static_cast<char>(0x80|(c&63))); }
    }
    static bool decode_utf8(const std::string& text,std::size_t& at,char32_t& value) {
        if(at>=text.size())return false;
        const auto byte=[&](std::size_t i){return static_cast<unsigned char>(text[i]);};
        const unsigned char lead=byte(at++);
        if(lead<=0x7f){value=lead;return true;}
        std::size_t continuation=0;
        if(lead>=0xc2&&lead<=0xdf){value=lead&0x1f;continuation=1;}
        else if(lead>=0xe0&&lead<=0xef){value=lead&0x0f;continuation=2;}
        else if(lead>=0xf0&&lead<=0xf4){value=lead&0x07;continuation=3;}
        else return false;
        if(continuation>text.size()-at)return false;
        for(std::size_t i=0;i<continuation;++i){
            const unsigned char next=byte(at++);
            if((next&0xc0)!=0x80)return false;
            if(i==0&&((lead==0xe0&&next<0xa0)||(lead==0xed&&next>0x9f)||
                      (lead==0xf0&&next<0x90)||(lead==0xf4&&next>0x8f)))return false;
            value=(value<<6)|(next&0x3f);
        }
        return true;
    }
    static bool valid_utf8(const std::string& text) {
        std::size_t at=0;char32_t scalar=0;
        while(at<text.size())if(!decode_utf8(text,at,scalar))return false;
        return true;
    }
public:
    AssociatedPhrases():data_(empty_data()){}
    static AssociatedPhrases parse_utf16le(std::istream& input) {
        auto data=std::make_shared<Data>();
        auto fail=[](){throw std::runtime_error("Invalid UTF-16LE associated phrase stream");};
        auto unit=[&](std::uint16_t& value) {
            char low,high;
            if(!input.get(low)) {
                if(input.bad()||!input.eof()) fail();
                return false;
            }
            if(!input.get(high)) fail();
            value=static_cast<unsigned char>(low)|(static_cast<std::uint16_t>(static_cast<unsigned char>(high))<<8);
            return true;
        };
        std::uint16_t u; bool beginning=true,delimited=false;
        std::string phrase; char32_t first=0; std::size_t scalars=0;
        while(unit(u)) {
            if(beginning&&u==0xfeff) { beginning=false; continue; }
            beginning=false;
            char32_t c=u;
            if(u>=0xd800&&u<=0xdbff) {
                std::uint16_t low;
                if(!unit(low)||low<0xdc00||low>0xdfff) fail();
                c=0x10000+((u-0xd800)<<10)+(low-0xdc00);
            } else if(u>=0xdc00&&u<=0xdfff) fail();
            if(c==U';') {
                if(!delimited) { delimited=true; continue; }
                if(scalars<2) fail();
                data->groups[first].push_back(std::move(phrase)); ++data->count;
                phrase.clear(); scalars=0; first=0;
            } else {
                if(!delimited||c<0x20||c==0x7f||c==0xfeff||c==0xfffe) fail();
                if(scalars==0) first=c;
                append_utf8(phrase,c); ++scalars;
            }
        }
        if(scalars) fail(); // A missing final delimiter is a truncated file.
        if(data->count==0) return AssociatedPhrases();
        return AssociatedPhrases(std::move(data));
    }
    const std::vector<std::string>& starting_with(char32_t first) const {
        static const std::vector<std::string> none;
        auto found=data_->groups.find(first);
        return found==data_->groups.end()?none:found->second;
    }
    std::vector<std::string> suffixes_for_prefix(const std::string& prefix) const {
        std::vector<std::string> result;
        if(prefix.empty()||!valid_utf8(prefix))return result;
        std::size_t at=0;char32_t first=0;
        if(!decode_utf8(prefix,at,first))return result;
        for(const auto& phrase:starting_with(first))
            if(phrase.size()>prefix.size()&&phrase.compare(0,prefix.size(),prefix)==0)
                result.push_back(phrase.substr(prefix.size()));
        return result;
    }
    std::size_t size()const {return data_->count;}
    std::size_t group_count()const {return data_->groups.size();}
    bool empty()const {return size()==0;}
};
}
#endif
