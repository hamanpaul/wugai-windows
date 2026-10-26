#ifndef BOSHIAMY_ROOT_CHART_HPP
#define BOSHIAMY_ROOT_CHART_HPP
#include <array>
#include <cstddef>
#include <string_view>
namespace boshiamy {
struct RootChartRow { char key; std::wstring_view shape; std::wstring_view sound; std::wstring_view meaning; std::wstring_view supplement; int height; };
struct RootChartMarginNote { char first_key; char last_key; std::wstring_view text; };
struct RootChartGlyphSource { wchar_t token; char key; char column; int source_x0; int source_y0; int source_x1; int source_y1; };
// Private-use markers in the shape column are drawing tokens, not Unicode
// character claims. Their normalized strokes are rendered by RootChartWindow.
inline constexpr wchar_t root_chart_glyph_e0=L'\uE000';
inline constexpr wchar_t root_chart_glyph_e1=L'\uE001';
inline constexpr wchar_t root_chart_glyph_e2=L'\uE002';
inline constexpr wchar_t root_chart_glyph_x0=L'\uE003';
inline constexpr wchar_t root_chart_glyph_x1=L'\uE004';
inline constexpr wchar_t root_chart_glyph_x2=L'\uE005';
inline constexpr wchar_t root_chart_glyph_y0=L'\uE006';
inline constexpr wchar_t root_chart_glyph_z0=L'\uE007';
inline constexpr wchar_t root_chart_glyph_z1=L'\uE008';
inline constexpr wchar_t root_chart_glyph_f0=L'\uE009';
inline constexpr wchar_t root_chart_glyph_f1=L'\uE00A';
inline constexpr wchar_t root_chart_glyph_f2=L'\uE00B';
inline constexpr wchar_t root_chart_glyph_f3=L'\uE00C';
inline constexpr wchar_t root_chart_glyph_f4=L'\uE00D';
inline constexpr wchar_t root_chart_glyph_f5=L'\uE00E';
inline constexpr wchar_t root_chart_glyph_f6=L'\uE00F';
inline constexpr wchar_t root_chart_glyph_q0=L'\uE010';
inline constexpr wchar_t root_chart_glyph_q1=L'\uE011';
inline constexpr wchar_t root_chart_glyph_q2=L'\uE012';
inline constexpr wchar_t root_chart_glyph_q3=L'\uE013';
inline constexpr wchar_t root_chart_glyph_q4=L'\uE014';
inline constexpr wchar_t root_chart_glyph_q5=L'\uE015';
inline constexpr wchar_t root_chart_glyph_r0=L'\uE016';
inline constexpr wchar_t root_chart_glyph_r1=L'\uE017';
inline constexpr wchar_t root_chart_glyph_r2=L'\uE018';
inline constexpr wchar_t root_chart_glyph_r3=L'\uE019';
inline constexpr wchar_t root_chart_glyph_r4=L'\uE01A';
inline constexpr wchar_t root_chart_glyph_s0=L'\uE01B';
inline constexpr wchar_t root_chart_glyph_s1=L'\uE01C';
inline constexpr wchar_t root_chart_glyph_s2=L'\uE01D';
inline constexpr wchar_t root_chart_glyph_s3=L'\uE01E';
inline constexpr wchar_t root_chart_glyph_s4=L'\uE01F';
inline constexpr wchar_t root_chart_glyph_s5=L'\uE020';
inline constexpr wchar_t root_chart_glyph_b0=L'\uE021';
inline constexpr wchar_t root_chart_glyph_q6=L'\uE022';
inline constexpr wchar_t root_chart_glyph_o0=L'\uE023';
inline constexpr wchar_t root_chart_glyph_o1=L'\uE024';
inline constexpr wchar_t root_chart_glyph_o2=L'\uE025';
inline constexpr wchar_t root_chart_glyph_fm0=L'\uE026';
inline constexpr wchar_t root_chart_glyph_fm1=L'\uE027';
inline constexpr wchar_t root_chart_glyph_fm2=L'\uE028';
inline constexpr wchar_t root_chart_glyph_fm3=L'\uE029';
inline constexpr wchar_t root_chart_glyph_fm4=L'\uE02A';
inline constexpr wchar_t root_chart_glyph_fm5=L'\uE02B';
inline constexpr wchar_t root_chart_glyph_fm6=L'\uE02C';
inline constexpr wchar_t root_chart_glyph_km0=L'\uE02D';
inline constexpr wchar_t root_chart_glyph_km1=L'\uE02E';
inline constexpr wchar_t root_chart_glyph_km2=L'\uE02F';
inline constexpr wchar_t root_chart_glyph_t0=L'\uE030';
inline constexpr wchar_t root_chart_glyph_fs0=L'\uE031';
inline constexpr wchar_t root_chart_glyph_fs1=L'\uE032';
inline constexpr wchar_t root_chart_glyph_es0=L'\uE033';
inline constexpr wchar_t root_chart_glyph_es1=L'\uE034';
inline constexpr wchar_t root_chart_glyph_gs0=L'\uE035';
inline constexpr wchar_t root_chart_glyph_gs1=L'\uE036';
inline constexpr wchar_t root_chart_glyph_gs2=L'\uE037';
inline constexpr wchar_t root_chart_glyph_es2=L'\uE038';
inline constexpr wchar_t root_chart_glyph_es3=L'\uE039';
inline constexpr wchar_t root_chart_glyph_es4=L'\uE03A';
inline constexpr wchar_t root_chart_glyph_gs3=L'\uE03B';
// Regions are per-glyph bounds in the 460x600 official RT_BITMAP (resource
// 113), not whole-cell copies; normalized strokes live in the renderer.
inline constexpr std::array<RootChartGlyphSource,60> root_chart_glyph_sources{{
 {root_chart_glyph_f0,'F','S',41,127,49,137},{root_chart_glyph_f1,'F','S',52,127,62,136},{root_chart_glyph_f2,'F','S',67,127,73,137},
 {root_chart_glyph_f3,'F','S',40,142,51,151},{root_chart_glyph_f4,'F','S',52,142,62,152},{root_chart_glyph_f5,'F','S',73,142,78,152},{root_chart_glyph_f6,'F','S',83,142,89,152},
 {root_chart_glyph_e0,'E','S',41,111,52,117},{root_chart_glyph_e1,'E','S',55,108,61,118},{root_chart_glyph_e2,'E','S',64,111,74,117},
 {root_chart_glyph_x0,'X','S',40,550,48,560},{root_chart_glyph_x1,'X','S',52,549,62,560},{root_chart_glyph_x2,'X','S',67,550,76,560},
 {root_chart_glyph_y0,'Y','S',41,566,46,576},{root_chart_glyph_z0,'Z','S',41,584,50,593},{root_chart_glyph_z1,'Z','S',53,584,64,592},
 {root_chart_glyph_q0,'Q','M',254,397,261,406},{root_chart_glyph_q1,'Q','M',267,396,276,406},{root_chart_glyph_q2,'Q','M',279,397,290,405},
 {root_chart_glyph_q3,'Q','M',294,396,304,406},{root_chart_glyph_q4,'Q','M',309,398,316,405},{root_chart_glyph_q5,'Q','M',322,395,331,406},
 {root_chart_glyph_r0,'R','M',255,413,259,424},{root_chart_glyph_r1,'R','M',265,414,269,424},{root_chart_glyph_r2,'R','M',295,414,301,423},
 {root_chart_glyph_r3,'R','M',307,414,317,424},{root_chart_glyph_r4,'R','M',321,415,329,422},
 {root_chart_glyph_s0,'S','M',254,440,263,450},{root_chart_glyph_s1,'S','M',269,441,278,449},{root_chart_glyph_s2,'S','M',283,440,294,451},
 {root_chart_glyph_s3,'S','M',298,442,309,448},{root_chart_glyph_s4,'S','M',314,442,323,449},{root_chart_glyph_s5,'S','M',328,442,338,449},
 {root_chart_glyph_b0,'B','S',60,42,66,51},
 {root_chart_glyph_q6,'Q','M',337,396,342,406},
 {root_chart_glyph_o0,'O','S',67,363,75,373},
 {root_chart_glyph_o1,'O','S',79,366,90,371},
 {root_chart_glyph_o2,'O','S',93,363,103,373},
 {root_chart_glyph_fm0,'F','M',270,131,278,135},
 {root_chart_glyph_fm1,'F','M',285,129,295,137},
 {root_chart_glyph_fm2,'F','M',300,129,311,135},
 {root_chart_glyph_fm3,'F','M',317,129,325,137},
 {root_chart_glyph_fm4,'F','M',331,127,342,138},
 {root_chart_glyph_fm5,'F','M',347,130,357,136},
 {root_chart_glyph_fm6,'F','M',362,130,372,136},
 {root_chart_glyph_km0,'K','M',323,245,333,255},
 {root_chart_glyph_km1,'K','M',337,245,346,255},
 {root_chart_glyph_km2,'K','M',349,244,360,255},
 {root_chart_glyph_t0,'T','M',265,464,282,475},
 {root_chart_glyph_fs0,'F','S',158,126,167,137},
 {root_chart_glyph_fs1,'F','S',170,126,187,136},
 {root_chart_glyph_es0,'E','S',177,110,187,118},
 {root_chart_glyph_es1,'E','S',190,108,201,120},
 {root_chart_glyph_gs0,'G','S',137,161,146,171},
 {root_chart_glyph_gs1,'G','S',149,161,159,172},
 {root_chart_glyph_gs2,'G','S',162,161,171,171},
 {root_chart_glyph_es2,'E','S',138,109,149,120},
 {root_chart_glyph_es3,'E','S',153,109,160,119},
 {root_chart_glyph_es4,'E','S',211,109,221,119},
 {root_chart_glyph_gs3,'G','S',177,175,182,186}
}};
inline constexpr std::array<RootChartRow,26> root_chart_rows{{
 {'A',L"人（金） 寸",L"也",L"丶（一點）",L"",38},
 {'B',L"阝 \uE021",L"半（羊） 不 比",L"人 入 丷 氵 匕（二點）",L"八",38},
 {'C',L"",L"西 夕",L"車（Car） 中央（Center）\n（虫 亘 史 串 里 室 田 戶 巴 尸）",L"七",68},
 {'D',L"力 夕",L"大 刀 鬥 歹 豆（且）斗 勿 鼎",L"日（Day）（曰 月）",L"的",38},
 {'E',L"\uE000 \uE001 \uE002 山 心 小 爪",L"尹（\uE038 \uE039 丑 \uE033 \uE034） \uE03A 與",L"少 西 丌 也 且 手 刂",L"一",38},
 {'F',L"\uE009 \uE00A \uE00B\n\uE00C \uE00D（\uE00E \uE00F）",L"方 戶 飛 \uE031\uE032\n匚（匕 亡 言 乙）",L"火 \uE026 \uE027 \uE028 \uE029 \uE02A \uE02B \uE02C\n手 皿 田 由（四點）",L"四",68},
 {'G',L"",L"千（\uE035 \uE036 \uE037 毛）\n甘 瓜 夾 白 \uE03B（片）",L"女（Girl）",L"",60},
 {'H',L"月（朋）",L"乎 禾 么",L"手（Hand）（手主手）心（Heart）（小）",L"",38},
 {'I',L"工 言（i） 丨 丨 丨",L"",L"我（I）（戈）",L"",38},
 {'J',L"才",L"井 央 束 交 其（其） 久（欠）",L"大（直交）",L"十",38},
 {'K',L"以 代 長 尺",L"开 气 其 五（ㄨ） ㄅ",L"王（King）（王） \uE02D \uE02E \uE02F\n斤（kg）（斤）",L"九",60},
 {'L',L"ㄑ（乙） 厂（广）",L"立（一半） 來（來） 未 庶 兩 皿",L"",L"六",38},
 {'M',L"田 冊 凹\n乡（乡少）",L"米（書米） 目（且）\n毛 禾 門 馬 一",L"",L"",60},
 {'N',L"勺 一（ㄣ）\n冂（巾角）",L"寅 鳥 人（捺）",L"",L"",60},
 {'O',L"口 囗 \uE023 \uE024 \uE025",L"",L"",L"O",38},
 {'P',L"尸（巴）",L"皮 平 卜 片 欠 八（撒）",L"亻（People）",L"",38},
 {'Q',L"田（里） 回（里）",L"弓 骨",L"\uE010 \uE011 \uE012 \uE013 \uE014 \uE015 \uE022（Bar. B. Q）",L"",38},
 {'R',L"乃 几",L"耳（耳） 儿 爾",L"\uE016 \uE017（臣） \uE018 \uE019 \uE01A",L"二",38},
 {'S',L"",L"小（小个） 少（少） 系 身 世\n糸 皿 生（牛 牛 牛 牛 生）",L"\uE01B \uE01C \uE01D \uE01E \uE01F \uE020",L"三",60},
  {'T',L"丁 一 マ",L"去（云）",L"木\uE030（Tree）",L"",38},
 {'U',L"山（幽） 人",L"永 月（月夕）\n千 子 雨 又 魚",L"",L"",60},
 {'V',L"ノ",L"弗 彡（万）",L"佳（Very good）（隹）",L"",38},
 {'W',L"么（玄之） 了",L"文（旅） 午 垂（垂） 瓦 鳥 馬",L"水（Water）（氵 水 永） 土",L"五",38},
 {'X',L"\uE003 \uE004 \uE005（斜交）",L"食 氏（氏） 升 電（龜）",L"",L"",38},
 {'Y',L"\uE006",L"刁 一",L"",L"",38},
 {'Z',L"\uE007 \uE008（九孔）",L"竹 子 舟 止 片 卜 乍",L"",L"",38}
}};
inline constexpr std::array<RootChartMarginNote,5> root_chart_margin_notes{{
 {'I','I',L"欠"}, {'K','K',L"黃"}, {'P','Q',L"至\n兼永"}, {'U','V',L"才\n禾鬼"}, {'X','Y',L"牙\n土土"}
}};
inline constexpr std::wstring_view root_chart_shape_header=L"形";
inline constexpr std::wstring_view root_chart_sound_header=L"音（含近音）";
inline constexpr std::wstring_view root_chart_meaning_header=L"義";
inline constexpr bool root_chart_transcription_complete=false;
}
#endif
