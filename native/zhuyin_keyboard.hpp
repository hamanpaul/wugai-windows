#ifndef BOSHIAMY_ZHUYIN_KEYBOARD_HPP
#define BOSHIAMY_ZHUYIN_KEYBOARD_HPP

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace boshiamy::zhuyin {

inline const std::string& keys() {
    static const std::string value = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-";
    return value;
}

inline const std::vector<std::string>& symbols() {
    static const std::vector<std::string> value = {
        "ㄅ","ㄆ","ㄇ","ㄈ","ㄉ","ㄊ","ㄋ","ㄌ","ㄍ","ㄎ","ㄏ","ㄐ",
        "ㄑ","ㄒ","ㄓ","ㄔ","ㄕ","ㄖ","ㄗ","ㄘ","ㄙ","ㄧ","ㄨ","ㄩ",
        "ㄚ","ㄛ","ㄜ","ㄝ","ㄞ","ㄟ","ㄠ","ㄡ","ㄢ","ㄣ","ㄤ","ㄥ","ㄦ"
    };
    return value;
}

inline char normalized(char key) {
    return key >= 'A' && key <= 'Z' ? static_cast<char>(key - 'A' + 'a') : key;
}

inline bool is_tone(char key) {
    key = normalized(key);
    return key == '6' || key == '3' || key == '4' || key == '7';
}

inline bool has_tone(const std::string& input) {
    return std::any_of(input.begin(), input.end(), [](char key) { return is_tone(key); });
}

inline bool is_symbol(char key) {
    key = normalized(key);
    return keys().find(key) != std::string::npos;
}

inline std::string symbol(char key);

inline int category(char key) {
    const std::string value = symbol(key);
    static const std::vector<std::string> initials = {
        "ㄅ","ㄆ","ㄇ","ㄈ","ㄉ","ㄊ","ㄋ","ㄌ","ㄍ","ㄎ","ㄏ","ㄐ",
        "ㄑ","ㄒ","ㄓ","ㄔ","ㄕ","ㄖ","ㄗ","ㄘ","ㄙ","ㄦ"
    };
    static const std::vector<std::string> medials = {"ㄧ","ㄨ","ㄩ"};
    static const std::vector<std::string> finals = {
        "ㄚ","ㄛ","ㄜ","ㄝ","ㄞ","ㄟ","ㄠ","ㄡ","ㄢ","ㄣ","ㄤ","ㄥ"
    };
    if (std::find(initials.begin(), initials.end(), value) != initials.end()) return 0;
    if (std::find(medials.begin(), medials.end(), value) != medials.end()) return 1;
    if (std::find(finals.begin(), finals.end(), value) != finals.end()) return 2;
    return 3;
}

// Keep the editable Zhuyin key sequence in canonical initial/medial/final
// order. A later key in the same category replaces the existing category.
// The Engine stores original key order separately for the Enter override.
inline std::string append_canonical(std::string input, char key) {
    key = normalized(key);
    if (is_tone(key)) {
        for (char& existing : input) {
            if (is_tone(existing)) {
                existing = key;
                return input;
            }
        }
        input.push_back(key);
        return input;
    }
    if (!is_symbol(key)) return input;
    const int target = category(key);
    for (char& existing : input) {
        if (!is_tone(existing) && category(existing) == target) {
            existing = key;
            return input;
        }
    }
    const auto position = std::find_if(input.begin(), input.end(), [target](char existing) {
        return is_tone(existing) || category(existing) > target;
    });
    input.insert(position, key);
    return input;
}

inline std::string symbol(char key) {
    key = normalized(key);
    if (key == ' ') return "ˉ";
    if (is_tone(key)) {
        switch (key) {
        case '6': return "ˊ";
        case '3': return "ˇ";
        case '4': return "ˋ";
        default: return "˙";
        }
    }
    const auto pos = keys().find(key);
    return pos == std::string::npos ? std::string{} : symbols()[pos];
}

inline std::string display(const std::string& input) {
    std::string result;
    std::vector<char> ordered(input.begin(), input.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](char left, char right) {
        const auto rank = [](char key) { return is_tone(key) ? 3 : category(key); };
        return rank(left) < rank(right);
    });
    for (char key : ordered) result += symbol(key);
    return result;
}

inline std::optional<std::string> reading(const std::string& input) {
    static const std::vector<std::string> initials = {
        // ㄦ occupies the official keyboard's class-0 editable slot. This is
        // an editing/category rule, not a claim about its phonological role.
        "ㄅ","ㄆ","ㄇ","ㄈ","ㄉ","ㄊ","ㄋ","ㄌ","ㄍ","ㄎ","ㄏ","ㄐ","ㄑ","ㄒ","ㄓ","ㄔ","ㄕ","ㄖ","ㄗ","ㄘ","ㄙ","ㄦ"
    };
    static const std::vector<std::string> medials = {"ㄧ","ㄨ","ㄩ"};
    static const std::vector<std::string> finals = {
        "ㄚ","ㄛ","ㄜ","ㄝ","ㄞ","ㄟ","ㄠ","ㄡ","ㄢ","ㄣ","ㄤ","ㄥ"
    };
    bool has_initial = false, has_medial = false, has_final = false, has_tone = false;
    std::string initial, medial, final, tone;
    for (char key : input) {
        key = normalized(key);
        if (key == '7') {
            if (has_tone) return std::nullopt;
            has_tone = true;
            tone = "˙";
            continue;
        }
        if (is_tone(key)) {
            if (has_tone) return std::nullopt;
            has_tone = true;
            if (key == '6') tone = "ˊ";
            else if (key == '3') tone = "ˇ";
            else if (key == '4') tone = "ˋ";
            else tone = "˙";
            continue;
        }
        if (has_tone) return std::nullopt;
        if (!is_symbol(key)) return std::nullopt;
        const std::string value = symbol(key);
        if (std::find(initials.begin(), initials.end(), value) != initials.end()) {
            if (has_initial || has_medial || has_final) return std::nullopt;
            has_initial = true; initial = value;
        } else if (std::find(medials.begin(), medials.end(), value) != medials.end()) {
            if (has_medial || has_final) return std::nullopt;
            has_medial = true; medial = value;
        } else if (std::find(finals.begin(), finals.end(), value) != finals.end()) {
            if (has_final) return std::nullopt;
            has_final = true; final = value;
        } else return std::nullopt;
    }
    if (!has_initial && !has_medial && !has_final) return std::nullopt;
    if (tone == "˙") return tone + initial + medial + final;
    return initial + medial + final + tone;
}

}
#endif
