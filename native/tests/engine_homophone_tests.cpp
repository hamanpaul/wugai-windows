#include "../dictionary.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#define CHECK(x) do { if(!(x)){std::cerr << #x << " line " << __LINE__ << '\n';return 1;} } while(0)
int main(int argc,char** argv){
 using namespace boshiamy;
 Engine e; e.add("s",{"Z"});
 std::ostringstream rows; rows << "mode\tseed\tordinal\tcandidate_text\n";
 for(int i=0;i<12;++i){std::string text(1,static_cast<char>('A'+i)); e.add(std::string(1,static_cast<char>('a'+i)),{text});rows << "traditional\tZ\t" << i+1 << '\t' << text << '\n';}
 std::istringstream readings("Z\treading\n");e.set_readings(Readings::parse(readings));
 std::istringstream index(rows.str());e.set_observed_primary_homophones(ObservedPrimaryHomophoneIndex::parse(index));
 e.key('\'');e.key('s');auto seed=e.snapshot();CHECK(seed.query==QueryStage::HomophoneSeed);CHECK(seed.candidates[0].text=="Z");
 auto list=e.key(Key::Space);CHECK(list.query==QueryStage::Homophones);CHECK(list.commit.empty());CHECK(list.candidates.size()==10);CHECK(list.candidates[0].text=="A");CHECK(candidate_display_text(list.candidates[0])=="A A");
 auto second=e.key(Key::Space);CHECK(second.query==QueryStage::Homophones);CHECK(second.page==1);CHECK(second.candidates[0].text=="K");
 auto picked=e.key('0');CHECK(picked.commit=="K");CHECK(picked.hint.empty());CHECK(picked.query==QueryStage::None);CHECK(picked.composition.empty());
 e.key('\'');e.key('s');e.key(Key::Space);e.key(Key::Space);auto wrap=e.key(Key::Space);CHECK(wrap.page==0);CHECK(wrap.candidates[0].text=="A");
 auto picked2=e.key('1');CHECK(picked2.commit=="B");CHECK(picked2.query==QueryStage::None);
 e.key('\'');e.key('s');e.key(Key::Space);auto down=e.key(Key::Down);CHECK(down.candidate_selection==1&&down.commit.empty());
 auto up=e.key(Key::Up);CHECK(up.candidate_selection==0&&up.commit.empty());
 auto last=e.key(Key::Up);CHECK(last.page==1&&last.candidate_selection==11);CHECK(e.key(Key::Enter).commit=="L");
 e.key('\'');e.key('s');e.key(Key::Space);CHECK(e.key(Key::Enter).commit=="A");
 e.key('\'');e.key('s');e.key(Key::Space);for(int i=0;i<10;++i)e.key(Key::Down);CHECK(e.snapshot().page==1&&e.snapshot().candidate_selection==10);CHECK(e.key(Key::Enter).commit=="K");
 e.key('\'');e.key('s');e.key(Key::Space);auto cancel=e.key(Key::Escape);CHECK(cancel.commit.empty());CHECK(cancel.query==QueryStage::None);
 e.key('s');CHECK(e.key(Key::Space).commit=="Z");
 CHECK(argc==4); std::ifstream dictionary(argv[1],std::ios::binary), actualReadings(argv[2],std::ios::binary), phonetic(argv[3],std::ios::binary);
 CHECK(dictionary && actualReadings && phonetic); auto real=load_dictionary(dictionary).make_engine();
 real.set_readings(Readings::parse(actualReadings));real.set_phonetic_candidates(PhoneticCandidateIndex::parse(phonetic));real.set_observed_primary_homophones(ObservedPrimaryHomophoneIndex::built_in());
 real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto actual=real.key(Key::Space);
 CHECK(actual.query==QueryStage::Homophones);CHECK(!actual.candidates.empty());CHECK(actual.candidates[0].text=="\xe8\x9d\xa6");
 CHECK(candidate_display_text(actual.candidates[0])=="蝦 CFA");auto shrimp=real.key('0');CHECK(shrimp.commit=="\xe8\x9d\xa6");CHECK(shrimp.hint.empty());
 std::istringstream recommended("section\ttext\tcode\nTRADITIONAL\t蝦\tcofu\n");real.set_recommendations(RecommendedCodes::parse(recommended));
 real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto recommendedList=real.key(Key::Space);CHECK(candidate_display_text(recommendedList.candidates[0])=="蝦 COFU");
 auto recommendedShrimp=real.key('0');CHECK(recommendedShrimp.commit=="蝦");CHECK(recommendedShrimp.hint.empty());
 real.set_reverse_mode(ReverseMode::All);real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto allList=real.key(Key::Space);CHECK(candidate_display_text(allList.candidates[0])=="蝦 CFA / COX / COFU");
 auto allShrimp=real.key('0');CHECK(allShrimp.commit=="蝦");CHECK(allShrimp.hint.empty());
 std::cout << "homophone_query_tests_passed\n";return 0;
}
