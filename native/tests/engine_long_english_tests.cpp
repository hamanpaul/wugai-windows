#include "../engine.hpp"
#include <iostream>
#include <string>
#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

int main() {
    using namespace boshiamy;
    Engine engine;
    std::string expected;
    for (int i=0; i<2048; ++i) {
        const char c=static_cast<char>((i%2 ? 'A' : 'a') + i%26);
        expected.push_back(c);
        engine.key(c);
    }
    CHECK(engine.snapshot().composition.size()==expected.size());
    CHECK(engine.snapshot().candidates.empty());
    engine.key('7'); expected.push_back('7');
    engine.key('-'); expected.push_back('-');
    engine.key(Key::Backspace); expected.pop_back();
    CHECK(engine.key(Key::Enter).commit==expected);
    CHECK(engine.snapshot().composition.empty());

    engine.add("abcde",{"\xe5\xb0\x8d"});
    for(char c:std::string("abcde")) engine.key(c);
    CHECK(engine.snapshot().candidates.size()==1);
    engine.key('f');
    CHECK(engine.snapshot().composition=="abcdef" && engine.snapshot().candidates.empty());
    engine.key(Key::Backspace);
    CHECK(engine.snapshot().composition=="abcde" && engine.snapshot().candidates.size()==1);
    CHECK(engine.key(Key::Space).commit=="\xe5\xb0\x8d");
    engine.add("a",{"one","two"});
    engine.key('a');
    CHECK(engine.key('1').commit=="two");
    for(char c:std::string("longenglish")) engine.key(c);
    engine.key(Key::Escape);
    CHECK(engine.snapshot().composition.empty() && engine.key(Key::Enter).commit.empty());
    std::cout << "long English composition and Chinese candidate regression passed\n";
}
