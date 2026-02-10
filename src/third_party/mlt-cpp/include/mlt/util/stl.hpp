#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace mlt::util {

/// Create a vector of N items by invoking the given function N times
template <typename T, typename F, typename I = std::size_t>
    requires requires(F f, I i) {
        { f(i) } -> std::same_as<T>;
    }
std::vector<T> generateVector(const std::size_t count, F generator) {
    std::vector<T> result;
    result.reserve(count);
    std::generate_n(
        std::back_inserter(result), count, [i = I{0}, f = std::move(generator)]() mutable { return f(i++); });
    return result;
}

// Helper for using lambdas with `std::variant`
// See https://en.cppreference.com/w/cpp/utility/variant/visit
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

// explicit deduction guide (not needed as of C++20)
// (but seems to be needed by MSVC)
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

} // namespace mlt::util
