#include "../tsf/candidate_window.hpp"
#include <iostream>
#include <vector>

#define CHECK(x) do { if(!(x)){std::cerr << #x << " line " << __LINE__ << '\n';return 1;} } while(0)

static BOOL CALLBACK collect(HWND hwnd, LPARAM value) {
    wchar_t text[128]{};GetWindowTextW(hwnd,text,128);
    if(wcsncmp(text,L"0 ",2)==0||wcsncmp(text,L"1 ",2)==0)
        reinterpret_cast<std::vector<HWND>*>(value)->push_back(hwnd);
    return TRUE;
}
int main(){
    boshiamy::CandidateWindow window;
    CHECK(window.hwnd() && window.viewport_hwnd());
    boshiamy::Outcome out;out.composition="'mnfo";
    out.query=boshiamy::QueryStage::Homophones;
    out.candidates={{"蝦","cofu"},{"岈","exp"}};
    out.layout_candidates=std::make_shared<const std::vector<boshiamy::Candidate>>(out.candidates);
    out.candidate_page_indices={0};out.candidate_selection=0;
    RECT anchor{80,80,82,100},work{0,0,1280,900};
    window.update(out,anchor,96,work);
    std::vector<HWND> labels;EnumChildWindows(window.viewport_hwnd(),collect,reinterpret_cast<LPARAM>(&labels));
    CHECK(labels.size()==2);
    for(auto label:labels)UpdateWindow(label);
    out.candidate_selection=1;window.update(out,anchor,96,work);
    for(auto label:labels)CHECK(GetUpdateRect(label,nullptr,FALSE));
    std::cout << "candidate_selection_redraw_passed\n";
}
