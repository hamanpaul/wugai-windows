#ifndef BOSHIAMY_COMPOSITION_DISPLAY_HPP
#define BOSHIAMY_COMPOSITION_DISPLAY_HPP
#include <string>
namespace boshiamy {
// Presentation only: dictionary lookup and Engine state retain normalized roots.
inline std::string composition_display(std::string roots) {
    for(char& c:roots)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
    return roots;
}
}
#endif
