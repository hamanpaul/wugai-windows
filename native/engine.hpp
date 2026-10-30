#ifndef BOSHIAMY_ENGINE_HPP
#define BOSHIAMY_ENGINE_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "extension_filter.hpp"
#include "readings.hpp"
#include "phonetic_candidates.hpp"
#include "observed_primary_homophones.hpp"
#include "zhuyin_keyboard.hpp"
#include "modes.hpp"
#include "recommended_codes.hpp"
#include "associated_phrases.hpp"
#include <limits>
#include <tuple>

namespace boshiamy {
enum class HintKind { Shortest, Recommended, All };
enum class Key { Space, Backspace, Escape, PageUp, PageDown, Enter, Up, Down };
enum class QueryStage { None, HomophoneSeed, Homophones, PhoneticInput, PhoneticCandidates };
struct Candidate { std::string text; std::string reverse_code; };
inline std::string candidate_display_text(const Candidate& candidate) {
    if(candidate.reverse_code.empty())return candidate.text;
    std::string code=candidate.reverse_code;
    for(char& c:code)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return candidate.text+" "+code;
}
struct Outcome {
    std::string commit;
    std::string composition;
    std::vector<Candidate> candidates;
    // Full visible lookup snapshot for stable candidate-panel sizing across pages.
    std::shared_ptr<const std::vector<Candidate>> layout_candidates;
    std::vector<std::size_t> candidate_page_indices;
    std::size_t candidate_selection = 0; // Absolute index in layout_candidates.
    Mode mode = Mode::Traditional;
    std::size_t page = 0;
    QueryStage query = QueryStage::None;
    bool query_preview = false;
    bool related_stage = false;
    std::string hint;
    HintKind hint_kind = HintKind::Shortest;
};

class Engine {
public:
    void add(std::string code, std::vector<std::string> texts, Mode mode = Mode::Traditional) {
        normalize(code);
        if (!dictionaries_.unique())
            dictionaries_ = std::make_shared<Dictionaries>(*dictionaries_);
        auto& dest = (*dictionaries_)[mode][code];
        for (auto& text : texts) dest.push_back({std::move(text)});
        traditional_eligible_texts_.reset();
    }

    Outcome snapshot() const { return outcome_; }
    bool set_candidate_page_indices(const std::vector<std::size_t>& starts) {
        if (related_stage_ || reverse_mode_ == ReverseMode::Associated) return false;
        const auto all = visible_candidates();
        if (starts.empty() || starts.front() != 0) return false;
        for (std::size_t i = 0; i < starts.size(); ++i) {
            if (starts[i] >= all.size() || (i && starts[i] <= starts[i - 1])) return false;
        }
        candidate_page_indices_ = starts;
        page_ = 0;
        candidate_selection_ = 0;
        outcome_.commit.clear();
        refresh();
        return true;
    }
    bool set_candidate_selection_global(std::size_t index) {
        if (related_stage_) return false;
        const auto all = visible_candidates();
        const auto starts = page_starts(all);
        if (index >= all.size() || starts.empty()) return false;
        const auto next = std::upper_bound(starts.begin(), starts.end(), index);
        page_ = static_cast<std::size_t>(next - starts.begin() - 1);
        candidate_selection_ = index;
        outcome_.commit.clear();
        refresh();
        return true;
    }
    bool finalize_candidate_selection_global(std::size_t index, Outcome& selected) {
        if (related_stage_ || !preview_.empty() || !fast_preview_code_.empty() ||
            !query_preview_codes_.empty() || index != candidate_selection_) return false;
        const auto all = visible_candidates();
        const auto starts = page_starts(all);
        if (index >= all.size() || page_ >= starts.size()) return false;
        const auto end = page_ + 1 < starts.size() ? starts[page_ + 1] : all.size();
        if (index < starts[page_] || index >= end) return false;
        outcome_.commit.clear();outcome_.hint.clear();
        const auto local = index - starts[page_];
        if (query_ == QueryStage::HomophoneSeed) preview_selection(local);
        else select(local);
        selected = outcome_;
        return true;
    }
    Outcome commit_query_preview() {
        outcome_.commit.clear();outcome_.hint.clear();
        if(preview_.empty())return outcome_;
        const auto selected=preview_;composition_.clear();clear_query();page_=0;refresh();
        outcome_.commit=selected;query_hint(selected);return outcome_;
    }
    void set_speed_hint_only(bool enabled) { speed_hint_only_ = enabled; }
    void set_reverse_mode(ReverseMode mode) {
        reverse_mode_=mode;
        if(query_==QueryStage::Homophones){annotate_homophones();refresh();}
        if(mode==ReverseMode::Associated&&!candidate_page_indices_.empty()){
            candidate_page_indices_.clear();page_=0;refresh();
        }
        if(mode!=ReverseMode::Associated&&related_stage_){outcome_.commit.clear();outcome_.hint.clear();close_related_stage(false);}
    }
    void set_associated_phrases(AssociatedPhrases phrases) { associated_phrases_=std::move(phrases); }
    void set_related_enter_space(bool enabled) { related_enter_space_=enabled; }
    void cancel_related_on_focus_loss() {
        if(!related_stage_)return;
        outcome_.commit.clear();outcome_.hint.clear();
        close_related_stage(true);
    }
    void set_recommendations(RecommendedCodes data) { recommendations_ = std::move(data); dismiss_hint(); if(query_==QueryStage::Homophones){annotate_homophones();refresh();} }
    void dismiss_hint() { outcome_.hint.clear(); outcome_.commit.clear(); }
    void set_readings(Readings readings) {
        readings_=std::move(readings);composition_.clear();clear_query();close_related_state();page_=0;
        outcome_.commit.clear();outcome_.hint.clear();refresh();
    }
    void set_phonetic_candidates(PhoneticCandidateIndex index) {
        phonetic_candidates_ = std::move(index);
        (void)traditional_eligible_texts();
    }
    void set_observed_primary_homophones(ObservedPrimaryHomophoneIndex index) {
        observed_primary_homophones_ = std::move(index);
    }

    void set_show_extensions(bool show) {
        if (show_extensions_ == show) return;
        show_extensions_ = show;
        outcome_.commit.clear();
        const auto visible = visible_candidates();
        const std::size_t last_page = visible.empty() ? 0 : (visible.size() - 1) / 10;
        page_ = std::min(page_, last_page);
        refresh();
    }

    bool show_extensions() const { return show_extensions_; }

    void set_mode(Mode mode) {
        mode_ = mode; composition_.clear(); clear_query();close_related_state();page_ = 0; outcome_.commit.clear();outcome_.hint.clear(); refresh();
    }

    Outcome key(Key key, bool shift = false) {
        outcome_.commit.clear();
        outcome_.hint.clear();
        if(related_stage_)return related_key(key);
        if(!preview_.empty()) {
            if(key==Key::Backspace){preview_.clear();fast_preview_code_.clear();query_preview_codes_.clear();}
            else if(key==Key::Enter){
                outcome_.commit=raw_input_;
                composition_.clear();clear_query();page_=0;refresh();return outcome_;
            }
            else if(key==Key::Space||key==Key::Escape){
                composition_.clear();clear_query();page_=0;refresh();return outcome_;
            }else return outcome_;
        }
        switch (key) {
        case Key::Enter:
            if(query_==QueryStage::Homophones){
                const auto all=visible_candidates();const auto starts=page_starts(all);
                if(!starts.empty()&&candidate_selection_<all.size()){
                    select(candidate_selection_-starts[page_]);return outcome_;
                }
            }
            // Enter is an English-text commit while input is active. Keep the
            // original keys because query composition may be a Zhuyin display.
            if(!raw_input_.empty()){
                outcome_.commit=raw_input_;
                composition_.clear();clear_query();page_=0;refresh();
            }
            break;
        case Key::Space:
            if(query_==QueryStage::PhoneticInput) open_phonetic(true);
            else if(query_==QueryStage::Homophones)return this->key(Key::PageDown);
            else if(query_==QueryStage::PhoneticCandidates)refresh();
            else if (query_!=QueryStage::None) select(0, true);
            else if (!run_command()&&!open_related_after_seed()) select(0, true);
            break;
        case Key::Backspace:
            if(query_==QueryStage::PhoneticCandidates){
                query_=QueryStage::PhoneticInput;homophones_.clear();
                if(!composition_.empty()&&zhuyin::is_tone(composition_.back())){
                    const auto reading=zhuyin::reading(composition_);
                    const bool neutral=reading&&reading->rfind("˙",0)==0;
                    if(!neutral){composition_.pop_back();if(!raw_input_.empty())raw_input_.pop_back();}
                }
            }
            else if(query_==QueryStage::PhoneticInput&&composition_.empty()){clear_query();}
            else if(query_==QueryStage::Homophones){
                query_=QueryStage::HomophoneSeed;homophones_.clear();
                if(!composition_.empty())composition_.pop_back();
                if(!raw_input_.empty())raw_input_.pop_back();
            }
            else if (!composition_.empty()) { composition_.pop_back();if(!raw_input_.empty())raw_input_.pop_back(); }
            else clear_query();
            if(raw_input_.empty()&&composition_.empty())clear_query();
            page_ = 0; refresh(); break;
        case Key::Escape:
            composition_.clear(); clear_query();page_ = 0;
            refresh(); break;
        case Key::PageUp: {
            const auto all = visible_candidates();
            const auto count = page_count(all);
            if(all.empty())page_=0;
            else page_=page_==0?count-1:page_-1;
            refresh();break;
        }
        case Key::PageDown: {
            const auto all = visible_candidates();
            const auto count = page_count(all);
            if (all.empty()) page_ = 0;
            else page_ = (page_ + 1) % count;
            refresh();
            break;
        }
        case Key::Up:
        case Key::Down: {
            if(query_!=QueryStage::Homophones)break;
            const auto all=visible_candidates();
            if(all.empty())break;
            const auto next=key==Key::Down?(candidate_selection_+1)%all.size():
                (candidate_selection_+all.size()-1)%all.size();
            set_candidate_selection_global(next);
            break;
        }
        }
        (void)shift; // Shift+comma/period are also supplied as page keys by the adapter.
        return outcome_;
    }

    Outcome key(char input, bool shift = false, bool shift_lower_symbols = false) {
        if(related_stage_){outcome_.commit.clear();outcome_.hint.clear();return related_key(input,shift);}
        char raw_character = input;
        if (shift && shift_lower_symbols) {
            switch (input) {
            case ',': input = '<'; break;
            case '.': input = '>'; break;
            case '\'': input = '"'; break;
            case '[': input = '{'; break;
            case ']': input = '}'; break;
            default: break;
            }
            raw_character = input;
            shift = false;
        } else {
            if (shift && input == ',') return key(Key::PageUp);
            if (shift && input == '.') return key(Key::PageDown);
        }
        outcome_.commit.clear();
        outcome_.hint.clear();
        unsigned char u = static_cast<unsigned char>(input);
        char c = static_cast<char>(std::tolower(u));
        if(!preview_.empty()){
            if(is_root(c)){
                auto selected=preview_;composition_.clear();clear_query();page_=0;
                key(input,shift,shift_lower_symbols);outcome_.commit=std::move(selected);return outcome_;
            }
            if(c>='0'&&c<='9'){composition_.clear();clear_query();page_=0;refresh();}
            return outcome_;
        }
        if(query_==QueryStage::PhoneticInput){
            if((zhuyin::is_symbol(c)||zhuyin::is_tone(c))&&composition_.size()<4){
                composition_=zhuyin::append_canonical(std::move(composition_),c);raw_input_.push_back(raw_character);page_=0;
                if(zhuyin::has_tone(composition_))open_phonetic();else refresh();
            }
            return outcome_;
        }
        if(query_==QueryStage::Homophones||query_==QueryStage::PhoneticCandidates){if(c>='0'&&c<='9')select(static_cast<std::size_t>(c-'0'));return outcome_;}
        if(query_==QueryStage::None&&composition_=="'"&&!readings_.empty()&&c==';'){
            query_=QueryStage::PhoneticInput;composition_.clear();raw_input_.push_back(input);page_=0;refresh();return outcome_;
        }
        if(query_==QueryStage::None&&composition_=="'"&&!readings_.empty()&&is_root(c)&&!is_shifted_symbol(c)&&c!='\''){
            query_=QueryStage::HomophoneSeed;composition_.clear();
        }
        if (is_root(c)) {
            // Normal composition can also be committed literally with Enter;
            // dictionary/query code length must not truncate that text.
            if (query_ == QueryStage::None || composition_.size() < 5) { composition_.push_back(c);raw_input_.push_back(raw_character); }
            page_ = 0;
            refresh();
        } else if (c >= '0' && c <= '9') {
            const std::size_t index = static_cast<std::size_t>(c - '0');
            const auto all = lookup(composition_);
            const auto starts = page_starts(all);
            const auto end = page_ + 1 < starts.size() ? starts[page_ + 1] : all.size();
            if (!starts.empty() && page_ < starts.size() && starts[page_] + index < end) select(index);
            else if (page_ > 0) {
                // Official last-page missing slots cancel; page-zero digits
                // retain their separate role as code extensions.
                composition_.clear();clear_query();page_ = 0;refresh();
            }
            else if (!composition_.empty() && (query_ == QueryStage::None || composition_.size() < 5)) {
                composition_.push_back(c);raw_input_.push_back(raw_character);
                page_ = 0;
                refresh();
            }
        } else if (is_custom_aux(c) && !composition_.empty() && (query_ == QueryStage::None || composition_.size() < 5)) {
            composition_.push_back(c);raw_input_.push_back(raw_character);
            page_ = 0;
            refresh();
        }
        return outcome_;
    }

private:
    using Dict = std::map<std::string, std::vector<Candidate>>;
    using Dictionaries = std::map<Mode, Dict>;
    // Tentative TSF states share immutable tables. Only a later add() detaches.
    std::shared_ptr<Dictionaries> dictionaries_ = std::make_shared<Dictionaries>();
    mutable std::shared_ptr<const std::set<std::string>> traditional_eligible_texts_;
    std::string composition_;
    std::string raw_input_;
    AssociatedPhrases associated_phrases_;
    std::vector<Candidate> related_candidates_;
    bool related_enter_space_=false;
    bool related_stage_=false;
    Mode mode_ = Mode::Traditional;
    std::size_t page_ = 0;
    std::vector<std::size_t> candidate_page_indices_;
    std::size_t candidate_selection_ = 0;
    std::string candidate_identity_composition_;
    QueryStage candidate_identity_query_ = QueryStage::None;
    Mode candidate_identity_mode_ = Mode::Traditional;
    bool candidate_identity_show_extensions_ = true;
    bool candidate_identity_related_stage_ = false;
    bool has_candidate_identity_ = false;
    std::vector<std::string> candidate_identity_texts_;
    bool show_extensions_ = true;
    bool speed_hint_only_ = false;
    ReverseMode reverse_mode_ = ReverseMode::Recommended;
    RecommendedCodes recommendations_;
    Outcome outcome_;
    Readings readings_;
    PhoneticCandidateIndex phonetic_candidates_;
    ObservedPrimaryHomophoneIndex observed_primary_homophones_;
    QueryStage query_=QueryStage::None;
    std::string preview_;
    std::string fast_preview_code_;
    std::vector<std::string> query_preview_codes_;
    std::vector<Candidate> homophones_;
    void clear_query(){query_=QueryStage::None;homophones_.clear();preview_.clear();fast_preview_code_.clear();query_preview_codes_.clear();raw_input_.clear();}
    void close_related_state(){related_stage_=false;related_candidates_.clear();}
    void close_related_stage(bool discard_seed){
        close_related_state();page_=0;raw_input_.clear();
        if(discard_seed)composition_.clear();
        else { outcome_.commit=composition_;composition_.clear(); }
        refresh();
    }
    bool open_related_after_seed(){
        if(reverse_mode_!=ReverseMode::Associated||query_!=QueryStage::None)return false;
        const auto all=lookup(composition_);
        const auto selected=page_*10;
        if(selected>=all.size())return false;
        const auto seed=all[selected].text;
        auto suffixes=associated_phrases_.suffixes_for_prefix(seed);
        if(suffixes.empty())return false;
        composition_=seed;raw_input_.clear();related_candidates_.clear();
        related_candidates_.reserve(suffixes.size());
        for(auto& suffix:suffixes)related_candidates_.push_back({std::move(suffix)});
        related_stage_=true;page_=0;refresh();return true;
    }
    Outcome related_key(Key key){
        switch(key){
        case Key::Space:
            if(related_enter_space_)select_related(0);
            else close_related_stage(false);
            break;
        case Key::Enter:
        case Key::Backspace:
        case Key::Escape: close_related_stage(false);break;
        case Key::PageUp:
            page_=page_==0?(related_candidates_.size()-1)/10:page_-1;refresh();break;
        case Key::PageDown:
            page_=(page_+1)%((related_candidates_.size()-1)/10+1);refresh();break;
        default:break;
        }
        return outcome_;
    }
    Outcome related_key(char input,bool shift){
        const auto c=static_cast<char>(std::tolower(static_cast<unsigned char>(input)));
        if(c>='0'&&c<='9'){
            if(shift)select_related(static_cast<std::size_t>(c-'0'));
            else close_related_stage(false);
            return outcome_;
        }
        if(is_root(c)){
            const auto seed=composition_;
            close_related_stage(true);
            auto next=key(input,shift);
            next.commit=seed;outcome_=std::move(next);return outcome_;
        }
        return outcome_;
    }
    void select_related(std::size_t index){
        const auto at=page_*10+index;
        if(at>=related_candidates_.size()){
            close_related_stage(false);return;
        }
        outcome_.commit=composition_+related_candidates_[at].text;
        close_related_stage(true);
    }
    std::string best_code(const std::string& text)const{
        std::string best;
        for(const auto& code:active_dict())for(const auto& candidate:code.second)
            if(candidate.text==text&&(best.empty()||std::make_pair(code.first.size(),code.first)<std::make_pair(best.size(),best)))best=code.first;
        return best;
    }
    void query_hint(const std::string& text){
        outcome_.hint.clear();
        if(reverse_mode_==ReverseMode::Shortest){outcome_.hint=best_code(text);outcome_.hint_kind=HintKind::Shortest;}
        else if(reverse_mode_==ReverseMode::Recommended){
            outcome_.hint_kind=HintKind::Recommended;
            const auto code=recommendations_.get(text,mode_);
            auto found=active_dict().find(code);
            if(!code.empty()&&found!=active_dict().end()&&std::any_of(found->second.begin(),found->second.end(),[&](const Candidate& candidate){return candidate.text==text;}))outcome_.hint=code;
        }else if(reverse_mode_==ReverseMode::All){
            outcome_.hint_kind=HintKind::All;
            std::vector<std::string> codes;
            for(const auto& code:active_dict())if(std::any_of(code.second.begin(),code.second.end(),[&](const Candidate& candidate){return candidate.text==text;}))codes.push_back(code.first);
            std::sort(codes.begin(),codes.end(),[](const std::string& a,const std::string& b){return std::make_pair(a.size(),a)<std::make_pair(b.size(),b);});
            for(const auto& code:codes){if(!outcome_.hint.empty())outcome_.hint+=" / ";outcome_.hint+=code;}
        }
    }
    std::vector<std::string> query_preview_codes(const std::string& text) const {
        if(query_!=QueryStage::HomophoneSeed&&query_!=QueryStage::Homophones&&
           query_!=QueryStage::PhoneticCandidates)return {};
        if(reverse_mode_==ReverseMode::Recommended)
            return outcome_.hint.empty()?std::vector<std::string>{}:std::vector<std::string>{outcome_.hint};
        if(reverse_mode_!=ReverseMode::Shortest&&reverse_mode_!=ReverseMode::All)return {};
        std::vector<std::string> codes;
        std::size_t shortest=std::numeric_limits<std::size_t>::max();
        // active_dict is keyed by original code, so iteration gives unique
        // lexicographic rows. Shortest retains every minimum-length tie.
        for(const auto& row:active_dict()){
            if(!std::any_of(row.second.begin(),row.second.end(),[&](const Candidate& candidate){return candidate.text==text;}))continue;
            if(reverse_mode_==ReverseMode::Shortest){
                if(row.first.size()>shortest)continue;
                if(row.first.size()<shortest){shortest=row.first.size();codes.clear();}
            }
            codes.push_back(row.first);
        }
        return codes;
    }
    void prepare_query(std::vector<std::string> texts,const std::string& excluded={}){
        if (texts.empty()) {
            homophones_.clear();
            return;
        }
        std::map<std::string,std::string> codes;
        for(const auto& code:active_dict())for(const auto& candidate:code.second){
            auto found=codes.find(candidate.text);
            if(found==codes.end()||std::make_pair(code.first.size(),code.first)<std::make_pair(found->second.size(),found->second))codes[candidate.text]=code.first;
        }
        texts.erase(std::remove_if(texts.begin(),texts.end(),[&](const std::string& text){return text==excluded||!codes.count(text);}),texts.end());
        // Data-based provisional ordering; full official ordering is unverified.
        auto rank=[&](const std::string& text){return std::make_tuple(readings_.grade(text).value_or(std::numeric_limits<int>::max()),codes.at(text).size(),codes.at(text),text);};
        std::sort(texts.begin(),texts.end(),[&](const std::string& a,const std::string& b){return rank(a)<rank(b);});
        homophones_.clear();for(auto& text:texts)homophones_.push_back({std::move(text)});
    }
    const std::set<std::string>& traditional_eligible_texts() const {
        if (!traditional_eligible_texts_) {
            auto texts = std::make_shared<std::set<std::string>>();
            const auto found = dictionaries_->find(Mode::Traditional);
            if (found != dictionaries_->end())
                for (const auto& code : found->second)
                    for (const auto& candidate : code.second) texts->insert(candidate.text);
            traditional_eligible_texts_ = std::move(texts);
        }
        return *traditional_eligible_texts_;
    }
    void prepare_ordered_phonetic_query(const std::vector<std::string>& texts) {
        if (texts.empty()) {
            homophones_.clear();
            return;
        }
        const auto& eligible = traditional_eligible_texts();
        std::set<std::string> seen;
        homophones_.clear();
        for (const auto& text : texts)
            if (eligible.count(text) && seen.insert(text).second) homophones_.push_back({text});
    }
    void prepare_indexed_homophone_query(const std::vector<std::string>& texts) {
        const auto& eligible = traditional_eligible_texts();
        homophones_.clear();
        for (const auto& text : texts)
            if (eligible.count(text)) homophones_.push_back({text});
    }
    void open_homophones(const std::string& seed){
        const auto indexed = mode_ == Mode::Traditional
            ? observed_primary_or_h3(observed_primary_homophones_, phonetic_candidates_, seed)
            : std::optional<std::vector<std::string>>{};
        if (indexed) prepare_indexed_homophone_query(*indexed);
        else prepare_query(readings_.primary_matches(seed),seed);
        annotate_homophones();
        query_=QueryStage::Homophones;page_=0;refresh();
    }
    void annotate_homophones(){
        std::map<std::string,std::vector<std::string>> codes;
        for(const auto& candidate:homophones_)codes.emplace(candidate.text,std::vector<std::string>{});
        for(const auto& row:active_dict())for(const auto& candidate:row.second){
            auto found=codes.find(candidate.text);
            if(found!=codes.end())found->second.push_back(row.first);
        }
        for(auto& candidate:homophones_){
            auto& values=codes[candidate.text];
            std::sort(values.begin(),values.end(),[](const std::string& a,const std::string& b){return std::make_pair(a.size(),a)<std::make_pair(b.size(),b);});
            values.erase(std::unique(values.begin(),values.end()),values.end());
            candidate.reverse_code.clear();
            if(values.empty())continue;
            if(reverse_mode_==ReverseMode::All){
                for(const auto& code:values){if(!candidate.reverse_code.empty())candidate.reverse_code+=" / ";candidate.reverse_code+=code;}
            }else if(reverse_mode_==ReverseMode::Recommended){
                const auto recommended=recommendations_.get(candidate.text,mode_);
                candidate.reverse_code=std::find(values.begin(),values.end(),recommended)!=values.end()?recommended:values.front();
            }else candidate.reverse_code=values.front();
        }
    }
    void open_phonetic(bool explicit_confirmation=false){
        const auto reading=zhuyin::reading(composition_);
        const bool neutral=reading&&reading->rfind("˙",0)==0;
        // Tone entry stays editable for neutral readings; explicit Space may
        // confirm the reading while retaining its phonetic candidates.
        const bool confirmed = reading && (!neutral || explicit_confirmation);
        const auto* official = confirmed && mode_ == Mode::Traditional
            ? phonetic_candidates_.lookup(*reading) : nullptr;
        if (official) prepare_ordered_phonetic_query(*official);
        else prepare_query(confirmed ? readings_.matches_reading(*reading) : std::vector<std::string>{});
        query_=homophones_.empty()?QueryStage::PhoneticInput:QueryStage::PhoneticCandidates;page_=0;refresh();
    }
    void preview_selection(std::size_t index){
        auto all=lookup(composition_);const auto starts=page_starts(all);
        const auto at=starts.empty()?all.size():starts[page_]+index;
        const auto end=!starts.empty()&&page_+1<starts.size()?starts[page_+1]:all.size();
        if(!starts.empty()&&at<end){
            preview_=all[at].text;query_hint(preview_);
            query_preview_codes_=query_preview_codes(preview_);
            refresh();
        }
        else {composition_.clear();clear_query();page_=0;refresh();}
    }

    static void normalize(std::string& s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    static bool is_shifted_symbol(char c) { return c == '<' || c == '>' || c == '"' || c == '{' || c == '}'; }
    static bool is_root(char c) { return (c >= 'a' && c <= 'z') || c == ',' || c == '.' || c == '\'' || c == '[' || c == ']' || is_shifted_symbol(c); }
    static bool is_custom_aux(char c) { return c == ';' || c == '/' || c == '-'; }
    bool run_command() {
        Mode target;
        if (composition_ == ",,t") target = Mode::Traditional;
        else if (composition_ == ",,c") target = Mode::Simplified;
        else if (composition_ == ",,ct") target = Mode::TradToSimplified;
        else if (composition_ == ",,j") target = Mode::Japanese;
        else if (composition_ == ",,sp") target = Mode::Fast;
        else return false;
        mode_ = target; composition_.clear();raw_input_.clear(); page_ = 0; refresh();
        return true;
    }
    const Dict& active_dict() const {
        auto key = mode_ == Mode::Fast ? Mode::Traditional : mode_;
        auto it = dictionaries_->find(key);
        if (it == dictionaries_->end()) { static const Dict empty; return empty; }
        return it->second;
    }
    std::vector<Candidate> lookup(const std::string& code) const {
        std::vector<Candidate> result;
        if(query_==QueryStage::PhoneticInput)return result;
        if(query_==QueryStage::Homophones||query_==QueryStage::PhoneticCandidates){result=homophones_;filter_extensions(result);return result;}
        auto it = active_dict().find(code);
        if (it != active_dict().end()) {
            result = it->second;
            filter_extensions(result);
            return result;
        }
        if (code.size() > 1) {
            const std::string assists = "vrsfwlcb";
            const auto pos = assists.find(code.back());
            if (pos != std::string::npos) {
                auto base = active_dict().find(code.substr(0, code.size() - 1));
                if (base != active_dict().end()) {
                    auto visible = base->second;
                    filter_extensions(visible);
                    const std::size_t candidate_index = pos + 1; // #0 is the first candidate; V selects #1.
                    if (candidate_index < visible.size()) result = {visible[candidate_index]};
                }
            }
        }
        return result;
    }
    std::vector<Candidate> visible_candidates() const {
        if(!query_preview_codes_.empty()){
            std::vector<Candidate> rows;
            rows.reserve(query_preview_codes_.size());
            for(auto code:query_preview_codes_){
                for(char& c:code)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                rows.push_back({std::move(code)});
            }
            return rows;
        }
        if(fast_preview_code_.empty())return lookup(composition_);
        std::string code=fast_preview_code_;
        for(char& c:code)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return {{std::move(code)}};
    }
    void filter_extensions(std::vector<Candidate>& candidates) const {
        if (!show_extensions_)
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
                return extension_filter::contains_extension_han(candidate.text);
            }), candidates.end());
    }
    void refresh() {
        std::vector<Candidate> all=related_stage_?related_candidates_:
            (preview_.empty()||!fast_preview_code_.empty()||!query_preview_codes_.empty()
                ? visible_candidates() : std::vector<Candidate>{});
        bool identity_changed = !has_candidate_identity_ ||
            candidate_identity_composition_ != composition_ ||
            candidate_identity_query_ != query_ || candidate_identity_mode_ != mode_ ||
            candidate_identity_show_extensions_ != show_extensions_ ||
            candidate_identity_related_stage_ != related_stage_ ||
            candidate_identity_texts_.size() != all.size();
        if (!identity_changed) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (candidate_identity_texts_[i] != all[i].text) { identity_changed = true; break; }
            }
        }
        if (identity_changed && !candidate_page_indices_.empty()) {
            candidate_page_indices_.clear();
            page_ = 0;
        }
        if (identity_changed) candidate_selection_ = 0;
        candidate_identity_composition_ = composition_;
        candidate_identity_query_ = query_;
        candidate_identity_mode_ = mode_;
        candidate_identity_show_extensions_ = show_extensions_;
        candidate_identity_related_stage_ = related_stage_;
        candidate_identity_texts_.clear();
        candidate_identity_texts_.reserve(all.size());
        for (const auto& candidate : all) candidate_identity_texts_.push_back(candidate.text);
        has_candidate_identity_ = true;

        const auto starts = page_starts(all);
        if (page_ >= starts.size()) page_ = 0;
        const std::size_t start = starts.empty() ? 0 : starts[page_];
        const std::size_t end = !starts.empty() && page_ + 1 < starts.size() ? starts[page_ + 1] : all.size();
        if (all.empty()) candidate_selection_ = 0;
        else if (candidate_selection_ < start || candidate_selection_ >= end) candidate_selection_ = start;
        std::vector<Candidate> page;
        if (start < all.size()) {
            page.assign(all.begin() + static_cast<std::ptrdiff_t>(start), all.begin() + static_cast<std::ptrdiff_t>(end));
        }
        outcome_.composition = !preview_.empty()?preview_:(query_==QueryStage::PhoneticInput||query_==QueryStage::PhoneticCandidates)
            ? "注"+zhuyin::display(composition_) : (query_==QueryStage::None?"":"'")+composition_;
        outcome_.candidates = std::move(page);
        outcome_.candidate_page_indices = starts;
        outcome_.candidate_selection = candidate_selection_;
        outcome_.layout_candidates = all.empty()
            ? nullptr : std::make_shared<const std::vector<Candidate>>(std::move(all));
        outcome_.page = page_;
        outcome_.mode = mode_;
        outcome_.query = query_;
        outcome_.query_preview = !preview_.empty();
        outcome_.related_stage=related_stage_;
    }
    void select(std::size_t index, bool cancel_unmatched = false) {
        auto all = lookup(composition_);
        const auto starts = page_starts(all);
        const auto at = starts.empty() ? all.size() : starts[page_] + index;
        const auto end = !starts.empty() && page_ + 1 < starts.size() ? starts[page_ + 1] : all.size();
        if (!starts.empty() && at < end) {
            const auto text=all[at].text;
            if(query_==QueryStage::HomophoneSeed){open_homophones(text);return;}
            if(query_==QueryStage::Homophones){
                outcome_.commit=text;composition_.clear();clear_query();page_=0;refresh();return;
            }
            if(query_==QueryStage::PhoneticCandidates){
                preview_=text;query_hint(text);
                query_preview_codes_=query_preview_codes(text);
                refresh();return;
            }
            const bool fast_input=mode_==Mode::Fast&&query_==QueryStage::None;
            const auto shortest=fast_input?best_code(text):std::string{};
            const bool longer=fast_input&&!shortest.empty()&&composition_.size()>shortest.size();
            if(longer&&!speed_hint_only_){
                preview_=text;fast_preview_code_=shortest;page_=0;refresh();return;
            }
            if(longer){outcome_.hint=shortest;outcome_.hint_kind=HintKind::Shortest;}
            const bool suppress=longer&&speed_hint_only_;
            if(!suppress)outcome_.commit = text;
            composition_.clear();clear_query(); page_ = 0;
        }
        else if (cancel_unmatched && all.empty()) { composition_.clear();clear_query(); page_ = 0; }
        refresh();
    }
    std::vector<std::size_t> page_starts(const std::vector<Candidate>& all) const {
        if (!related_stage_ && !candidate_page_indices_.empty()) return candidate_page_indices_;
        std::vector<std::size_t> starts;
        if (all.empty()) return {};
        for (std::size_t index = 0; index < all.size(); index += 10) starts.push_back(index);
        return starts;
    }
    std::size_t page_count(const std::vector<Candidate>& all) const {
        return page_starts(all).size();
    }
};
} // namespace boshiamy
#endif
