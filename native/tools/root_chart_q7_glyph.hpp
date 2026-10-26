#ifndef BOSHIAMY_ROOT_CHART_Q7_GLYPH_HPP
#define BOSHIAMY_ROOT_CHART_Q7_GLYPH_HPP
#include <array>
#include <cstdint>
namespace boshiamy {
// Q-meaning glyph 7, cropped from RT_BITMAP 113 at source bbox
// [337,396)-[342,406). Each run stores source RGB for pixels with mean < 235.
// The external source image is never loaded by the product.
struct RootChartQ7InkRun { std::uint8_t y,x0,x1,r,g,b; };
inline constexpr std::uint8_t root_chart_q7_source_width=5;
inline constexpr std::uint8_t root_chart_q7_source_height=10;
inline constexpr std::array<RootChartQ7InkRun,28> root_chart_q7_ink_runs{{
 {0,4,5,164,161,161},
 {1,1,2,216,215,215},
 {1,2,3,200,198,198},
 {1,3,4,159,156,155},
 {1,4,5,139,134,133},
 {2,2,3,146,142,141},
 {2,3,4,106,102,97},
 {3,1,2,184,182,182},
 {3,2,3,173,170,170},
 {3,3,4,153,149,146},
 {3,4,5,230,229,229},
 {4,3,4,100,94,89},
 {4,4,5,187,185,185},
 {5,2,3,121,116,115},
 {5,3,5,161,158,157},
 {6,0,1,226,225,225},
 {6,1,2,123,120,119},
 {6,2,3,213,213,213},
 {6,3,4,204,202,202},
 {6,4,5,157,154,153},
 {7,0,1,208,207,206},
 {7,3,4,164,161,161},
 {7,4,5,176,174,173},
 {8,2,3,155,152,151},
 {8,3,4,77,69,66},
 {8,4,5,218,217,217},
 {9,2,3,208,207,206},
 {9,3,4,170,168,168},
}};
}
#endif
