#ifndef BOSHIAMY_READINGS_HPP
#define BOSHIAMY_READINGS_HPP

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace boshiamy {

class Readings {
    struct Entry {
        std::vector<std::string> readings;
        std::optional<int> grade;
    };
    struct Data {
        std::map<std::string, Entry> entries;
        std::map<std::string, std::set<std::string>> primary_index;
        std::map<std::string, std::set<std::string>> reading_index;
    };

    std::shared_ptr<const Data> data_;

    explicit Readings(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

    static std::shared_ptr<const Data> empty_data() {
        static const auto value = std::make_shared<const Data>();
        return value;
    }

    static bool valid_utf8(const std::string& text) {
        std::size_t i = 0;
        while (i < text.size()) {
            const unsigned char a = static_cast<unsigned char>(text[i]);
            if (a <= 0x7f) { ++i; continue; }
            if (a >= 0xc2 && a <= 0xdf) {
                if (i + 1 >= text.size()) return false;
                const unsigned char b = static_cast<unsigned char>(text[i + 1]);
                if (b < 0x80 || b > 0xbf) return false;
                i += 2; continue;
            }
            if (a >= 0xe0 && a <= 0xef) {
                if (i + 2 >= text.size()) return false;
                const unsigned char b = static_cast<unsigned char>(text[i + 1]);
                const unsigned char c = static_cast<unsigned char>(text[i + 2]);
                if (c < 0x80 || c > 0xbf) return false;
                if (a == 0xe0 ? (b < 0xa0 || b > 0xbf)
                    : a == 0xed ? (b < 0x80 || b > 0x9f)
                    : (b < 0x80 || b > 0xbf)) return false;
                i += 3; continue;
            }
            if (a >= 0xf0 && a <= 0xf4) {
                if (i + 3 >= text.size()) return false;
                const unsigned char b = static_cast<unsigned char>(text[i + 1]);
                const unsigned char c = static_cast<unsigned char>(text[i + 2]);
                const unsigned char d = static_cast<unsigned char>(text[i + 3]);
                if (c < 0x80 || c > 0xbf || d < 0x80 || d > 0xbf) return false;
                if (a == 0xf0 ? (b < 0x90 || b > 0xbf)
                    : a == 0xf4 ? (b < 0x80 || b > 0x8f)
                    : (b < 0x80 || b > 0xbf)) return false;
                i += 4; continue;
            }
            return false;
        }
        return true;
    }

    static bool one_unicode_scalar(const std::string& text) {
        if (text.empty() || !valid_utf8(text)) return false;
        const unsigned char first = static_cast<unsigned char>(text.front());
        const std::size_t expected = first <= 0x7f ? 1 : first <= 0xdf ? 2 : first <= 0xef ? 3 : 4;
        return text.size() == expected;
    }

    static std::optional<int> parse_grade(const std::string& text) {
        if (text.empty()) return std::nullopt;
        char* end = nullptr;
        errno = 0;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
            value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            return std::nullopt;
        return static_cast<int>(value);
    }

public:
    Readings() : data_(empty_data()) {}

    static Readings parse(std::istream& input) {
        auto data = std::make_shared<Data>();
        std::string line;
        bool first_line = true;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first_line) {
                first_line = false;
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
                    static_cast<unsigned char>(line[1]) == 0xbb && static_cast<unsigned char>(line[2]) == 0xbf)
                    line.erase(0, 3);
            }
            std::size_t nonspace = 0;
            while (nonspace < line.size() && std::isspace(static_cast<unsigned char>(line[nonspace]))) ++nonspace;
            if (nonspace == line.size() || line[nonspace] == '#') continue;

            const std::size_t first_tab = line.find('\t');
            if (first_tab == std::string::npos) continue;
            const std::size_t second_tab = line.find('\t', first_tab + 1);
            const std::string text = line.substr(0, first_tab);
            const std::string reading_field = line.substr(first_tab + 1,
                second_tab == std::string::npos ? std::string::npos : second_tab - first_tab - 1);
            if (!one_unicode_scalar(text)) continue;

            std::vector<std::string> readings;
            std::size_t pos = 0;
            while (pos < reading_field.size()) {
                while (pos < reading_field.size() && std::isspace(static_cast<unsigned char>(reading_field[pos]))) ++pos;
                const std::size_t start = pos;
                while (pos < reading_field.size() && !std::isspace(static_cast<unsigned char>(reading_field[pos]))) ++pos;
                if (start < pos) {
                    std::string reading = reading_field.substr(start, pos - start);
                    if (!valid_utf8(reading)) { readings.clear(); break; }
                    readings.push_back(std::move(reading));
                }
            }
            if (readings.empty()) continue;

            std::optional<int> grade;
            if (second_tab != std::string::npos) {
                const std::size_t third_tab = line.find('\t', second_tab + 1);
                std::string grade_field = line.substr(second_tab + 1,
                    third_tab == std::string::npos ? std::string::npos : third_tab - second_tab - 1);
                const auto begin = grade_field.find_first_not_of(" \t");
                const auto end = grade_field.find_last_not_of(" \t");
                if (begin != std::string::npos) grade = parse_grade(grade_field.substr(begin, end - begin + 1));
            }

            auto old = data->entries.find(text);
            if (old != data->entries.end()) {
                const std::string& old_primary = old->second.readings.front();
                auto group = data->primary_index.find(old_primary);
                if (group != data->primary_index.end()) {
                    group->second.erase(text);
                    if (group->second.empty()) data->primary_index.erase(group);
                }
                for (const auto& old_reading : old->second.readings) {
                    auto matches = data->reading_index.find(old_reading);
                    if (matches != data->reading_index.end()) {
                        matches->second.erase(text);
                        if (matches->second.empty()) data->reading_index.erase(matches);
                    }
                }
            }
            const std::string primary = readings.front();
            data->entries[text] = Entry{readings, grade};
            data->primary_index[primary].insert(text);
            for (const auto& reading : readings) data->reading_index[reading].insert(text);
        }
        if (input.bad() || (input.fail() && !input.eof()))
            throw std::runtime_error("Failed to read readings stream");
        if (data->entries.empty()) return Readings();
        return Readings(std::move(data));
    }

    std::vector<std::string> of(const std::string& text) const {
        auto it = data_->entries.find(text);
        return it == data_->entries.end() ? std::vector<std::string>{} : it->second.readings;
    }

    std::vector<std::string> primary_matches(const std::string& seed) const {
        auto entry = data_->entries.find(seed);
        if (entry == data_->entries.end() || entry->second.readings.empty()) return {};
        auto it = data_->primary_index.find(entry->second.readings.front());
        if (it == data_->primary_index.end()) return {};
        return {it->second.begin(), it->second.end()};
    }

    std::vector<std::string> matches_reading(const std::string& reading) const {
        auto it = data_->reading_index.find(reading);
        if (it == data_->reading_index.end()) return {};
        return {it->second.begin(), it->second.end()};
    }

    std::optional<int> grade(const std::string& text) const {
        auto it = data_->entries.find(text);
        return it == data_->entries.end() ? std::nullopt : it->second.grade;
    }

    bool empty() const { return data_->entries.empty(); }
    std::size_t size() const { return data_->entries.size(); }
};

}
#endif
