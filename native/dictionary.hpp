#ifndef LIUKAI_DICTIONARY_HPP
#define LIUKAI_DICTIONARY_HPP
#include "engine.hpp"
#include <array>
#include <istream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace boshiamy {
inline bool read_tsv_record(std::istream& input, std::vector<std::string>& fields) {
    fields.clear();
    std::string field;
    bool quoted = false, closed = false, any = false;
    for (;;) {
        int next = input.get();
        if (next == std::char_traits<char>::eof()) {
            if (quoted) throw std::runtime_error("unterminated quoted TSV field");
            if (!any) return false;
            fields.push_back(field);
            return true;
        }
        any = true;
        char ch = static_cast<char>(next);
        if (quoted) {
            if (ch == '"') {
                if (input.peek() == '"') { input.get(); field.push_back('"'); }
                else { quoted = false; closed = true; }
            } else field.push_back(ch);
        } else if (ch == '\t') {
            fields.push_back(field); field.clear(); closed = false;
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && input.peek() == '\n') input.get();
            fields.push_back(field); return true;
        } else if (closed) throw std::runtime_error("characters after quoted TSV field");
        else if (ch == '"' && field.empty()) quoted = true;
        else if (ch == '"') throw std::runtime_error("unexpected quote in TSV field");
        else field.push_back(ch);
    }
}

struct Dictionary {
    std::map<Mode, std::map<std::string, std::vector<std::string>>> sections;
    std::size_t entries = 0;
    Engine make_engine() const {
        Engine engine;
        for (const auto& section : sections)
            for (const auto& code : section.second)
                engine.add(code.first, code.second, section.first);
        return engine;
    }
};

inline Dictionary load_dictionary(std::istream& input) {
    std::vector<std::string> columns;
    if (!read_tsv_record(input, columns) || columns != std::vector<std::string>{"section", "code", "text", "freq", "order"})
        throw std::runtime_error("unsupported dictionary TSV header");
    const std::map<std::string, Mode> modes = {
        {"TRADITIONAL", Mode::Traditional}, {"SIMPLIFIED", Mode::Simplified},
        {"TW_SIMPLIFIED", Mode::TradToSimplified}, {"JAPANESE", Mode::Japanese}};
    Dictionary result;
    std::size_t number = 1;
    while (read_tsv_record(input, columns)) {
        ++number;
        if (columns.size() != 5)
            throw std::runtime_error("invalid dictionary columns at row " + std::to_string(number));
        auto mode = modes.find(columns[0]);
        if (mode == modes.end() || columns[1].empty() || columns[2].empty())
            throw std::runtime_error("invalid dictionary entry at row " + std::to_string(number));
        for (std::size_t col = 3; col < 5; ++col) {
            std::size_t consumed = 0;
            try {
                if (columns[col].empty() || columns[col][0] == '-') throw std::invalid_argument("negative or empty");
                std::stoull(columns[col], &consumed);
                if (consumed != columns[col].size()) throw std::invalid_argument("trailing characters");
            } catch (const std::exception&) {
                throw std::runtime_error("invalid dictionary integer at row " + std::to_string(number));
            }
        }
        result.sections[mode->second][columns[1]].push_back(columns[2]);
        ++result.entries;
    }
    if (!input.eof()) throw std::runtime_error("dictionary input read failed");
    return result;
}
} // namespace boshiamy
#endif
