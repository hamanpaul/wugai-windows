#ifndef BOSHIAMY_OBSERVED_PRIMARY_HOMOPHONES_HPP
#define BOSHIAMY_OBSERVED_PRIMARY_HOMOPHONES_HPP

#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "observed_primary_homophones_data.hpp"

namespace boshiamy {

// Exact traditional-mode vectors captured from the official IME. A present
// empty vector is authoritative; an absent seed remains eligible for H3.
class ObservedPrimaryHomophoneIndex {
    using Table = std::map<std::string, std::vector<std::string>>;
    std::shared_ptr<const Table> by_seed_;

    static const std::shared_ptr<const Table>& empty_table() {
        static const auto table = std::make_shared<const Table>();
        return table;
    }

    static bool valid_utf8(const std::string& value) {
        std::size_t i = 0;
        while (i < value.size()) {
            const auto a = static_cast<unsigned char>(value[i]);
            if (a <= 0x7f) { ++i; continue; }
            if (a >= 0xc2 && a <= 0xdf) {
                if (i + 1 >= value.size()) return false;
                const auto b = static_cast<unsigned char>(value[i + 1]);
                if (b < 0x80 || b > 0xbf) return false;
                i += 2; continue;
            }
            if (a >= 0xe0 && a <= 0xef) {
                if (i + 2 >= value.size()) return false;
                const auto b = static_cast<unsigned char>(value[i + 1]);
                const auto c = static_cast<unsigned char>(value[i + 2]);
                if (c < 0x80 || c > 0xbf ||
                    (a == 0xe0 ? (b < 0xa0 || b > 0xbf) :
                     a == 0xed ? (b < 0x80 || b > 0x9f) : (b < 0x80 || b > 0xbf))) return false;
                i += 3; continue;
            }
            if (a >= 0xf0 && a <= 0xf4) {
                if (i + 3 >= value.size()) return false;
                const auto b = static_cast<unsigned char>(value[i + 1]);
                const auto c = static_cast<unsigned char>(value[i + 2]);
                const auto d = static_cast<unsigned char>(value[i + 3]);
                if (b < 0x80 || b > 0xbf || c < 0x80 || c > 0xbf || d < 0x80 || d > 0xbf ||
                    (a == 0xf0 && b < 0x90) || (a == 0xf4 && b > 0x8f)) return false;
                i += 4; continue;
            }
            return false;
        }
        return true;
    }

    static bool one_scalar(const std::string& value) {
        if (value.empty() || !valid_utf8(value)) return false;
        const auto first = static_cast<unsigned char>(value[0]);
        const std::size_t width = first <= 0x7f ? 1 : first <= 0xdf ? 2 : first <= 0xef ? 3 : 4;
        return value.size() == width;
    }

public:
    ObservedPrimaryHomophoneIndex() : by_seed_(empty_table()) {}

    static const ObservedPrimaryHomophoneIndex& built_in() {
        static const ObservedPrimaryHomophoneIndex index = [] {
            std::istringstream input(std::string(generated::observed_primary_homophone_tsv));
            return parse(input);
        }();
        return index;
    }

    static ObservedPrimaryHomophoneIndex parse(std::istream& input) {
        std::string line;
        if (!std::getline(input, line) || line != "mode\tseed\tordinal\tcandidate_text")
            throw std::runtime_error("invalid observed homophone index header");

        ObservedPrimaryHomophoneIndex result;
        auto table = std::make_shared<Table>();
        std::set<std::string> empty_seeds;
        while (std::getline(input, line)) {
            if (line.find('\r') != std::string::npos)
                throw std::runtime_error("observed homophone index must use LF lines");
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (;;) {
                const auto tab = line.find('\t', start);
                fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (fields.size() != 4 || fields[0] != "traditional" || !one_scalar(fields[1]) ||
                !valid_utf8(fields[3]))
                throw std::runtime_error("invalid observed homophone row");
            const auto& seed = fields[1];
            const auto& ordinal_text = fields[2];
            if (ordinal_text.empty()) throw std::runtime_error("empty observed homophone ordinal");
            std::size_t ordinal = 0;
            for (char ch : ordinal_text) {
                if (ch < '0' || ch > '9' || ordinal > (static_cast<std::size_t>(-1) - 9) / 10)
                    throw std::runtime_error("invalid observed homophone ordinal");
                ordinal = ordinal * 10 + static_cast<std::size_t>(ch - '0');
            }
            auto found = table->find(seed);
            if (ordinal == 0) {
                if (!fields[3].empty() || found != table->end() || !empty_seeds.insert(seed).second)
                    throw std::runtime_error("invalid known-empty observed homophone row");
                table->emplace(seed, std::vector<std::string>{});
            } else {
                if (fields[3].empty() || empty_seeds.count(seed))
                    throw std::runtime_error("invalid observed homophone candidate row");
                auto& vector = (*table)[seed];
                if (ordinal != vector.size() + 1)
                    throw std::runtime_error("observed homophone ordinals must be contiguous");
                vector.push_back(fields[3]);
            }
        }
        if (input.bad() || (input.fail() && !input.eof()))
            throw std::runtime_error("failed while reading observed homophone index");
        result.by_seed_ = std::move(table);
        return result;
    }

    std::optional<std::vector<std::string>> lookup(const std::string& seed) const {
        const auto found = by_seed_->find(seed);
        if (found == by_seed_->end()) return std::nullopt;
        return found->second;
    }

    std::size_t size() const { return by_seed_->size(); }
};

template<class H3Index>
std::optional<std::vector<std::string>> observed_primary_or_h3(
    const ObservedPrimaryHomophoneIndex& observed, const H3Index& h3, const std::string& seed) {
    const auto captured = observed.lookup(seed);
    return captured ? captured : h3.primary_homophones(seed);
}

} // namespace boshiamy

#endif
