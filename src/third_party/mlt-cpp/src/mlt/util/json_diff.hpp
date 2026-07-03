#pragma once
#include <cstddef>
#if MLT_WITH_JSON

#include <mlt/json.hpp>

#include <nlohmann/detail/string_concat.hpp>

#include <string>

namespace mlt::util {
using json = nlohmann::json;

// Comparison functor that can compare stringified numbers and consider precision
struct JSONComparer {
    const double doubleEpsilon = 1.0e-15;
    bool operator()(const json& left, const json& right) const {
        const auto leftDouble = getDouble(left);
        const auto rightDouble = getDouble(right);

        // Very similar numbers are equivalent, even if one or both are in string form
        if (leftDouble && rightDouble) {
            const auto md = (*leftDouble + *rightDouble) / 2;
            return (std::fabs(*leftDouble - *rightDouble) / ((md == 0) ? 1 : md)) < doubleEpsilon;
        }

        // Missing entries are equivalent to numeric zeros
        if ((left.is_null() && rightDouble == 0) || (right.is_null() && leftDouble == 0)) {
            return true;
        }

        // Empty objects and arrays are equivalent to missing entries
        if (((left.is_object() || left.is_array()) && left.empty() && right.is_null()) ||
            (left.is_null() && (right.is_object() || right.is_array()) && right.empty())) {
            return true;
        }
        return (left == right);
    }

private:
    static std::optional<double> getDouble(const json& v) {
        switch (v.type()) {
            case nlohmann::json_abi_v3_11_3::detail::value_t::null:
                return 0;
            case nlohmann::json_abi_v3_11_3::detail::value_t::number_integer:
            case nlohmann::json_abi_v3_11_3::detail::value_t::number_unsigned:
            case nlohmann::json_abi_v3_11_3::detail::value_t::number_float:
                return v.get<double>();
            case nlohmann::json_abi_v3_11_3::detail::value_t::string: {
                const auto s = v.get<std::string>();
                char* end = nullptr; // NOLINT(misc-const-correctness)
                const auto d = std::strtof(s.c_str(), &end);
                if (end - s.c_str() == static_cast<std::ptrdiff_t>(s.size())) {
                    return d;
                }
                return {}; // not a number
            }
            default:
                return {};
        }
    }
};

/// Based on `nlohmann:json::diff` but with a custom comparator and puts the old/expected value in the result
static json diff(const json& source,
                 const json& target,
                 const std::string& path = {},
                 const std::function<bool(const json&, const json&)>& compare = JSONComparer()) {
    using namespace nlohmann;
    using nlohmann::detail::concat;
    using nlohmann::detail::escape;

    // the patch
    json result(json::value_t::array);

    // if the values are the same, return empty patch
    if (compare(source, target)) {
        return result;
    }

    if (source.type() != target.type()) {
        // different types: replace value
        result.push_back({{"op", "replace"}, {"path", path}, {"value", target}, {"original", source}});
        return result;
    }

    switch (source.type()) {
        case json::value_t::array: {
            // first pass: traverse common elements
            std::size_t i = 0;
            while (i < source.size() && i < target.size()) {
                // recursive call to compare array values at index i
                auto temp_diff = diff(source[i], target[i], concat(path, '/', std::to_string(i)));
                result.insert(result.end(), temp_diff.begin(), temp_diff.end());
                ++i;
            }

            // We now reached the end of at least one array
            // in a second pass, traverse the remaining elements

            // remove my remaining elements
            const auto end_index = static_cast<json::difference_type>(result.size());
            while (i < source.size()) {
                // add operations in reverse order to avoid invalid
                // indices
                result.insert(
                    result.begin() + end_index,
                    json::object(
                        {{"op", "remove"}, {"path", concat(path, '/', std::to_string(i))}, {"value", source[i]}}));
                ++i;
            }

            // add other remaining elements
            while (i < target.size()) {
                result.push_back({{"op", "add"}, {"path", concat(path, "/-")}, {"value", target[i]}});
                ++i;
            }

            break;
        }

        case json::value_t::object: {
            // first pass: traverse this object's elements
            for (auto it = source.cbegin(); it != source.cend(); ++it) {
                // escape the key name to be used in a JSON patch
                const auto path_key = concat(path, '/', escape(it.key()));

                if (target.contains(it.key())) {
                    // recursive call to compare object values at key it
                    auto temp_diff = diff(it.value(), target[it.key()], path_key);
                    result.insert(result.end(), temp_diff.begin(), temp_diff.end());
                } else {
                    // found a key that is not in o.
                    // If the value is equivalent to nothing, that's not a difference.
                    const auto& sourceValue = source[it.key()];
                    if (!compare(sourceValue, json())) {
                        result.push_back(json::object({{"op", "remove"}, {"path", path_key}, {"value", sourceValue}}));
                    }
                }
            }

            // second pass: traverse other object's elements
            for (auto it = target.cbegin(); it != target.cend(); ++it) {
                if (source.contains(it.key())) {
                    // found a key that is not in this
                    // If the value is equivalent to nothing, that's not a difference.
                    if (!compare(json(), *it)) {
                        const auto path_key = concat(path, '/', escape(it.key()));
                        result.push_back({{"op", "add"}, {"path", path_key}, {"value", *it}});
                    }
                }
            }

            break;
        }

        case json::value_t::null:
        case json::value_t::string:
        case json::value_t::boolean:
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
        case json::value_t::binary:
        case json::value_t::discarded:
        default: {
            // both primitive type: replace value
            result.push_back({{"op", "replace"}, {"path", path}, {"value", target}, {"original", source}});
            break;
        }
    }

    return result;
}

} // namespace mlt::util
#endif // MLT_WITH_JSON
