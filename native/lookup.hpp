#ifndef BOSHIAMY_LOOKUP_HPP
#define BOSHIAMY_LOOKUP_HPP

#include "dictionary.hpp"
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace boshiamy {

struct LookupResult {
    std::string code;
    std::string text;
    bool operator==(const LookupResult& other) const {
        return code == other.code && text == other.text;
    }
};

// Read-only inverse/search view over the application's parsed Android table.
// Code order follows Dictionary's ordered code map; duplicate texts retain
// their row order within each code, matching the loader's source-preserving lists.
class Lookup {
public:
    static constexpr std::size_t max_code_length = 5;
    explicit Lookup(const Dictionary& dictionary) : sections_(dictionary.sections) {}
    Lookup(Dictionary&&) = delete;
    Lookup(const Dictionary&&) = delete;
    explicit Lookup(const std::map<Mode, std::map<std::string, std::vector<std::string>>>& sections)
        : sections_(sections) {}
    Lookup(std::map<Mode, std::map<std::string, std::vector<std::string>>>&&) = delete;
    Lookup(const std::map<Mode, std::map<std::string, std::vector<std::string>>>&&) = delete;

    // Every stored code that emits this exact text, in stable dictionary order.
    std::vector<std::string> text(const std::string& value, Mode mode) const {
        std::vector<std::string> result;
        const auto* entries = section(mode);
        if (!entries) return result;
        for (const auto& code : *entries)
            for (const auto& candidate : code.second)
                if (candidate == value) { result.push_back(code.first); break; }
        return result;
    }

    // Codes that actually produce this text through an auxiliary suffix. A
    // stored full code masks the derived interpretation, even when its text is
    // identical; its ordinary text() result reports that code without a false
    // derived marker.
    std::vector<LookupResult> derived_text(const std::string& value, Mode mode) const {
        std::vector<LookupResult> result;
        const auto* entries = section(mode);
        if (!entries) return result;
        static const std::string assists = "vrsfwlcb";
        for (const auto& base : *entries) {
            if (base.first.size() >= max_code_length) continue;
            for (std::size_t index = 1; index <= assists.size() && index < base.second.size(); ++index) {
                if (base.second[index] != value) continue;
                std::string derived_code = base.first + assists[index - 1];
                if (entries->find(derived_code) == entries->end())
                    result.push_back({std::move(derived_code), value});
            }
        }
        return result;
    }
    // All entries under a literal code prefix, with source ordering as above.
    std::vector<LookupResult> prefix(const std::string& value, Mode mode) const {
        std::vector<LookupResult> result;
        const auto* entries = section(mode);
        if (!entries) return result;
        auto it = value.empty() ? entries->begin() : entries->lower_bound(value);
        for (; it != entries->end(); ++it) {
            if (it->first.compare(0, value.size(), value) != 0) {
                if (!value.empty()) break;
                continue;
            }
            for (const auto& candidate : it->second) result.push_back({it->first, candidate});
        }
        return result;
    }

    // '?' matches one code character; '*' matches zero or more. The whole code
    // must match the pattern, which makes patterns useful for unknown roots.
    std::vector<LookupResult> match(const std::string& pattern, Mode mode) const {
        std::vector<LookupResult> result;
        const auto* entries = section(mode);
        if (!entries) return result;
        for (const auto& code : *entries)
            if (wildcard(pattern, code.first))
                for (const auto& candidate : code.second) result.push_back({code.first, candidate});
        return result;
    }

    // Exact stored codes win. Otherwise a final V/R/S/F/W/L/C/B derives the
    // corresponding 1st..8th alternate from its base code (index zero is first).
    std::vector<LookupResult> lookup_code(const std::string& code, Mode mode) const {
        if (code.size() > max_code_length) return {};
        const auto* entries = section(mode);
        if (!entries) return {};
        auto exact = entries->find(code);
        if (exact != entries->end()) {
            std::vector<LookupResult> result;
            for (const auto& candidate : exact->second) result.push_back({code, candidate});
            return result;
        }
        if (code.size() < 2) return {};
        const std::string assists = "vrsfwlcb";
        const auto pos = assists.find(code.back());
        if (pos == std::string::npos) return {};
        auto base = entries->find(code.substr(0, code.size() - 1));
        const auto index = pos + 1;
        if (base == entries->end() || index >= base->second.size()) return {};
        return {{code, base->second[index]}};
    }

private:
    const std::map<Mode, std::map<std::string, std::vector<std::string>>>& sections_;

    const std::map<std::string, std::vector<std::string>>* section(Mode mode) const {
        auto it = sections_.find(mode);
        return it == sections_.end() ? nullptr : &it->second;
    }

    static bool wildcard(const std::string& pattern, const std::string& value) {
        std::size_t p = 0, v = 0, star = std::string::npos, retry = 0;
        while (v < value.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) { ++p; ++v; }
            else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = v; }
            else if (star != std::string::npos) { p = star + 1; v = ++retry; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }
};
} // namespace boshiamy
#endif
