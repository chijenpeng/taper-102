#pragma once

#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace taper::ui
{
inline bool asciiSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && asciiSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && asciiSpace(text.back())) text.remove_suffix(1);
    return text;
}

// Complete finite decimal number, with an optional dB suffix. The caller applies
// the parameter's range; failure does not modify result. Parsing is locale-independent.
inline bool parseDb(std::string_view input, double& result)
{
    auto text = trim(input);
    if (text.size() >= 2 && (text[text.size() - 2] == 'd' || text[text.size() - 2] == 'D')
        && (text.back() == 'b' || text.back() == 'B'))
        text = trim(text.substr(0, text.size() - 2));

    std::string number(text);
    if (number.compare(0, 3, "\xE2\x88\x92") == 0)
        number.replace(0, 3, "-");
    if (number.empty()) return false;

    std::size_t position = 0;
    if (number[position] == '+' || number[position] == '-') ++position;
    bool hasDigit = false;
    const auto digits = [&]()
    {
        while (position < number.size() && number[position] >= '0' && number[position] <= '9')
        {
            hasDigit = true;
            ++position;
        }
    };
    digits();
    if (position < number.size() && number[position] == '.') { ++position; digits(); }
    if (!hasDigit) return false;
    if (position < number.size() && (number[position] == 'e' || number[position] == 'E'))
    {
        ++position;
        if (position < number.size() && (number[position] == '+' || number[position] == '-')) ++position;
        const auto exponentStart = position;
        while (position < number.size() && number[position] >= '0' && number[position] <= '9') ++position;
        if (position == exponentStart) return false;
    }
    if (position != number.size()) return false;

    std::istringstream stream(number);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (!stream || !stream.eof() || !std::isfinite(value)) return false;
    result = value;
    return true;
}
}
