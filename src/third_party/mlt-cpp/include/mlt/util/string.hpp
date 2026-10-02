#pragma once

#include <algorithm>
#include <ranges>
#include <string_view>

namespace mlt::util {

template <std::ranges::input_range Collection>
    requires requires(std::ranges::range_reference_t<const Collection> value) { std::string_view{value}; }
inline std::string_view longestCommonPrefix(const Collection& values) {
    auto it = std::ranges::begin(values);
    const auto end = std::ranges::end(values);

    // empty set, empty result
    if (it == end) {
        return {};
    }

    // one value, that's the LCP
    auto prefix = std::string_view{*it};
    ++it;
    if (it == end) {
        return prefix;
    }

    // truncate the prefix to match each additional value
    for (; it != end && !prefix.empty(); ++it) {
        const auto current = std::string_view{*it};
        const auto maxLen = std::min(prefix.size(), current.size());

        std::size_t i = 0;
        while (i < maxLen && prefix[i] == current[i]) {
            ++i;
        }
        prefix = prefix.substr(0, i);
    }

    return prefix;
}

} // namespace mlt::util
