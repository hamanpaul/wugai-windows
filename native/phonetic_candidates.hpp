#ifndef BOSHIAMY_PHONETIC_CANDIDATES_HPP
#define BOSHIAMY_PHONETIC_CANDIDATES_HPP

#include "cp950.hpp"

#include <algorithm>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>

namespace boshiamy {

// Immutable, ordered phonetic candidate data. A present reading with an empty
// vector is distinct from a reading that is absent from the index.
class PhoneticCandidateIndex {
    struct Data {
        std::map<std::string, std::vector<std::string>> ordered;
        std::map<std::string, std::vector<std::string>> primary_reverse;
    };
    std::shared_ptr<const Data> data_;

    explicit PhoneticCandidateIndex(std::shared_ptr<const Data> data)
        : data_(std::move(data)) {}

    static std::shared_ptr<const Data> empty_data() {
        static const auto value = std::make_shared<const Data>();
        return value;
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
                if (c < 0x80 || c > 0xbf || d < 0x80 || d > 0xbf ||
                    (a == 0xf0 ? (b < 0x90 || b > 0xbf) :
                     a == 0xf4 ? (b < 0x80 || b > 0x8f) : (b < 0x80 || b > 0xbf))) return false;
                i += 4; continue;
            }
            return false;
        }
        return true;
    }

public:
    PhoneticCandidateIndex() : data_(empty_data()) {}

    static PhoneticCandidateIndex parse(std::istream& input) {
        std::string line;
        if (!std::getline(input, line) || line != "reading\ttext")
            throw std::runtime_error("invalid phonetic candidate index header");

        auto data = std::make_shared<Data>();
        std::map<std::string, std::set<std::string>> seen;
        std::set<std::string> empty_readings;
        std::vector<std::string> source_reading_order;
        std::map<std::string, bool> cp950_eligible;
        while (std::getline(input, line)) {
            if (line.find('\r') != std::string::npos)
                throw std::runtime_error("phonetic candidate index must use LF lines");
            const auto tab = line.find('\t');
            if (tab == std::string::npos || line.find('\t', tab + 1) != std::string::npos)
                throw std::runtime_error("phonetic candidate index row must have two columns");
            const std::string reading = line.substr(0, tab);
            const std::string text = line.substr(tab + 1);
            if (reading.empty() || !valid_utf8(reading) || !valid_utf8(text))
                throw std::runtime_error("invalid UTF-8 or empty reading in phonetic candidate index");

            if (text.empty()) {
                if (data->ordered.count(reading) || !empty_readings.insert(reading).second)
                    throw std::runtime_error("duplicate or mixed empty phonetic candidate reading");
                data->ordered.emplace(reading, std::vector<std::string>{});
                source_reading_order.push_back(reading);
                continue;
            }
            if (empty_readings.count(reading) || !seen[reading].insert(text).second)
                throw std::runtime_error("duplicate or mixed empty phonetic candidate reading");
            if (!data->ordered.count(reading)) source_reading_order.push_back(reading);
            data->ordered[reading].push_back(text);
            if (!cp950_eligible.count(text))
                cp950_eligible.emplace(text, strict_cp950_roundtrip_utf8(text));
        }
        if (input.bad() || (input.fail() && !input.eof()))
            throw std::runtime_error("failed while reading phonetic candidate index");

        struct Membership {
            std::size_t seed_position;
            std::size_t source_order;
            const std::vector<std::string>* group;
        };
        std::map<std::string, std::vector<Membership>> memberships;
        for (std::size_t order = 0; order < source_reading_order.size(); ++order) {
            const auto& group = data->ordered.at(source_reading_order[order]);
            for (std::size_t position = 0; position < group.size(); ++position)
                memberships[group[position]].push_back({position, order, &group});
        }
        for (auto& entry : memberships) {
            const auto& seed = entry.first;
            auto& groups = entry.second;
            std::sort(groups.begin(), groups.end(), [](const Membership& a, const Membership& b) {
                if (a.seed_position != b.seed_position) return a.seed_position < b.seed_position;
                return a.source_order < b.source_order;
            });
            auto& result = data->primary_reverse[seed];
            for (const auto& membership : groups) {
                for (const auto& text : *membership.group) {
                    if (text != seed && cp950_eligible.at(text)) result.push_back(text);
                }
            }
        }
        return PhoneticCandidateIndex(std::move(data));
    }

    const std::vector<std::string>* lookup(const std::string& reading) const {
        const auto found = data_->ordered.find(reading);
        return found == data_->ordered.end() ? nullptr : &found->second;
    }

    std::optional<std::vector<std::string>> primary_homophones(const std::string& seed) const {
        const auto found = data_->primary_reverse.find(seed);
        if (found == data_->primary_reverse.end()) return std::nullopt;
        return found->second;
    }
};

} // namespace boshiamy

#endif
